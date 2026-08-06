/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Vendored mlx5_vfmig UAPI for the build-up harness.
 *
 * This tracks the *incrementally upstreamed* vfmig ioctl surface, which
 * is intentionally a subset of the oracle's full
 * include/uapi/linux/mlx5_vfmig.h. It is vendored here -- rather than
 * including the in-tree header -- because the two can diverge in struct
 * layout, and only a definition whose sizeof() matches the running
 * kernel's is wire-compatible (the _IOWR command number encodes the
 * struct size).
 *
 * Grow this header one command at a time, in lockstep with the kernel
 * milestones, so `mlx5_vfmig_min` always matches the surface it probes.
 * As of the deterministic-IOVA tracking milestone, QUERY_VF carries the
 * @tracked and @vf_uuid fields (matching the in-tree struct byte for
 * byte) and SET_TRACKED (0x07) is present.
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
	__u8  tracked;		/* out: 1 if a vfmig IOVA domain is attached */
	__u8  vf_uuid[16];	/* out: orchestrator UUID, all-zeros if unset */
	__u8  reserved_out[8];	/* out: zeroed */
};

#define MLX5_VFMIG_IOC_QUERY_VF \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x03, struct mlx5_vfmig_query_vf)

struct mlx5_vfmig_enable_migratable {
	__u32 vf_id;	/* in */
	__u32 reserved;
};

#define MLX5_VFMIG_IOC_ENABLE_MIGRATABLE \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x06, struct mlx5_vfmig_enable_migratable)

/*
 * SET_TRACKED: with @enable=1 the driver attaches an unmanaged paging
 * iommu_domain to VF @vf_id, staking out a deterministic per-VF IOVA
 * window; @enable=0 detaches and frees it. The VF must be unbound. See
 * QUERY_VF.@tracked for the resulting state.
 */
struct mlx5_vfmig_set_tracked {
	__u32 vf_id;		/* in  */
	__u32 enable;		/* in: 0 = untrack, 1 = track */
	__u32 flags;		/* in: reserved, must be 0 */
	__u32 reserved;		/* in: reserved, must be 0 */
};

#define MLX5_VFMIG_IOC_SET_TRACKED \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x07, struct mlx5_vfmig_set_tracked)

#define MLX5_VFMIG_DIR_FLAG_INITIATOR	0x1u
#define MLX5_VFMIG_DIR_FLAG_RESPONDER	0x2u
#define MLX5_VFMIG_DIR_FLAG_ALL \
	(MLX5_VFMIG_DIR_FLAG_INITIATOR | MLX5_VFMIG_DIR_FLAG_RESPONDER)

struct mlx5_vfmig_suspend_vhca {
	__u32 vf_id;	/* in  */
	__u32 flags;	/* in: 0 or a subset of MLX5_VFMIG_DIR_FLAG_* */
	__u32 reserved[2];
};

#define MLX5_VFMIG_IOC_SUSPEND_VHCA \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x13, struct mlx5_vfmig_suspend_vhca)

struct mlx5_vfmig_resume_vhca {
	__u32 vf_id;	/* in  */
	__u32 flags;	/* in: 0 or a subset of MLX5_VFMIG_DIR_FLAG_* */
	__u32 reserved[2];
};

#define MLX5_VFMIG_IOC_RESUME_VHCA \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x14, struct mlx5_vfmig_resume_vhca)

#define MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED	0x1u

struct mlx5_vfmig_save_state {
	__u32 vf_id;	/* in  */
	__u32 flags;	/* in: 0 or MLX5_VFMIG_SAVE_FLAG_* */
	__s32 save_fd;	/* out */
	__u32 reserved;
};

#define MLX5_VFMIG_IOC_SAVE_VHCA_STATE \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x05, struct mlx5_vfmig_save_state)

struct mlx5_vfmig_load_state {
	__u32 vf_id;	/* in  */
	__u32 flags;	/* in: reserved, must be 0 */
	__s32 load_fd;	/* out */
	__u32 reserved;
};

#define MLX5_VFMIG_IOC_LOAD_VHCA_STATE \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x04, struct mlx5_vfmig_load_state)

/*
 * On-wire framing for the SAVE stream. Each record starts with this
 * 16-byte little-endian header; the first (and, for this milestone, only)
 * record is a FW_DATA blob whose payload is @record_size bytes of raw
 * firmware image following the header.
 */
struct vfmig_wire_header {
	__u64 record_size;	/* payload bytes after this header */
	__u32 flags;
	__u32 tag;
};

#define VFMIG_WIRE_TAG_FW_DATA	0

#endif /* _MLX5_VFMIG_BUILDUP_H */
