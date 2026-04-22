/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * UAPI for mlx5 host-driven VF migration / CRIU restore.
 *
 * One char device per mlx5_core PF, exposed as /dev/mlx5_vfmig/<bdf>.
 * Userspace marks individual VFs as "restored" so that, when those VFs are
 * subsequently bound to mlx5_core, their probe path skips INIT_HCA and
 * instead adopts the firmware state previously installed by LOAD_VHCA_STATE.
 *
 * Lifecycle (destination side, prototype - M1'):
 *   1. sriov_drivers_autoprobe = 0 on the PF
 *   2. sriov_numvfs = N on the PF        (VFs created, unbound)
 *   3. open /dev/mlx5_vfmig/<pf_bdf>
 *   4. ioctl(MLX5_VFMIG_IOC_MARK_RESTORED, vf_id)
 *      [M2: ioctl(MLX5_VFMIG_IOC_LOAD_STATE, ...) which includes the above]
 *   5. driver_override + bind on the VF
 *   6. mlx5_core probe skips INIT_HCA for that VF
 */

#ifndef _UAPI_LINUX_MLX5_VFMIG_H
#define _UAPI_LINUX_MLX5_VFMIG_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define MLX5_VFMIG_IOC_MAGIC	0xB5

/*
 * MLX5_VFMIG_IOC_MARK_RESTORED:
 *   Mark VF @vf_id as having had its firmware state restored. The next
 *   mlx5_core probe of that VF will skip INIT_HCA.
 *   Returns 0 on success, -EINVAL if vf_id is out of range, -EALREADY if
 *   the flag was already set.
 */
struct mlx5_vfmig_mark_restored {
	__u32 vf_id;
	__u32 reserved;
};
#define MLX5_VFMIG_IOC_MARK_RESTORED \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x01, struct mlx5_vfmig_mark_restored)

/*
 * MLX5_VFMIG_IOC_GET_VHCA_ID:
 *   PF-side query of the VF's vhca_id via QUERY_HCA_CAP(other_function=1).
 *   Debug helper that lets userspace confirm the PF can address the VF
 *   without binding any driver to it.
 */
struct mlx5_vfmig_get_vhca_id {
	__u32 vf_id;	/* in  */
	__u16 vhca_id;	/* out */
	__u16 reserved;
};
#define MLX5_VFMIG_IOC_GET_VHCA_ID \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x02, struct mlx5_vfmig_get_vhca_id)

/*
 * MLX5_VFMIG_IOC_QUERY_VF:
 *   Diagnostic snapshot of one VF on the owning PF. Returns the VF's
 *   live vhca_id (queried via QUERY_HCA_CAP(other_function=1)), the
 *   "restored" bit currently latched on the PF, and the total number
 *   of VFs the PF has provisioned. Userspace iterates 0..num_vfs-1 to
 *   enumerate; that's intentionally cheaper to maintain than a
 *   variable-length list ioctl.
 */
struct mlx5_vfmig_query_vf {
	__u32 vf_id;		/* in  */
	__u32 num_vfs;		/* out: total VFs provisioned on this PF */
	__u16 vhca_id;		/* out */
	__u8  restored;		/* out: 1 if MARK_RESTORED was issued */
	__u8  reserved;
};
#define MLX5_VFMIG_IOC_QUERY_VF \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x03, struct mlx5_vfmig_query_vf)

#endif /* _UAPI_LINUX_MLX5_VFMIG_H */
