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
 *   - a list of open LOAD_VHCA_STATE sessions (mlx5_vfmig_load_ctx)
 *
 * Userspace can hold the cdev (or any LOAD anon-inode fd) open across PF
 * unbind. cdev_del() does NOT wait for in-flight callers, so we use:
 *   - kref:    keeps the struct alive while any fd or ioctl holds a
 *              reference. Initial ref taken in pf_init(), released in
 *              pf_cleanup(). cdev open() takes a ref, cdev release()
 *              drops it. Each LOAD session also takes a ref for the
 *              lifetime of its anon-inode fd.
 *   - lock:    rwsem protecting pf_mdev / dead. ioctl handlers and load
 *              fd .write handlers down_read() and bail with -ENODEV if
 *              dead. pf_cleanup() down_write()s once to neuter the cdev
 *              and synchronously tear down LOAD-session firmware
 *              resources before pf_mdev is freed by mlx5_uninit_one().
 *   - load_ctxs_lock: mutex protecting load_ctxs list mutations. Held
 *              over open's "claim vf_id + list_add" and release's
 *              list_del. Never held while invoking fput().
 *
 * Lock order:
 *      vfmig->lock  ->  vfmig->load_ctxs_lock  ->  ctx->io_lock
 * pf_cleanup holds vfmig->lock for write, which blocks all readers; the
 * load_ctxs walk inside pf_cleanup therefore needs no further locking.
 */

#include <linux/anon_inodes.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mlx5/device.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/mlx5_ifc.h>
#include <uapi/linux/mlx5_vfmig.h>

#include "mlx5_core.h"
#include "vfmig.h"

#define MLX5_VFMIG_MAX_DEVICES	256

/*
 * Hardware-imposed maximum bytes per single LOAD_VHCA_STATE command.
 * Mirrors MAX_LOAD_SIZE in drivers/vfio/pci/mlx5/main.c. Records larger
 * than this in the input blob are rejected; userspace must split them.
 */
#define VFMIG_MAX_LOAD_SIZE \
	(BIT_ULL(__mlx5_bit_sz(load_vhca_state_in, size)) - 1)

/*
 * Wire-format header: byte-compatible with the VFIO mlx5 variant driver's
 * migration stream (drivers/vfio/pci/mlx5/cmd.h:mlx5_vf_migration_header).
 * Duplicated here because the original is not exported as UAPI.
 *
 * TODO(vfmig-dedup): once we lift the shared helpers into mlx5_core, this
 * type should be promoted alongside them and the VFIO variant should
 * consume the shared definition.
 */
struct vfmig_wire_header {
	__le64 record_size;
	__le32 flags;
	__le32 tag;
};

/* Mirror MLX5_MIGF_HEADER_TAG_* / FLAGS_TAG_OPTIONAL from the VFIO driver. */
#define VFMIG_WIRE_TAG_FW_DATA		0
#define VFMIG_WIRE_TAG_STOP_COPY_SIZE	1
#define VFMIG_WIRE_FLAGS_TAG_OPTIONAL	BIT(0)

/* Module-wide cdev region; one minor per PF mlx5_core. */
static dev_t mlx5_vfmig_devt;
static struct class *mlx5_vfmig_class;
static DEFINE_IDA(mlx5_vfmig_minor_ida);

struct mlx5_vfmig_load_ctx;

/* Per-PF state attached to mlx5_priv via .vfmig opaque pointer. */
struct mlx5_vfmig_pf {
	struct kref kref;
	struct rw_semaphore lock;	/* protects pf_mdev / dead */
	struct mlx5_core_dev *pf_mdev;	/* NULL once dead */
	bool dead;
	struct cdev cdev;
	int minor;

	struct mutex load_ctxs_lock;	/* protects load_ctxs list */
	struct list_head load_ctxs;	/* of struct mlx5_vfmig_load_ctx */
};

/*
 * Parser FSM for the load fd. Mirrors the VFIO variant's
 * MLX5_VF_LOAD_STATE_* enum in drivers/vfio/pci/mlx5/cmd.h.
 */
enum vfmig_load_state {
	VFMIG_LS_READ_HEADER = 0,
	VFMIG_LS_READ_HEADER_DATA,
	VFMIG_LS_PREP_IMAGE,
	VFMIG_LS_READ_IMAGE,
	VFMIG_LS_LOAD_IMAGE,
};

/*
 * Per-LOAD-fd context. Hung off vfmig_pf->load_ctxs. The fd's private_data
 * points here; lifetime is tied to the fd.
 */
struct mlx5_vfmig_load_ctx {
	struct list_head node;		/* on vfmig->load_ctxs */
	struct mlx5_vfmig_pf *vfmig;	/* holds a kref; never NULL once
					 * load_ctx exists and is on the list
					 */
	struct mutex io_lock;		/* serializes concurrent write()s */
	u32 vf_id;
	u16 vhca_id;

	/* Resources tied to the PF mdev. Released by pf_cleanup or release.
	 * Mutated under vfmig->lock (read suffices: pf_cleanup takes write).
	 */
	bool resources_freed;
	bool pd_allocated;
	u32 pdn;

	/* Image staging buffer, sized to the largest record we've seen so
	 * far. Reallocated under vfmig->lock-read when a record exceeds it.
	 */
	struct page **image_pages;
	u32 image_npages;	/* allocated capacity, in PAGE_SIZE units */
	u64 image_filled;	/* bytes accumulated in current record */
	u32 *image_mkey_in;	/* alloc_mkey_in() buffer; NULL if no MKEY */
	u32 image_mkey;
	bool image_mkey_created;
	bool image_dma_mapped;
	struct dma_iova_state image_dma_state;

	/* Parser scratch */
	enum vfmig_load_state state;
	u8  hdr_buf[sizeof(struct vfmig_wire_header)];
	u32 hdr_buf_filled;
	u64 record_size;	/* current record's payload size */
	u32 record_tag;		/* current record's tag */
	u64 record_skipped;	/* bytes consumed-and-discarded for this rec */
};

static void vfmig_load_release_resources(struct mlx5_vfmig_load_ctx *ctx);

static void vfmig_pf_release(struct kref *kref)
{
	struct mlx5_vfmig_pf *vfmig =
		container_of(kref, struct mlx5_vfmig_pf, kref);

	WARN_ON(!list_empty(&vfmig->load_ctxs));
	mutex_destroy(&vfmig->load_ctxs_lock);
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
 * Mirrors mlx5vf_cmd_get_vhca_id() in drivers/vfio/pci/mlx5/cmd.c.
 *
 * TODO(vfmig-dedup): hoist into mlx5_core proper and let the VFIO variant
 * call it instead of carrying its own copy.
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

	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	if (sriov->vfs_ctx[arg.vf_id].restored)
		return -EALREADY;

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

/* -------- LOAD_VHCA_STATE: helpers cloned from VFIO mlx5 variant -------- */

/*
 * The four helpers below (alloc_mkey_in, create_mkey, register_dma_pages,
 * unregister_dma_pages) are line-for-line copies of the static helpers in
 * drivers/vfio/pci/mlx5/cmd.c (alloc_mkey_in @316, create_mkey @348,
 * register_dma_pages @378, unregister_dma_pages @357). We duplicate them
 * here so M2 doesn't need to touch the VFIO variant or invent an export
 * boundary; a future patch should hoist them into mlx5_core proper.
 *
 * TODO(vfmig-dedup): collapse with drivers/vfio/pci/mlx5/cmd.c counterparts.
 */
static u32 *vfmig_alloc_mkey_in(u32 npages, u32 pdn)
{
	int inlen;
	void *mkc;
	u32 *in;

	inlen = MLX5_ST_SZ_BYTES(create_mkey_in) +
		sizeof(__be64) * round_up(npages, 2);

	in = kvzalloc(inlen, GFP_KERNEL_ACCOUNT);
	if (!in)
		return NULL;

	MLX5_SET(create_mkey_in, in, translations_octword_actual_size,
		 DIV_ROUND_UP(npages, 2));

	mkc = MLX5_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);
	MLX5_SET(mkc, mkc, access_mode_1_0, MLX5_MKC_ACCESS_MODE_MTT);
	MLX5_SET(mkc, mkc, lr, 1);
	MLX5_SET(mkc, mkc, lw, 1);
	MLX5_SET(mkc, mkc, rr, 1);
	MLX5_SET(mkc, mkc, rw, 1);
	MLX5_SET(mkc, mkc, pd, pdn);
	MLX5_SET(mkc, mkc, bsf_octword_size, 0);
	MLX5_SET(mkc, mkc, qpn, 0xffffff);
	MLX5_SET(mkc, mkc, log_page_size, PAGE_SHIFT);
	MLX5_SET(mkc, mkc, translations_octword_size, DIV_ROUND_UP(npages, 2));
	MLX5_SET64(mkc, mkc, len, npages * PAGE_SIZE);

	return in;
}

static int vfmig_create_mkey(struct mlx5_core_dev *mdev, u32 npages,
			     u32 *mkey_in, u32 *mkey)
{
	int inlen = MLX5_ST_SZ_BYTES(create_mkey_in) +
		sizeof(__be64) * round_up(npages, 2);

	return mlx5_core_create_mkey(mdev, mkey, mkey_in, inlen);
}

static void vfmig_unregister_dma_pages(struct mlx5_core_dev *mdev, u32 npages,
				       u32 *mkey_in,
				       struct dma_iova_state *state,
				       enum dma_data_direction dir)
{
	dma_addr_t addr;
	__be64 *mtt;
	int i;

	if (dma_use_iova(state)) {
		dma_iova_destroy(mdev->device, state, npages * PAGE_SIZE, dir,
				 0);
	} else {
		mtt = (__be64 *)MLX5_ADDR_OF(create_mkey_in, mkey_in,
					     klm_pas_mtt);
		for (i = npages - 1; i >= 0; i--) {
			addr = be64_to_cpu(mtt[i]);
			dma_unmap_page(mdev->device, addr, PAGE_SIZE, dir);
		}
	}
}

static int vfmig_register_dma_pages(struct mlx5_core_dev *mdev, u32 npages,
				    struct page **page_list, u32 *mkey_in,
				    struct dma_iova_state *state,
				    enum dma_data_direction dir)
{
	dma_addr_t addr;
	size_t mapped = 0;
	__be64 *mtt;
	int i, err;

	mtt = (__be64 *)MLX5_ADDR_OF(create_mkey_in, mkey_in, klm_pas_mtt);

	if (dma_iova_try_alloc(mdev->device, state, 0, npages * PAGE_SIZE)) {
		addr = state->addr;
		for (i = 0; i < npages; i++) {
			err = dma_iova_link(mdev->device, state,
					    page_to_phys(page_list[i]), mapped,
					    PAGE_SIZE, dir, 0);
			if (err)
				goto error;
			*mtt++ = cpu_to_be64(addr);
			addr += PAGE_SIZE;
			mapped += PAGE_SIZE;
		}
		err = dma_iova_sync(mdev->device, state, 0, mapped);
		if (err)
			goto error;
	} else {
		for (i = 0; i < npages; i++) {
			addr = dma_map_page(mdev->device, page_list[i], 0,
					    PAGE_SIZE, dir);
			err = dma_mapping_error(mdev->device, addr);
			if (err)
				goto error;
			*mtt++ = cpu_to_be64(addr);
		}
	}
	return 0;

error:
	vfmig_unregister_dma_pages(mdev, i, mkey_in, state, dir);
	return err;
}

/*
 * Bulk page allocator equivalent to drivers/vfio/pci/mlx5/cmd.c's
 * mlx5vf_add_pages(). Out param @page_list is kvcalloc()'d.
 *
 * TODO(vfmig-dedup): see above.
 */
static int vfmig_alloc_pages(struct page ***page_list, unsigned int npages)
{
	unsigned int filled, done = 0;
	int i;

	*page_list = kvcalloc(npages, sizeof(struct page *),
			      GFP_KERNEL_ACCOUNT);
	if (!*page_list)
		return -ENOMEM;

	for (;;) {
		filled = alloc_pages_bulk(GFP_KERNEL_ACCOUNT, npages - done,
					  *page_list + done);
		if (!filled)
			goto err;

		done += filled;
		if (done == npages)
			break;
	}

	return 0;
err:
	for (i = 0; i < done; i++)
		__free_page((*page_list)[i]);

	kvfree(*page_list);
	*page_list = NULL;
	return -ENOMEM;
}

static void vfmig_free_pages(struct page **page_list, u32 npages)
{
	int i;

	if (!page_list)
		return;

	for (i = npages - 1; i >= 0; i--)
		__free_page(page_list[i]);

	kvfree(page_list);
}

/*
 * LOAD_VHCA_STATE firmware command. Slimmer than the VFIO variant
 * (cmd.c:832 mlx5vf_cmd_load_vhca_state) because we don't carry
 * mvdev/migf indirection -- the caller hands us the PF mdev and the
 * vhca_id directly.
 *
 * TODO(vfmig-dedup): same as helpers above.
 */
static int vfmig_cmd_load_vhca_state(struct mlx5_core_dev *pf_mdev,
				     u16 vhca_id, u32 mkey, size_t size)
{
	u32 out[MLX5_ST_SZ_DW(load_vhca_state_out)] = {};
	u32 in[MLX5_ST_SZ_DW(load_vhca_state_in)] = {};

	MLX5_SET(load_vhca_state_in, in, opcode, MLX5_CMD_OP_LOAD_VHCA_STATE);
	MLX5_SET(load_vhca_state_in, in, op_mod, 0);
	MLX5_SET(load_vhca_state_in, in, vhca_id, vhca_id);
	MLX5_SET(load_vhca_state_in, in, mkey, mkey);
	MLX5_SET(load_vhca_state_in, in, size, size);

	return mlx5_cmd_exec_inout(pf_mdev, load_vhca_state, in, out);
}

/* -------- LOAD_VHCA_STATE: per-fd image-buffer plumbing ----------------- */

/*
 * Tear down whatever DMA state is currently held on @ctx's image buffer.
 * Caller must hold vfmig->lock for read AND ctx->vfmig->pf_mdev must be
 * non-NULL (i.e. !vfmig->dead).
 */
static void vfmig_load_drop_image_dma(struct mlx5_vfmig_load_ctx *ctx,
				      struct mlx5_core_dev *pf_mdev)
{
	if (ctx->image_mkey_created) {
		mlx5_core_destroy_mkey(pf_mdev, ctx->image_mkey);
		ctx->image_mkey_created = false;
	}
	if (ctx->image_dma_mapped) {
		vfmig_unregister_dma_pages(pf_mdev, ctx->image_npages,
					   ctx->image_mkey_in,
					   &ctx->image_dma_state,
					   DMA_TO_DEVICE);
		ctx->image_dma_mapped = false;
	}
	kvfree(ctx->image_mkey_in);
	ctx->image_mkey_in = NULL;
}

/*
 * Ensure ctx has an image buffer of at least @want_npages, with a fresh
 * MKEY suitable for handing to LOAD_VHCA_STATE. If the existing buffer
 * is large enough we just rebuild the MKEY-in scaffolding.
 *
 * Caller holds vfmig->lock for read.
 */
static int vfmig_load_prepare_image(struct mlx5_vfmig_load_ctx *ctx,
				    u32 want_npages)
{
	struct mlx5_core_dev *pf_mdev = ctx->vfmig->pf_mdev;
	int err;

	if (WARN_ON(!pf_mdev))
		return -ENODEV;

	vfmig_load_drop_image_dma(ctx, pf_mdev);

	if (ctx->image_npages < want_npages) {
		vfmig_free_pages(ctx->image_pages, ctx->image_npages);
		ctx->image_pages = NULL;
		ctx->image_npages = 0;

		err = vfmig_alloc_pages(&ctx->image_pages, want_npages);
		if (err)
			return err;
		ctx->image_npages = want_npages;
	}

	ctx->image_mkey_in = vfmig_alloc_mkey_in(want_npages, ctx->pdn);
	if (!ctx->image_mkey_in)
		return -ENOMEM;

	err = vfmig_register_dma_pages(pf_mdev, want_npages, ctx->image_pages,
				       ctx->image_mkey_in,
				       &ctx->image_dma_state, DMA_TO_DEVICE);
	if (err)
		goto err_register;
	ctx->image_dma_mapped = true;

	err = vfmig_create_mkey(pf_mdev, want_npages, ctx->image_mkey_in,
				&ctx->image_mkey);
	if (err)
		goto err_mkey;
	ctx->image_mkey_created = true;
	return 0;

err_mkey:
	vfmig_unregister_dma_pages(pf_mdev, want_npages, ctx->image_mkey_in,
				   &ctx->image_dma_state, DMA_TO_DEVICE);
	ctx->image_dma_mapped = false;
err_register:
	kvfree(ctx->image_mkey_in);
	ctx->image_mkey_in = NULL;
	return err;
}

/* -------- LOAD_VHCA_STATE: parser FSM ----------------------------------- */

static ssize_t vfmig_load_consume_image(struct mlx5_vfmig_load_ctx *ctx,
					const char __user *ubuf, size_t want)
{
	size_t copied = 0;

	while (want) {
		size_t page_off = ctx->image_filled & (PAGE_SIZE - 1);
		u32 page_idx = ctx->image_filled >> PAGE_SHIFT;
		size_t chunk = min_t(size_t, want, PAGE_SIZE - page_off);
		u8 *to;

		if (page_idx >= ctx->image_npages)
			return -EINVAL;

		to = kmap_local_page(ctx->image_pages[page_idx]);
		if (copy_from_user(to + page_off, ubuf, chunk)) {
			kunmap_local(to);
			return copied ? (ssize_t)copied : -EFAULT;
		}
		kunmap_local(to);

		ctx->image_filled += chunk;
		ubuf += chunk;
		want -= chunk;
		copied += chunk;
	}
	return copied;
}

static ssize_t vfmig_load_consume_header(struct mlx5_vfmig_load_ctx *ctx,
					 const char __user *ubuf, size_t want)
{
	size_t need = sizeof(ctx->hdr_buf) - ctx->hdr_buf_filled;
	size_t take = min(need, want);

	if (!take)
		return 0;
	if (copy_from_user(ctx->hdr_buf + ctx->hdr_buf_filled, ubuf, take))
		return -EFAULT;
	ctx->hdr_buf_filled += take;
	return take;
}

/*
 * Skip @want bytes of an unknown-but-optional record's payload. We
 * intentionally do not stage them anywhere -- the VFIO variant stages
 * STOP_COPY_SIZE into a small buffer to then size-up the next image
 * proactively, but for our prototype we just discard and let
 * PREP_IMAGE realloc pick the right size on demand.
 */
static ssize_t vfmig_load_skip_record(struct mlx5_vfmig_load_ctx *ctx,
				      const char __user *ubuf, size_t want)
{
	u8 sink[64];
	size_t total = 0;

	while (want && ctx->record_skipped < ctx->record_size) {
		size_t left = ctx->record_size - ctx->record_skipped;
		size_t chunk = min3(want, left, sizeof(sink));

		if (copy_from_user(sink, ubuf, chunk))
			return total ? (ssize_t)total : -EFAULT;
		ctx->record_skipped += chunk;
		ubuf += chunk;
		want -= chunk;
		total += chunk;
	}
	return total;
}

static int vfmig_load_dispatch_header(struct mlx5_vfmig_load_ctx *ctx)
{
	struct vfmig_wire_header *hdr =
		(struct vfmig_wire_header *)ctx->hdr_buf;
	u64 record_size = le64_to_cpu(hdr->record_size);
	u32 flags = le32_to_cpu(hdr->flags);
	u32 tag = le32_to_cpu(hdr->tag);

	if (record_size > VFMIG_MAX_LOAD_SIZE)
		return -EINVAL;

	ctx->record_size = record_size;
	ctx->record_tag = tag;
	ctx->record_skipped = 0;
	ctx->image_filled = 0;
	ctx->hdr_buf_filled = 0;

	switch (tag) {
	case VFMIG_WIRE_TAG_FW_DATA:
		ctx->state = VFMIG_LS_PREP_IMAGE;
		return 0;
	default:
		if (!(flags & VFMIG_WIRE_FLAGS_TAG_OPTIONAL))
			return -EOPNOTSUPP;
		ctx->state = VFMIG_LS_READ_HEADER_DATA;
		return 0;
	}
}

static int vfmig_load_run_load(struct mlx5_vfmig_load_ctx *ctx)
{
	struct mlx5_core_dev *pf_mdev = ctx->vfmig->pf_mdev;
	int err;

	if (WARN_ON(!ctx->image_mkey_created))
		return -EINVAL;

	err = vfmig_cmd_load_vhca_state(pf_mdev, ctx->vhca_id,
					ctx->image_mkey, ctx->record_size);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: LOAD_VHCA_STATE vf %u (vhca_id 0x%04x) size %llu failed: %d\n",
			       ctx->vf_id, ctx->vhca_id,
			       (unsigned long long)ctx->record_size, err);
		return err;
	}

	mlx5_core_dbg(pf_mdev,
		      "vfmig: loaded %llu bytes of state into vf %u (vhca_id 0x%04x)\n",
		      (unsigned long long)ctx->record_size, ctx->vf_id,
		      ctx->vhca_id);
	return 0;
}

static int vfmig_load_step(struct mlx5_vfmig_load_ctx *ctx,
			   const char __user **ubuf, size_t *left,
			   bool *progressed)
{
	ssize_t n;
	int err;
	u32 want_npages;

	switch (ctx->state) {
	case VFMIG_LS_READ_HEADER:
		n = vfmig_load_consume_header(ctx, *ubuf, *left);
		if (n < 0)
			return n;
		*ubuf += n;
		*left -= n;
		*progressed = n > 0;
		if (ctx->hdr_buf_filled == sizeof(ctx->hdr_buf)) {
			err = vfmig_load_dispatch_header(ctx);
			if (err)
				return err;
		}
		return 0;

	case VFMIG_LS_PREP_IMAGE:
		want_npages = max_t(u32, 1,
				    DIV_ROUND_UP(ctx->record_size, PAGE_SIZE));
		err = vfmig_load_prepare_image(ctx, want_npages);
		if (err)
			return err;
		ctx->state = VFMIG_LS_READ_IMAGE;
		*progressed = true;
		return 0;

	case VFMIG_LS_READ_IMAGE: {
		size_t want = min_t(size_t, *left,
				    ctx->record_size - ctx->image_filled);

		if (!want) {
			if (ctx->image_filled == ctx->record_size)
				ctx->state = VFMIG_LS_LOAD_IMAGE;
			else
				*progressed = false;
			return 0;
		}
		n = vfmig_load_consume_image(ctx, *ubuf, want);
		if (n < 0)
			return n;
		*ubuf += n;
		*left -= n;
		*progressed = n > 0;
		if (ctx->image_filled == ctx->record_size)
			ctx->state = VFMIG_LS_LOAD_IMAGE;
		return 0;
	}

	case VFMIG_LS_LOAD_IMAGE:
		err = vfmig_load_run_load(ctx);
		if (err)
			return err;
		ctx->state = VFMIG_LS_READ_HEADER;
		*progressed = true;
		return 0;

	case VFMIG_LS_READ_HEADER_DATA:
		n = vfmig_load_skip_record(ctx, *ubuf, *left);
		if (n < 0)
			return n;
		*ubuf += n;
		*left -= n;
		*progressed = n > 0;
		if (ctx->record_skipped == ctx->record_size)
			ctx->state = VFMIG_LS_READ_HEADER;
		return 0;
	}
	return -EINVAL;
}

/* -------- LOAD_VHCA_STATE: file ops ------------------------------------- */

static ssize_t vfmig_load_write(struct file *filp, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct mlx5_vfmig_load_ctx *ctx = filp->private_data;
	struct mlx5_vfmig_pf *vfmig = ctx->vfmig;
	const char __user *cursor = ubuf;
	size_t left = count;
	ssize_t produced;
	bool progressed;
	int err = 0;

	if (!count)
		return 0;

	mutex_lock(&ctx->io_lock);
	down_read(&vfmig->lock);
	if (vfmig->dead || ctx->resources_freed) {
		err = -ENODEV;
		goto out;
	}

	while (left) {
		progressed = false;
		err = vfmig_load_step(ctx, &cursor, &left, &progressed);
		if (err)
			break;
		if (!progressed)
			break;
	}

out:
	up_read(&vfmig->lock);
	mutex_unlock(&ctx->io_lock);

	produced = (ssize_t)(count - left);
	if (!produced && err)
		return err;
	*ppos += produced;
	return produced;
}

/*
 * Drop firmware-tied resources held by @ctx. Caller must guarantee
 * pf_mdev is alive: either holds vfmig->lock for read with !vfmig->dead
 * (file release path), or holds vfmig->lock for write before pf_mdev is
 * cleared (pf_cleanup path). Becomes a no-op once resources_freed.
 */
static void vfmig_load_release_resources(struct mlx5_vfmig_load_ctx *ctx)
{
	struct mlx5_core_dev *pf_mdev = ctx->vfmig->pf_mdev;

	if (ctx->resources_freed)
		return;
	ctx->resources_freed = true;

	if (!pf_mdev)
		return;

	vfmig_load_drop_image_dma(ctx, pf_mdev);

	if (ctx->pd_allocated) {
		mlx5_core_dealloc_pd(pf_mdev, ctx->pdn);
		ctx->pd_allocated = false;
	}
}

static int vfmig_load_release(struct inode *inode, struct file *filp)
{
	struct mlx5_vfmig_load_ctx *ctx = filp->private_data;
	struct mlx5_vfmig_pf *vfmig = ctx->vfmig;

	/*
	 * Tear down firmware-tied resources while pf_mdev is still alive.
	 * If the PF has already been unbound (dead), pf_cleanup() did the
	 * teardown synchronously and resources_freed is already set.
	 */
	down_read(&vfmig->lock);
	if (!vfmig->dead)
		vfmig_load_release_resources(ctx);
	up_read(&vfmig->lock);

	mutex_lock(&vfmig->load_ctxs_lock);
	list_del(&ctx->node);
	mutex_unlock(&vfmig->load_ctxs_lock);

	vfmig_free_pages(ctx->image_pages, ctx->image_npages);
	mutex_destroy(&ctx->io_lock);
	vfmig_pf_put(vfmig);
	kfree(ctx);
	return 0;
}

static const struct file_operations mlx5_vfmig_load_fops = {
	.owner		= THIS_MODULE,
	.write		= vfmig_load_write,
	.release	= vfmig_load_release,
};

/*
 * Set up the LOAD session and hand back an anon-inode fd. Caller holds
 * vfmig->lock for read.
 */
static long vfmig_ioc_load_vhca_state(struct mlx5_vfmig_pf *vfmig,
				      void __user *uarg)
{
	struct mlx5_vfmig_load_state arg;
	struct mlx5_core_sriov *sriov;
	struct mlx5_vfmig_load_ctx *ctx, *iter;
	struct file *file;
	u16 vhca_id;
	int fd;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.flags || arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	err = vfmig_query_vhca_id(vfmig->pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	INIT_LIST_HEAD(&ctx->node);
	mutex_init(&ctx->io_lock);
	ctx->vf_id = arg.vf_id;
	ctx->vhca_id = vhca_id;
	ctx->state = VFMIG_LS_READ_HEADER;

	/*
	 * Claim the vf_id slot first so the dup check + list_add are
	 * atomic. ctx->vfmig is set here too because everything past this
	 * point may need to call vfmig_load_release_resources(), which
	 * dereferences ctx->vfmig.
	 */
	mutex_lock(&vfmig->load_ctxs_lock);
	list_for_each_entry(iter, &vfmig->load_ctxs, node) {
		if (iter->vf_id == ctx->vf_id) {
			mutex_unlock(&vfmig->load_ctxs_lock);
			err = -EBUSY;
			goto err_claim;
		}
	}
	vfmig_pf_get(vfmig);
	ctx->vfmig = vfmig;
	list_add(&ctx->node, &vfmig->load_ctxs);
	mutex_unlock(&vfmig->load_ctxs_lock);

	err = mlx5_core_alloc_pd(vfmig->pf_mdev, &ctx->pdn);
	if (err)
		goto err_pd;
	ctx->pd_allocated = true;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		err = fd;
		goto err_fd;
	}

	file = anon_inode_getfile("mlx5_vfmig_load", &mlx5_vfmig_load_fops,
				  ctx, O_WRONLY | O_CLOEXEC);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_anon;
	}
	stream_open(file_inode(file), file);

	arg.load_fd = fd;
	if (copy_to_user(uarg, &arg, sizeof(arg))) {
		err = -EFAULT;
		goto err_copy;
	}

	fd_install(fd, file);
	mlx5_core_info(vfmig->pf_mdev,
		       "vfmig: LOAD session opened for vf %u (vhca_id 0x%04x)\n",
		       ctx->vf_id, ctx->vhca_id);
	return 0;

err_copy:
	fput(file);
err_anon:
	put_unused_fd(fd);
err_fd:
	mlx5_core_dealloc_pd(vfmig->pf_mdev, ctx->pdn);
	ctx->pd_allocated = false;
err_pd:
	mutex_lock(&vfmig->load_ctxs_lock);
	list_del(&ctx->node);
	mutex_unlock(&vfmig->load_ctxs_lock);
	vfmig_pf_put(vfmig);
err_claim:
	mutex_destroy(&ctx->io_lock);
	kfree(ctx);
	return err;
}

/* -------- cdev file ops ------------------------------------------------- */

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
	case MLX5_VFMIG_IOC_LOAD_VHCA_STATE:
		ret = vfmig_ioc_load_vhca_state(vfmig, uarg);
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
	mutex_init(&vfmig->load_ctxs_lock);
	INIT_LIST_HEAD(&vfmig->load_ctxs);
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
	mutex_destroy(&vfmig->load_ctxs_lock);
	kfree(vfmig);
	return err;
}

void mlx5_vfmig_pf_cleanup(struct mlx5_core_dev *pf_mdev)
{
	struct mlx5_vfmig_pf *vfmig = pf_mdev->priv.vfmig;
	struct mlx5_vfmig_load_ctx *ctx;
	dev_t devno;

	if (!vfmig)
		return;

	pf_mdev->priv.vfmig = NULL;
	devno = MKDEV(MAJOR(mlx5_vfmig_devt), vfmig->minor);

	/*
	 * Neuter the device. Synchronously tear down LOAD-session
	 * firmware resources (PD/MKEY/DMA mappings) while pf_mdev is still
	 * alive; the page lists themselves are mdev-independent and get
	 * freed when each fd is later closed.
	 *
	 * The down_write blocks until all in-flight readers
	 * (vfmig_ioctl, vfmig_load_write, vfmig_load_release) drop their
	 * read locks. Once we hold the write lock the load_ctxs list is
	 * stable without taking load_ctxs_lock.
	 */
	down_write(&vfmig->lock);
	list_for_each_entry(ctx, &vfmig->load_ctxs, node)
		vfmig_load_release_resources(ctx);
	vfmig->dead = true;
	vfmig->pf_mdev = NULL;
	up_write(&vfmig->lock);

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
