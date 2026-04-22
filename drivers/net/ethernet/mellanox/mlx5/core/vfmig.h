/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * Host-driver-side VF migration / CRIU restore support for mlx5_core.
 *
 * Architecture (M1'):
 *   - One char device per PF mlx5_core, /dev/mlx5_vfmig/<pf_bdf>.
 *   - Userspace marks individual VFs as "restored" via ioctl. The flag
 *     lives on the PF, in priv.sriov.vfs_ctx[vf_id].restored, so it is
 *     trivially reachable from the VF's own probe path.
 *   - During VF probe, mlx5_function_open() consults the PF's flag for
 *     the VF currently being brought up and, if set, skips INIT_HCA so
 *     that previously-loaded firmware state survives.
 *
 * The cdev only exists on PF mdevs. The vf-side hook
 * (mlx5_vfmig_vf_consume_restored) works against any mlx5_core_dev: it
 * returns false for PFs and for VFs whose owning PF mdev is not bound
 * or has no flag set.
 *
 * Lifetime:
 *   mlx5_vfmig_pf_init()    -> called from mlx5_init_one_devl_locked()
 *                              after the device is fully up, on PFs only.
 *   mlx5_vfmig_pf_cleanup() -> called from mlx5_uninit_one() on PFs only.
 *
 * No firmware operations happen yet (M1' is a stub for the data-plane
 * side). Subsequent milestones will add LOAD/SAVE_VHCA_STATE, a data fd
 * for the blob, the SUSPEND/RESUME/QUERY commands, and proper teardown
 * semantics around the "restored" flag.
 */

#ifndef __MLX5_CORE_VFMIG_H__
#define __MLX5_CORE_VFMIG_H__

#include <linux/mlx5/driver.h>

int  mlx5_vfmig_pf_init(struct mlx5_core_dev *pf_mdev);
void mlx5_vfmig_pf_cleanup(struct mlx5_core_dev *pf_mdev);

/*
 * Returns true iff @dev is a VF and its PF has marked it as restored.
 * Safe to call unconditionally on any mlx5_core_dev. Internally takes
 * and releases mlx5_vf_get_core_dev() / mlx5_vf_put_core_dev() on the
 * PF, so it must NOT be called while already holding the PF's
 * intf_state_mutex.
 *
 * On true, @vhca_id_out (if non-NULL) is populated with the VF's
 * vhca_id captured at MARK_RESTORED time. Used purely to make the
 * probe-time skip log identify the firmware vHCA.
 *
 * The flag is consumed (cleared) by this call so that a subsequent
 * unbind/rebind of the same VF without an explicit MARK_RESTORED falls
 * back to the normal probe path. This is a deliberate choice for M1':
 * mis-replays of probe should fail loudly rather than silently keep
 * skipping INIT_HCA.
 */
bool mlx5_vfmig_vf_consume_restored(struct mlx5_core_dev *dev, u16 *vhca_id_out);

/* Module init/exit hooks for the cdev region. */
int  mlx5_vfmig_module_init(void);
void mlx5_vfmig_module_exit(void);

#endif /* __MLX5_CORE_VFMIG_H__ */
