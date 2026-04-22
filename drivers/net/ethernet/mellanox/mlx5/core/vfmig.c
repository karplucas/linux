// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * mlx5 host-driver-side VF migration / CRIU restore - control plane.
 * See vfmig.h for the architecture overview.
 *
 * Lifetime model
 * --------------
 * One struct mlx5_vfmig_pf is created per PF mlx5_core in mlx5_vfmig_pf_init()
 * and destroyed in mlx5_vfmig_pf_cleanup(). It owns:
 *   - a cdev under /dev/mlx5_vfmig/<bdf>
 *   - a back-pointer to the PF mlx5_core_dev
 *
 * Userspace can hold the cdev open across PF unbind. cdev_del() does NOT wait
 * for in-flight callers, so we use:
 *   - kref:   keeps the struct alive while any fd or ioctl holds a reference.
 *             Initial ref taken in pf_init(), released in pf_cleanup().
 *             open() takes a ref, release() drops it.
 *   - dead:   set under vfmig->lock at pf_cleanup() time. Once set, the
 *             struct's pf_mdev back-pointer must not be dereferenced because
 *             mlx5_uninit_one() is about to (or already has) freed the mdev.
 *             ioctl handlers take the rwsem for reading and bail with -ENODEV
 *             if dead; pf_cleanup() takes it for writing once.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <uapi/linux/mlx5_vfmig.h>

#include "mlx5_core.h"
#include "vfmig.h"

#define MLX5_VFMIG_MAX_DEVICES	256

/* Module-wide cdev region; one minor per PF mlx5_core. */
static dev_t mlx5_vfmig_devt;
static struct class *mlx5_vfmig_class;
static DEFINE_IDA(mlx5_vfmig_minor_ida);

/* Per-PF state attached to mlx5_priv via .vfmig opaque pointer. */
struct mlx5_vfmig_pf {
	struct kref kref;
	struct rw_semaphore lock;	/* protects pf_mdev / dead */
	struct mlx5_core_dev *pf_mdev;	/* NULL once dead */
	bool dead;
	struct cdev cdev;
	int minor;
};

static void vfmig_pf_release(struct kref *kref)
{
	struct mlx5_vfmig_pf *vfmig =
		container_of(kref, struct mlx5_vfmig_pf, kref);

	ida_free(&mlx5_vfmig_minor_ida, vfmig->minor);
	kfree(vfmig);
}

static void vfmig_pf_get(struct mlx5_vfmig_pf *vfmig)
{
	kref_get(&vfmig->kref);
}

static void vfmig_pf_put(struct mlx5_vfmig_pf *vfmig)
{
	kref_put(&vfmig->kref, vfmig_pf_release);
}

/* -------- ioctl handlers ------------------------------------------------- */

/*
 * QUERY_HCA_CAP(other_function=1) - PF-side query of a VF's vhca_id.
 * Mirrors mlx5vf_cmd_get_vhca_id() in drivers/vfio/pci/mlx5/cmd.c. We
 * keep our own copy in M1' to avoid coupling mlx5_core to the vfio
 * variant driver; M2 will hoist the shared version into mlx5_core.
 */
static int vfmig_query_vhca_id(struct mlx5_core_dev *pf_mdev,
			       u16 function_id, u16 *vhca_id)
{
	u32 in[MLX5_ST_SZ_DW(query_hca_cap_in)] = {};
	void *out;
	int out_size;
	int ret;

	out_size = MLX5_ST_SZ_BYTES(query_hca_cap_out);
	out = kzalloc(out_size, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	MLX5_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	MLX5_SET(query_hca_cap_in, in, other_function, 1);
	MLX5_SET(query_hca_cap_in, in, function_id, function_id);
	MLX5_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE << 1 |
		 HCA_CAP_OPMOD_GET_CUR);

	ret = mlx5_cmd_exec_inout(pf_mdev, query_hca_cap, in, out);
	if (ret)
		goto out;

	*vhca_id = MLX5_GET(query_hca_cap_out, out,
			    capability.cmd_hca_cap.vhca_id);
out:
	kfree(out);
	return ret;
}

static long vfmig_ioc_mark_restored(struct mlx5_vfmig_pf *vfmig,
				    void __user *uarg)
{
	struct mlx5_vfmig_mark_restored arg;
	struct mlx5_core_sriov *sriov;
	u16 vhca_id;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;

	/*
	 * sriov->num_vfs is mutated under the PF's intf_state_mutex on
	 * SR-IOV enable/disable. We read it without that mutex; a torn read
	 * is benign because vfs_ctx[] is freed by sriov_disable only after
	 * num_vfs is cleared, and we are protected from PF unbind by the
	 * vfmig->lock held by the caller.
	 */
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	if (sriov->vfs_ctx[arg.vf_id].restored)
		return -EALREADY;

	/*
	 * Capture vhca_id now so that the probe-time skip log can identify
	 * the firmware vHCA without issuing a fresh QUERY_HCA_CAP from the
	 * VF's own probe context (where the PF mdev pointer dance is
	 * cheaper to avoid). function_id == vf_id + 1; function_id 0 is the
	 * PF itself.
	 */
	err = vfmig_query_vhca_id(vfmig->pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	sriov->vfs_ctx[arg.vf_id].restored_vhca_id = vhca_id;
	sriov->vfs_ctx[arg.vf_id].restored = 1;
	mlx5_core_info(vfmig->pf_mdev,
		       "vfmig: marked VF %u (vhca_id 0x%04x) as restored (next probe will skip INIT_HCA)\n",
		       arg.vf_id, vhca_id);
	return 0;
}

static long vfmig_ioc_get_vhca_id(struct mlx5_vfmig_pf *vfmig,
				  void __user *uarg)
{
	struct mlx5_vfmig_get_vhca_id arg;
	struct mlx5_core_sriov *sriov;
	u16 vhca_id;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	err = vfmig_query_vhca_id(vfmig->pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	arg.vhca_id = vhca_id;
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;
	return 0;
}

static long vfmig_ioc_query_vf(struct mlx5_vfmig_pf *vfmig,
			       void __user *uarg)
{
	struct mlx5_vfmig_query_vf arg;
	struct mlx5_core_sriov *sriov;
	u16 vhca_id;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;

	/*
	 * Always return num_vfs so a caller probing a stale vf_id can
	 * recover and re-iterate without an extra round trip.
	 */
	arg.num_vfs = sriov->num_vfs;
	if (arg.vf_id >= sriov->num_vfs) {
		arg.vhca_id = 0;
		arg.restored = 0;
		if (copy_to_user(uarg, &arg, sizeof(arg)))
			return -EFAULT;
		return -ERANGE;
	}

	err = vfmig_query_vhca_id(vfmig->pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	arg.vhca_id = vhca_id;
	arg.restored = sriov->vfs_ctx[arg.vf_id].restored;
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;
	return 0;
}

/* -------- file ops ------------------------------------------------------- */

static int vfmig_open(struct inode *inode, struct file *filp)
{
	struct mlx5_vfmig_pf *vfmig =
		container_of(inode->i_cdev, struct mlx5_vfmig_pf, cdev);

	vfmig_pf_get(vfmig);
	filp->private_data = vfmig;
	return 0;
}

static int vfmig_release(struct inode *inode, struct file *filp)
{
	struct mlx5_vfmig_pf *vfmig = filp->private_data;

	vfmig_pf_put(vfmig);
	return 0;
}

static long vfmig_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mlx5_vfmig_pf *vfmig = filp->private_data;
	void __user *uarg = (void __user *)arg;
	long ret;

	/*
	 * Block PF unbind from racing with us, and reject calls that
	 * arrive after the PF is gone.
	 */
	down_read(&vfmig->lock);
	if (vfmig->dead) {
		ret = -ENODEV;
		goto out;
	}

	switch (cmd) {
	case MLX5_VFMIG_IOC_MARK_RESTORED:
		ret = vfmig_ioc_mark_restored(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_GET_VHCA_ID:
		ret = vfmig_ioc_get_vhca_id(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_QUERY_VF:
		ret = vfmig_ioc_query_vf(vfmig, uarg);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
out:
	up_read(&vfmig->lock);
	return ret;
}

static const struct file_operations mlx5_vfmig_fops = {
	.owner		= THIS_MODULE,
	.open		= vfmig_open,
	.release	= vfmig_release,
	.unlocked_ioctl	= vfmig_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

/* -------- per-PF init / cleanup ----------------------------------------- */

int mlx5_vfmig_pf_init(struct mlx5_core_dev *pf_mdev)
{
	struct mlx5_vfmig_pf *vfmig;
	struct device *dev;
	dev_t devno;
	int minor, err;

	if (mlx5_core_is_vf(pf_mdev))
		return 0;

	vfmig = kzalloc(sizeof(*vfmig), GFP_KERNEL);
	if (!vfmig)
		return -ENOMEM;

	kref_init(&vfmig->kref);
	init_rwsem(&vfmig->lock);
	vfmig->pf_mdev = pf_mdev;

	minor = ida_alloc_max(&mlx5_vfmig_minor_ida,
			      MLX5_VFMIG_MAX_DEVICES - 1, GFP_KERNEL);
	if (minor < 0) {
		err = minor;
		goto err_free;
	}
	vfmig->minor = minor;
	devno = MKDEV(MAJOR(mlx5_vfmig_devt), minor);

	cdev_init(&vfmig->cdev, &mlx5_vfmig_fops);
	vfmig->cdev.owner = THIS_MODULE;

	err = cdev_add(&vfmig->cdev, devno, 1);
	if (err)
		goto err_minor;

	/*
	 * "mlx5_vfmig!<bdf>" -> /dev/mlx5_vfmig/<bdf> via udev's '!' rule.
	 * Pass vfmig as drvdata so future tooling can dev_get_drvdata() it.
	 */
	dev = device_create(mlx5_vfmig_class, pf_mdev->device, devno, vfmig,
			    "mlx5_vfmig!%s", dev_name(pf_mdev->device));
	if (IS_ERR(dev)) {
		err = PTR_ERR(dev);
		goto err_cdev;
	}

	pf_mdev->priv.vfmig = vfmig;
	mlx5_core_info(pf_mdev, "vfmig: cdev /dev/mlx5_vfmig/%s ready\n",
		       dev_name(pf_mdev->device));
	return 0;

err_cdev:
	cdev_del(&vfmig->cdev);
err_minor:
	ida_free(&mlx5_vfmig_minor_ida, minor);
err_free:
	kfree(vfmig);
	return err;
}

void mlx5_vfmig_pf_cleanup(struct mlx5_core_dev *pf_mdev)
{
	struct mlx5_vfmig_pf *vfmig = pf_mdev->priv.vfmig;
	dev_t devno;

	if (!vfmig)
		return;

	pf_mdev->priv.vfmig = NULL;
	devno = MKDEV(MAJOR(mlx5_vfmig_devt), vfmig->minor);

	/*
	 * Neuter the device first: any ioctl already inside vfmig_ioctl()
	 * (under down_read of vfmig->lock) will finish; subsequent ioctls
	 * will see ->dead and return -ENODEV without touching pf_mdev.
	 */
	down_write(&vfmig->lock);
	vfmig->dead = true;
	vfmig->pf_mdev = NULL;
	up_write(&vfmig->lock);

	/*
	 * Hide from /dev (no new opens) and from chrdev lookup. cdev_del()
	 * does not wait for already-open fds; vfmig_release() drops the
	 * remaining refs and the kref's release callback will free us.
	 */
	device_destroy(mlx5_vfmig_class, devno);
	cdev_del(&vfmig->cdev);

	vfmig_pf_put(vfmig);
}

/* -------- VF probe-time hook -------------------------------------------- */

bool mlx5_vfmig_vf_consume_restored(struct mlx5_core_dev *dev, u16 *vhca_id_out)
{
	struct pci_dev *vf_pdev = dev->pdev;
	struct mlx5_core_dev *pf_mdev;
	struct mlx5_core_sriov *sriov;
	bool restored = false;
	int vf_id;

	if (!vf_pdev || !vf_pdev->is_virtfn)
		return false;

	vf_id = pci_iov_vf_id(vf_pdev);
	if (vf_id < 0)
		return false;

	/*
	 * Takes the PF's intf_state_mutex. Safe from VF probe because the
	 * PF and VF mdevs are distinct objects with separate lockdep keys
	 * (see lockdep_register_key in mlx5_mdev_init).
	 */
	pf_mdev = mlx5_vf_get_core_dev(vf_pdev);
	if (!pf_mdev)
		return false;

	sriov = &pf_mdev->priv.sriov;
	if (vf_id < sriov->num_vfs && sriov->vfs_ctx[vf_id].restored) {
		if (vhca_id_out)
			*vhca_id_out = sriov->vfs_ctx[vf_id].restored_vhca_id;
		sriov->vfs_ctx[vf_id].restored_vhca_id = 0;
		sriov->vfs_ctx[vf_id].restored = 0;
		restored = true;
	}
	mlx5_vf_put_core_dev(pf_mdev);

	return restored;
}

/* -------- module init/exit ---------------------------------------------- */

int mlx5_vfmig_module_init(void)
{
	int err;

	err = alloc_chrdev_region(&mlx5_vfmig_devt, 0,
				  MLX5_VFMIG_MAX_DEVICES, "mlx5_vfmig");
	if (err)
		return err;

	mlx5_vfmig_class = class_create("mlx5_vfmig");
	if (IS_ERR(mlx5_vfmig_class)) {
		err = PTR_ERR(mlx5_vfmig_class);
		unregister_chrdev_region(mlx5_vfmig_devt,
					 MLX5_VFMIG_MAX_DEVICES);
		return err;
	}

	return 0;
}

void mlx5_vfmig_module_exit(void)
{
	class_destroy(mlx5_vfmig_class);
	unregister_chrdev_region(mlx5_vfmig_devt, MLX5_VFMIG_MAX_DEVICES);
	ida_destroy(&mlx5_vfmig_minor_ida);
}
