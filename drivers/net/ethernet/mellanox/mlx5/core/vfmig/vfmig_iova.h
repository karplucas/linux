/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * Per-VF unmanaged IOVA domain for mlx5 host-driven VF migration.
 *
 * Why
 * ---
 * SAVE_VHCA_STATE captures, inside the firmware blob, the source VHCA's
 * own IOVAs (cmd ring, host pages, EQ/UAR buffers). On native (non-VFIO,
 * non-VM) mlx5_core the destination's dma-iommu layer hands out fresh,
 * unrelated IOVAs, so after LOAD the firmware would dereference addresses
 * that point nowhere. The foundation of the fix is to give each
 * migratable VF an unmanaged paging iommu_domain that the PF driver
 * fully owns, so a later patch can lay out deterministic, reproducible
 * IOVAs inside it that survive a SAVE on one host and a LOAD on another.
 *
 * Scope of this file
 * ------------------
 * Only the domain lifecycle: allocate + attach on SET_TRACKED{enable=1},
 * detach + free on SET_TRACKED{enable=0} / SR-IOV teardown / PF unload.
 * The deterministic IOVA allocator and the SAVE/LOAD page replay that
 * consume the domain land in later patches.
 *
 * Coexistence with dma-iommu
 * --------------------------
 * Attaching an IOMMU_DOMAIN_UNMANAGED displaces the device's
 * dma-iommu-managed default DMA domain, so while our domain is attached
 * dma_alloc_coherent() on the VF WILL fail -- nothing routes DMA-API
 * calls into our domain yet. That is intentional: a tracked VF has no
 * usable DMA until the allocator routing lands, so it must stay unbound
 * from its driver while tracked (SET_TRACKED enforces this).
 */

#ifndef __MLX5_CORE_VFMIG_IOVA_H__
#define __MLX5_CORE_VFMIG_IOVA_H__

#include <linux/types.h>

struct pci_dev;
struct vfmig_iova_domain;

/*
 * Per-VF IOVA window: [BASE + vf_id * PER_VF, BASE + (vf_id + 1) * PER_VF).
 * BASE is 4 GiB -- well clear of anything the default allocator hands out
 * and leaving the bottom 4 GiB untouched; PER_VF is 4 GiB, ample for the
 * cmd ring + MANAGE_PAGES host pages + EQ/UAR buffers a later layer maps.
 * The window must fit the device's IOMMU geometry aperture; that is
 * validated at attach time (vfmig_iova_domain_create()).
 */
#define VFMIG_IOVA_BASE		0x100000000ULL	/* 4 GiB */
#define VFMIG_IOVA_PER_VF	(4ULL << 30)	/* 4 GiB per VF */

/*
 * Allocate an unmanaged paging iommu_domain, attach it to @vf_pdev, and
 * return the handle in *@out. @vf_pdev must be unbound and its
 * device_lock held by the caller. Returns 0 on success (with a pinned
 * reference on @vf_pdev held for the domain's lifetime), -EOPNOTSUPP if
 * the per-VF IOVA window does not fit the device's IOMMU aperture, or a
 * negative errno from the iommu core.
 */
int  vfmig_iova_domain_create(struct pci_dev *vf_pdev, u32 vf_id,
			      struct vfmig_iova_domain **out);

/*
 * Detach @dom from its VF, free the iommu_domain, and drop the pinned VF
 * reference. The VF must be unbound. Safe with @dom == NULL.
 */
void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom);

#endif /* __MLX5_CORE_VFMIG_IOVA_H__ */
