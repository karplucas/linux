// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

/*
 * vfmig_dma_ops: per-VF dma_map_ops shim. See vfmig_dma_ops.h for the
 * why / lifetime; this file is the implementation.
 *
 * State association
 * -----------------
 * Each tracked VF's struct device is entered into a global xarray keyed
 * by (unsigned long)dev, holding the pre-attach dma_ops / dma_iommu we
 * must restore on detach (plus a back-pointer to the per-VF domain the
 * memory callbacks will chase once they are wired in follow-up patches).
 * The xarray is small (one entry per tracked VF, bounded by the SR-IOV
 * VF count). dev is stable for the (attach, detach) interval -- the
 * caller holds the VF's device_lock and the VF is unbound throughout.
 *
 * dev->dma_iommu override
 * -----------------------
 * __dma_map_sg_attrs() checks use_dma_iommu(dev) -> dev->dma_iommu
 * BEFORE falling through to ops->map_sg. If we don't clear that flag our
 * ops are silently bypassed. The original value is saved at attach
 * (@orig_dma_iommu) and restored on detach so dma-iommu transparently
 * resumes ownership when the VF is untracked.
 *
 * Routing (this patch)
 * --------------------
 * This patch installs the interception mechanism only. The memory
 * callbacks (.alloc/.free, .map_phys/.unmap_phys, .map_sg/.unmap_sg)
 * fail cleanly here; their per-VF backing arenas are wired in follow-up
 * patches, each of which turns one stub into a live callback. The
 * trivial callbacks (.sync_*, .dma_supported, .get_required_mask) are
 * final as written.
 */

#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include "vfmig_dma_ops.h"

/*
 * Per-attach saved state, in the xarray keyed by (unsigned long)dev.
 * @dom is the back-pointer the memory callbacks will chase (unused until
 * a follow-up wires them up); @orig_* are the pre-attach values used to
 * undo set_dma_ops + dma_iommu on detach.
 */
struct vfmig_dma_ops_priv {
	struct vfmig_iova_domain	*dom;
	const struct dma_map_ops	*orig_dma_ops;
#ifdef CONFIG_IOMMU_DMA
	bool				 orig_dma_iommu;
#endif
};

/*
 * Per-arch helpers for the @dev->dma_iommu override. The field itself
 * is compiled only under CONFIG_IOMMU_DMA (see include/linux/device.h);
 * on !IOMMU_DMA builds use_dma_iommu() is hard-false so there is nothing
 * to clobber and these become no-ops.
 */
static inline bool vfmig_dma_iommu_read(struct device *dev)
{
#ifdef CONFIG_IOMMU_DMA
	return dev_dma_iommu(dev);
#else
	return false;
#endif
}

static inline void vfmig_dma_iommu_write(struct device *dev, bool val)
{
#ifdef CONFIG_IOMMU_DMA
	dev_assign_dma_iommu(dev, val);
#else
	(void)dev;
	(void)val;
#endif
}

static DEFINE_XARRAY(vfmig_dma_ops_xa);

/* -------- dma_map_ops callbacks ----------------------------------------- */

/*
 * dma_map_phys / dma_map_single / dma_map_page land here. On a tracked
 * VF these are mlx5e's streaming RX/TX buffers, which must route to the
 * non-migrated kcoherent sub-arena. That arena is added in a follow-up
 * patch; until then fail cleanly rather than install a mapping the
 * hardware can't see via the (now-detached) default dma-iommu path.
 */
static dma_addr_t vfmig_dma_ops_map_phys(struct device *dev, phys_addr_t phys,
					 size_t size,
					 enum dma_data_direction dir,
					 unsigned long attrs)
{
	dev_warn_ratelimited(dev,
			     "vfmig_dma_ops: map_phys(0x%llx, %zu) on tracked VF not routed yet (kcoherent arena is a follow-up)\n",
			     (u64)phys, size);
	return DMA_MAPPING_ERROR;
}

static void vfmig_dma_ops_unmap_phys(struct device *dev, dma_addr_t handle,
				     size_t size,
				     enum dma_data_direction dir,
				     unsigned long attrs)
{
	/* map_phys never succeeds yet, so there is nothing to unmap. */
}

/*
 * dma_map_sg / dma_map_sgtable land here for tracked VFs -- the
 * ib_umem_get user-MR path. These mappings must round-trip through
 * SAVE/LOAD with stable IOVAs (the migration-tracked USER_PAGE
 * registry), added in a follow-up patch. Until then fail cleanly with
 * -EIO rather than mis-routing umem pages. Note the DMA core calls
 * ops->map_sg without a NULL guard once use_dma_iommu(dev) is false, so
 * this callback must exist even while it only fails.
 */
static int vfmig_dma_ops_map_sg(struct device *dev, struct scatterlist *sg,
				int nents, enum dma_data_direction dir,
				unsigned long attrs)
{
	dev_warn_ratelimited(dev,
			     "vfmig_dma_ops: map_sg on tracked VF not routed yet (user-MR USER_PAGE registry is a follow-up); failing %d segments\n",
			     nents);
	return -EIO;
}

static void vfmig_dma_ops_unmap_sg(struct device *dev, struct scatterlist *sg,
				   int nents, enum dma_data_direction dir,
				   unsigned long attrs)
{
	/* map_sg never succeeds yet, so there is nothing to unmap. */
}

/*
 * Cache-coherent x86 / arm64 with IOMMU_CACHE in the iommu_map call
 * means CPU caches and the device see the same memory; the sync_* hooks
 * have nothing to flush. Provided as no-ops so the DMA layer doesn't
 * WARN on missing callbacks.
 */
static void vfmig_dma_ops_sync_single_for_cpu(struct device *dev,
					      dma_addr_t handle, size_t size,
					      enum dma_data_direction dir)
{
}

static void vfmig_dma_ops_sync_single_for_device(struct device *dev,
						 dma_addr_t handle,
						 size_t size,
						 enum dma_data_direction dir)
{
}

static void vfmig_dma_ops_sync_sg_for_cpu(struct device *dev,
					  struct scatterlist *sg, int nents,
					  enum dma_data_direction dir)
{
}

static void vfmig_dma_ops_sync_sg_for_device(struct device *dev,
					     struct scatterlist *sg, int nents,
					     enum dma_data_direction dir)
{
}

static int vfmig_dma_ops_dma_supported(struct device *dev, u64 mask)
{
	/*
	 * VFMIG only attaches to mlx5 VFs, all 64-bit DMA capable. Our
	 * IOVAs live above the 32-bit boundary (VFMIG_IOVA_BASE = 4 GiB);
	 * require a mask covering at least the smallest IOMMU aperture we
	 * support (39 bits) so a sub-39-bit caller fails up front.
	 */
	return mask >= DMA_BIT_MASK(39);
}

static u64 vfmig_dma_ops_get_required_mask(struct device *dev)
{
	return DMA_BIT_MASK(64);
}

/*
 * dma_alloc_coherent lands here for tracked VFs (mlx5e ring / drop_rq /
 * CQ buffer setup). It routes to the non-migrated kcoherent sub-arena,
 * wired in a follow-up patch; until then fail cleanly so the caller sees
 * an allocation failure rather than an unmapped buffer.
 */
static void *vfmig_dma_ops_alloc(struct device *dev, size_t size,
				 dma_addr_t *dma_handle, gfp_t gfp,
				 unsigned long attrs)
{
	dev_warn_ratelimited(dev,
			     "vfmig_dma_ops: alloc(%zu) on tracked VF not routed yet (kcoherent arena is a follow-up)\n",
			     size);
	return NULL;
}

static void vfmig_dma_ops_free(struct device *dev, size_t size,
			       void *vaddr, dma_addr_t dma_handle,
			       unsigned long attrs)
{
	/* alloc never succeeds yet, so there is nothing to free. */
}

static const struct dma_map_ops vfmig_dma_ops = {
	.alloc			= vfmig_dma_ops_alloc,
	.free			= vfmig_dma_ops_free,

	.map_phys		= vfmig_dma_ops_map_phys,
	.unmap_phys		= vfmig_dma_ops_unmap_phys,
	.map_sg			= vfmig_dma_ops_map_sg,
	.unmap_sg		= vfmig_dma_ops_unmap_sg,

	.sync_single_for_cpu	= vfmig_dma_ops_sync_single_for_cpu,
	.sync_single_for_device	= vfmig_dma_ops_sync_single_for_device,
	.sync_sg_for_cpu	= vfmig_dma_ops_sync_sg_for_cpu,
	.sync_sg_for_device	= vfmig_dma_ops_sync_sg_for_device,

	.dma_supported		= vfmig_dma_ops_dma_supported,
	.get_required_mask	= vfmig_dma_ops_get_required_mask,
};

/* -------- attach / detach ----------------------------------------------- */

int vfmig_dma_ops_attach(struct pci_dev *vf_pdev,
			 struct vfmig_iova_domain *dom)
{
	struct vfmig_dma_ops_priv *priv;
	struct device *dev;
	void *old;
	int err;

	if (!vf_pdev || !dom)
		return -EINVAL;
	dev = &vf_pdev->dev;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dom		= dom;
#ifdef CONFIG_IOMMU_DMA
	priv->orig_dma_iommu	= dev_dma_iommu(dev);
#endif
	priv->orig_dma_ops	= dev->dma_ops;

	/*
	 * Insert into the xarray BEFORE flipping dma_ops / dma_iommu so
	 * the first dispatched callback can find @priv. xa_cmpxchg against
	 * NULL catches a double-attach as a non-NULL @old.
	 */
	old = xa_cmpxchg(&vfmig_dma_ops_xa, (unsigned long)dev, NULL, priv,
			 GFP_KERNEL);
	if (xa_is_err(old)) {
		err = xa_err(old);
		goto err_free;
	}
	if (old) {
		dev_warn(dev,
			 "vfmig_dma_ops: attach: device already has a shim attached (caller bug)\n");
		err = -EBUSY;
		goto err_free;
	}

	vfmig_dma_iommu_write(dev, false);
	set_dma_ops(dev, &vfmig_dma_ops);

	/*
	 * On architectures without CONFIG_ARCH_HAS_DMA_OPS, set_dma_ops is
	 * a no-op and get_dma_ops returns get_arch_dma_ops() rather than
	 * our pointer. Bail loudly so SET_TRACKED fails with a clear reason
	 * rather than the VF's DMA path silently bypassing us.
	 */
	if (get_dma_ops(dev) != &vfmig_dma_ops) {
		dev_warn(dev,
			 "vfmig_dma_ops: attach: set_dma_ops did not stick (CONFIG_ARCH_HAS_DMA_OPS=n?); DMA cannot be intercepted on this kernel\n");
		err = -EOPNOTSUPP;
		goto err_restore;
	}

	dev_info(dev,
		 "vfmig_dma_ops: attached on tracked VF (orig_dma_iommu=%d, orig_dma_ops=%pS)\n",
		 vfmig_dma_iommu_read(dev), priv->orig_dma_ops);
	return 0;

err_restore:
	set_dma_ops(dev, priv->orig_dma_ops);
#ifdef CONFIG_IOMMU_DMA
	vfmig_dma_iommu_write(dev, priv->orig_dma_iommu);
#endif
	xa_erase(&vfmig_dma_ops_xa, (unsigned long)dev);
err_free:
	kfree(priv);
	return err;
}

void vfmig_dma_ops_detach(struct pci_dev *vf_pdev)
{
	struct vfmig_dma_ops_priv *priv;
	struct device *dev;

	if (!vf_pdev)
		return;
	dev = &vf_pdev->dev;

	priv = xa_erase(&vfmig_dma_ops_xa, (unsigned long)dev);
	if (!priv) {
		dev_warn(dev,
			 "vfmig_dma_ops: detach without prior attach (caller bug)\n");
		return;
	}

	/*
	 * Restore in reverse order of attach: dma_ops first (otherwise a
	 * racing dma_map_sgtable could see vfmig_dma_ops with dma_iommu
	 * already restored and route through the iommu-dma path with our
	 * shim still installed), then dma_iommu. The device is unbound by
	 * the caller's contract so no concurrent DMA is in flight anyway.
	 */
	set_dma_ops(dev, priv->orig_dma_ops);
#ifdef CONFIG_IOMMU_DMA
	vfmig_dma_iommu_write(dev, priv->orig_dma_iommu);
#endif

	dev_info(dev,
		 "vfmig_dma_ops: detached (restored dma_iommu=%d, dma_ops=%pS)\n",
		 vfmig_dma_iommu_read(dev), priv->orig_dma_ops);
	kfree(priv);
}
