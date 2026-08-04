/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Vendored mlx5_vfmig UAPI for the build-up harness.
 *
 * This tracks the *incrementally upstreamed* vfmig ioctl surface (the
 * vfmig-rebase-0 series), which is intentionally a strict subset of the
 * oracle's full include/uapi/linux/mlx5_vfmig.h. It is vendored here --
 * rather than including the in-tree header -- precisely because the two
 * diverge: e.g. the build-up QUERY_VF struct is smaller than the
 * oracle's (no tracked / vf_uuid fields), so its _IOWR command number
 * differs and only this definition is wire-compatible with a kernel
 * built from the build-up branch.
 *
 * Grow this header one command at a time, in lockstep with the kernel
 * milestones, so `mlx5_vfmig_min` always matches the surface it probes.
 */

#ifndef _MLX5_VFMIG_BUILDUP_H
#define _MLX5_VFMIG_BUILDUP_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define MLX5_VFMIG_IOC_MAGIC	0xB5

struct mlx5_vfmig_mark_restored {
	__u32 vf_id;	/* in */
	__u32 reserved;
};

#define MLX5_VFMIG_IOC_MARK_RESTORED \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x01, struct mlx5_vfmig_mark_restored)

struct mlx5_vfmig_get_vhca_id {
	__u32 vf_id;	/* in  */
	__u16 vhca_id;	/* out */
	__u16 reserved;
};

#define MLX5_VFMIG_IOC_GET_VHCA_ID \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x02, struct mlx5_vfmig_get_vhca_id)

struct mlx5_vfmig_query_vf {
	__u32 vf_id;		/* in  */
	__u32 num_vfs;		/* out */
	__u16 vhca_id;		/* out */
	__u8  restored;		/* out */
	__u8  reserved;
};

#define MLX5_VFMIG_IOC_QUERY_VF \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x03, struct mlx5_vfmig_query_vf)

#endif /* _MLX5_VFMIG_BUILDUP_H */
