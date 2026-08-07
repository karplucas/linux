/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * vfmig_dma_ops: per-VF custom dma_map_ops shim that intercepts the
 * DMA API on a tracked VF and routes it through the per-VF unmanaged
 * iommu_domain owned by vfmig_iova.
 *
 * Why
 * ---
 * At SET_TRACKED time vfmig_iova_domain_create() detaches the VF from
 * its default DMA domain and attaches an unmanaged paging domain the PF
 * driver fully owns. After that the stock dma-iommu fast path is no
 * longer usable on the VF: it allocates IOVAs from the *default*
 * domain's cookie, which is no longer the domain the device is attached
 * to, so any dma_alloc_coherent / dma_map_page / dma_map_sgtable on the
 * VF would install mappings the hardware can't see. This shim takes
 * over the DMA-layer dispatch so every DMA-API entry point can be
 * landed in the per-VF domain instead. The eventual routing is:
 *
 *   - .alloc / .free and .map_phys / .unmap_phys -> the non-migrated
 *     kcoherent sub-arena (kernel coherent + streaming DMA on the VF:
 *     mlx5e rings, CQ/EQ buffers, page_pool, skb fragments). These
 *     allocations are not in the SAVE manifest; the destination
 *     re-probes mlx5e fresh.
 *   - .map_sg / .unmap_sg -> the migration-tracked USER_PAGE registry
 *     for user-space-pinned MR/CQ/QP/SRQ buffers (ib_umem_get ->
 *     dma_map_sgtable), whose IOVAs must round-trip through SAVE/LOAD.
 *
 * This patch installs the interception mechanism (dev->dma_ops +
 * dev->dma_iommu takeover, attach/detach tied to the domain lifetime)
 * and the trivial callbacks; the memory callbacks fail cleanly and are
 * turned live one at a time by follow-up patches that add their backing
 * arenas. Keeping the mechanism separate from the routing avoids a large
 * atomic change and keeps each callback landing next to its consumer.
 *
 * The shim must also clear dev->dma_iommu while attached:
 * __dma_map_sg_attrs() checks use_dma_iommu(dev) BEFORE falling through
 * to ops->map_sg, so without that override our ops are bypassed. The
 * original value is saved at attach and restored on detach.
 *
 * Lifetime is tied to vfmig_iova_domain_create / _detach_dev: the shim
 * is installed right after iommu_attach_device and uninstalled just
 * before iommu_detach_device, so the (custom dma_ops, custom iommu
 * domain) pairing is atomic from the DMA layer's point of view.
 */

#ifndef __MLX5_CORE_VFMIG_DMA_OPS_H__
#define __MLX5_CORE_VFMIG_DMA_OPS_H__

#include <linux/types.h>

struct pci_dev;
struct vfmig_iova_domain;

#if IS_ENABLED(CONFIG_MLX5_VFMIG)

/*
 * Install vfmig_dma_ops on @vf_pdev->dev and register the (dev, dom)
 * association so the dma_map_ops callbacks can find their domain.
 *
 * Caller has already iommu_attach_device'd @dom->iommu_dom; this
 * function takes over the DMA-layer dispatch on top. On failure no
 * state is changed and the caller's iommu_attach is still in effect.
 *
 * Errors:
 *   -EOPNOTSUPP	the architecture doesn't honour set_dma_ops
 *		(CONFIG_ARCH_HAS_DMA_OPS=n); checked at runtime so the
 *		same binary works on ARCH_HAS_DMA_OPS builds and
 *		degrades cleanly elsewhere.
 *   -EBUSY	@vf_pdev already has a shim attached (caller bug)
 *   -ENOMEM	priv allocation / xarray insert failed
 */
int  vfmig_dma_ops_attach(struct pci_dev *vf_pdev,
			  struct vfmig_iova_domain *dom);

/*
 * Reverse of vfmig_dma_ops_attach: restore the original dev->dma_ops
 * and dev->dma_iommu, and drop the (dev, dom) association. Safe with
 * @vf_pdev == NULL or with no prior attach (no-op + warn).
 */
void vfmig_dma_ops_detach(struct pci_dev *vf_pdev);

#else /* !CONFIG_MLX5_VFMIG */

static inline int  vfmig_dma_ops_attach(struct pci_dev *vf_pdev,
					struct vfmig_iova_domain *dom)
{
	return -EOPNOTSUPP;
}

static inline void vfmig_dma_ops_detach(struct pci_dev *vf_pdev) { }

#endif /* CONFIG_MLX5_VFMIG */

#endif /* __MLX5_CORE_VFMIG_DMA_OPS_H__ */
