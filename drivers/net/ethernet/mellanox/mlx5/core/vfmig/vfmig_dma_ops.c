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
 * must restore on detach, the per-VF iommu_domain the mappings land in,
 * and the shim-private kcoherent arena. The xarray is small (one entry
 * per tracked VF, bounded by the SR-IOV VF count). dev is stable for the
 * (attach, detach) interval -- the caller holds the VF's device_lock and
 * the VF is unbound throughout.
 *
 * dev->dma_iommu override
 * -----------------------
 * __dma_map_sg_attrs() checks use_dma_iommu(dev) -> dev->dma_iommu
 * BEFORE falling through to ops->map_sg. If we don't clear that flag our
 * ops are silently bypassed. The original value is saved at attach
 * (@orig_dma_iommu) and restored on detach so dma-iommu transparently
 * resumes ownership when the VF is untracked.
 *
 * kcoherent arena
 * ---------------
 * The shim owns a private bump allocator carved from the bottom of the
 * VF's USER_PAGE IOVA range (vfmig_iova_kcoherent_window()). It backs
 * dma_alloc_coherent() on a tracked VF: kernel-owned, physically
 * contiguous, IOMMU_CACHE memory whose IOVA is NOT recorded in any SAVE
 * manifest (the destination re-probes and gets its own). O(1) bump +
 * one tracking node per outstanding allocation; drained at detach.
 *
 * Routing (this patch)
 * --------------------
 *   - .alloc / .free        -> kcoherent arena (dma_alloc_coherent).
 *   - .map_phys/.unmap_phys -> kcoherent arena, registry-less streaming
 *                              maps (dma_map_page/single).
 *   - .map_sg / .unmap_sg   -> migration-tracked USER_PAGE registry,
 *                              deferred: fail cleanly until it lands.
 *   - .sync_* / .dma_supported / .get_required_mask are final.
 */

#include <linux/align.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/gfp.h>
#include <linux/iommu.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>

#include "vfmig_dma_ops.h"
#include "vfmig_iova.h"

/*
 * One outstanding kcoherent allocation. Lives on @priv->pages until
 * vfmig_dma_ops_free() unlinks it, or vfmig_kcoherent_drain() reclaims
 * it at detach.
 */
struct vfmig_kcoherent_page {
	struct list_head	node;
	u64			iova;
	size_t			len;
	void			*vaddr;
};

/*
 * Per-attach saved state, in the xarray keyed by (unsigned long)dev.
 * @orig_* are the pre-attach values undone on detach; @iommu_dom is the
 * per-VF domain the callbacks map into; @dom is the owning vfmig IOVA
 * domain, used by .map_sg to route umem pages into the USER_PAGE slot;
 * the kcoherent bump arena (@base/@end/@cursor/@pages/@n_pages) is
 * guarded by @lock.
 */
struct vfmig_dma_ops_priv {
	struct vfmig_iova_domain	*dom;
	struct iommu_domain		*iommu_dom;
	const struct dma_map_ops	*orig_dma_ops;
#ifdef CONFIG_IOMMU_DMA
	bool				 orig_dma_iommu;
#endif

	spinlock_t			 lock;	/* guards the arena below */
	u64				 base;
	u64				 end;
	u64				 cursor;
	struct list_head		 pages;
	unsigned int			 n_pages;
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
	return dev->dma_iommu;
#else
	return false;
#endif
}

static inline void vfmig_dma_iommu_write(struct device *dev, bool val)
{
#ifdef CONFIG_IOMMU_DMA
	dev->dma_iommu = val;
#else
	(void)dev;
	(void)val;
#endif
}

static DEFINE_XARRAY(vfmig_dma_ops_xa);

static inline struct vfmig_dma_ops_priv *
vfmig_dma_ops_priv_get(struct device *dev)
{
	return xa_load(&vfmig_dma_ops_xa, (unsigned long)dev);
}

/* The vfmig IOVA domain backing @dev's shim, or NULL if not attached. */
static inline struct vfmig_iova_domain *
vfmig_dma_ops_dom_for(struct device *dev)
{
	struct vfmig_dma_ops_priv *priv = vfmig_dma_ops_priv_get(dev);

	return priv ? priv->dom : NULL;
}

/* -------- kcoherent bump arena (private to the shim) -------------------- */

/* Held @priv->lock. Locate the entry mapped at exactly @iova, or NULL. */
static struct vfmig_kcoherent_page *
vfmig_kcoherent_find_locked(struct vfmig_dma_ops_priv *priv, u64 iova)
{
	struct vfmig_kcoherent_page *kp;

	list_for_each_entry(kp, &priv->pages, node) {
		if (kp->iova == iova)
			return kp;
	}
	return NULL;
}

/*
 * Reserve @aligned bytes at the cursor. Returns the reserved IOVA, or
 * U64_MAX on exhaustion so the caller can release its pre-allocated
 * backing pages without holding the lock to test.
 */
static u64 vfmig_kcoherent_reserve(struct vfmig_dma_ops_priv *priv,
				   size_t aligned)
{
	unsigned long flags;
	u64 iova;

	spin_lock_irqsave(&priv->lock, flags);
	if (priv->cursor + aligned > priv->end) {
		spin_unlock_irqrestore(&priv->lock, flags);
		return U64_MAX;
	}
	iova = priv->cursor;
	priv->cursor += aligned;
	spin_unlock_irqrestore(&priv->lock, flags);
	return iova;
}

/*
 * Tear down every outstanding kcoherent allocation. Called from detach
 * with the VF unbound (caller contract) and the domain still attached,
 * so iommu_unmap is valid and no concurrent traffic is possible; the
 * lock need not be held. A non-empty list is a driver leak and warns.
 */
static void vfmig_kcoherent_drain(struct device *dev,
				  struct vfmig_dma_ops_priv *priv)
{
	struct vfmig_kcoherent_page *kp, *tmp;

	if (priv->n_pages > 0)
		dev_warn(dev,
			 "vfmig_dma_ops: %u kcoherent allocations outstanding at detach (driver leak); cleaning up\n",
			 priv->n_pages);

	list_for_each_entry_safe(kp, tmp, &priv->pages, node) {
		(void)iommu_unmap(priv->iommu_dom, kp->iova, kp->len);
		free_pages_exact(kp->vaddr, kp->len);
		list_del(&kp->node);
		kfree(kp);
	}
	priv->n_pages = 0;
}

/*
 * Map @aligned bytes of caller-owned, page-aligned @phys into the arena
 * at a fresh bump IOVA. Registry-less: streaming maps are high-rate and
 * their IOVAs are never migrated, so there is no per-map tracking node
 * (unmap is by range). Returns 0 and *@iova_out, or a negative errno.
 */
static int vfmig_kcoherent_map_phys(struct vfmig_dma_ops_priv *priv,
				    phys_addr_t phys, size_t aligned,
				    gfp_t gfp, u64 *iova_out)
{
	gfp_t gfp_iommu;
	u64 iova;
	int err;

	if (!IS_ALIGNED(phys, PAGE_SIZE))
		return -EINVAL;

	/*
	 * Strip flags iommu_map rejects; keep the atomicity bits since
	 * streaming callers frequently map from softirq with GFP_ATOMIC.
	 * No __GFP_ZERO: @phys is caller-owned and already populated.
	 */
	gfp_iommu = gfp & ~(__GFP_HIGHMEM | __GFP_COMP |
			    __GFP_DMA | __GFP_DMA32);

	iova = vfmig_kcoherent_reserve(priv, aligned);
	if (iova == U64_MAX)
		return -ENOSPC;

	err = iommu_map(priv->iommu_dom, iova, phys, aligned,
			IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, gfp_iommu);
	if (err)
		return err;	/* iova leaks; bump-only arena */

	*iova_out = iova;
	return 0;
}

/* -------- dma_map_ops callbacks ----------------------------------------- */

/*
 * dma_map_phys / dma_map_single / dma_map_page land here. On a tracked
 * VF these are mlx5e's streaming RX/TX buffers (page_pool dma_map_page
 * per RX buffer, dma_map_single per TX skb fragment). Route to the
 * kcoherent arena: the IOVA is not migrated (the destination re-maps
 * fresh) and the bump path is O(1), which matters because ndo_open
 * posts thousands of RX WQEs in tight succession.
 *
 * DMA_ATTR_MMIO marks a peer-to-peer mapping of MMIO BAR space rather
 * than system memory; its IOVA isn't reconstructible across hosts (BAR
 * bases differ), so reject it rather than map something un-migratable.
 */
static dma_addr_t vfmig_dma_ops_map_phys(struct device *dev, phys_addr_t phys,
					 size_t size,
					 enum dma_data_direction dir,
					 unsigned long attrs)
{
	struct vfmig_dma_ops_priv *priv = vfmig_dma_ops_priv_get(dev);
	unsigned int off;
	u64 iova;
	int err;

	if (unlikely(!priv))
		return DMA_MAPPING_ERROR;

	if (attrs & DMA_ATTR_MMIO) {
		dev_warn_ratelimited(dev,
				     "vfmig_dma_ops: map_phys with DMA_ATTR_MMIO not supported (peer-to-peer dma-buf out of scope)\n");
		return DMA_MAPPING_ERROR;
	}

	off = phys & ~PAGE_MASK;
	err = vfmig_kcoherent_map_phys(priv, phys - off,
				       PAGE_ALIGN(size + off), GFP_ATOMIC,
				       &iova);
	if (err) {
		dev_warn_ratelimited(dev,
				     "vfmig_dma_ops: map_phys(0x%llx, %zu) failed: %d\n",
				     (u64)phys, size, err);
		return DMA_MAPPING_ERROR;
	}
	return iova + off;
}

static void vfmig_dma_ops_unmap_phys(struct device *dev, dma_addr_t handle,
				     size_t size,
				     enum dma_data_direction dir,
				     unsigned long attrs)
{
	struct vfmig_dma_ops_priv *priv = vfmig_dma_ops_priv_get(dev);
	unsigned int off;
	u64 iova;
	size_t aligned;

	if (unlikely(!priv))
		return;
	if (attrs & DMA_ATTR_MMIO)
		return;	/* never mapped, see map_phys */

	off     = handle & ~PAGE_MASK;
	iova    = handle - off;
	aligned = PAGE_ALIGN(size + off);

	/*
	 * Range-check against the arena window so a stale handle from
	 * another path (a kernel slot, transient) can't unmap here.
	 */
	if (iova < priv->base || iova + aligned > priv->end) {
		dev_warn_ratelimited(dev,
				     "vfmig_dma_ops: unmap_phys: IOVA 0x%llx + 0x%zx outside kcoherent window [0x%llx, 0x%llx); ignoring\n",
				     iova, aligned, priv->base, priv->end);
		return;
	}
	(void)iommu_unmap(priv->iommu_dom, iova, aligned);
}

/*
 * dma_map_sg / dma_map_sgtable land here for tracked VFs -- the
 * ib_umem_get user-MR / CQ / QP / SRQ path. Each inbound @sg segment is
 * a contiguous run of umem-pinned pages; we allocate one IOVA range per
 * segment from the VFMIG_SLOT_USER_PAGE window via
 * vfmig_iova_user_page_map_phys(), which iommu_maps the segment's phys
 * and records an external registry entry. The registry tracking is what
 * lets a later patch snapshot these mappings into the SAVE blob and
 * replay them at stable IOVAs on the destination.
 *
 * dma_map_sgtable contract: return @nents on success (we do not coalesce
 * physically-contiguous segments at this stage), a negative errno on
 * failure. On partial failure, unwind the segments mapped so far. The
 * DMA core calls ops->map_sg without a NULL guard once use_dma_iommu()
 * is false, so this callback must exist.
 */
static int vfmig_dma_ops_map_sg(struct device *dev, struct scatterlist *sg,
				int nents, enum dma_data_direction dir,
				unsigned long attrs)
{
	struct vfmig_iova_domain *dom = vfmig_dma_ops_dom_for(dev);
	struct scatterlist *s;
	int i, mapped = 0;
	int err;

	if (unlikely(!dom))
		return -EIO;

	for_each_sg(sg, s, nents, i) {
		phys_addr_t phys = sg_phys(s);
		unsigned int off = s->offset & ~PAGE_MASK;
		unsigned int len = s->length;
		dma_addr_t iova;

		/*
		 * ib_umem_get hands us page-aligned segments; round @phys
		 * down and @len up so a caller supplying a mid-page offset
		 * still gets a well-defined mapping. The returned
		 * dma_address carries the original byte offset back.
		 */
		err = vfmig_iova_user_page_map_phys(dom, phys & PAGE_MASK,
						    PAGE_ALIGN(len + off),
						    GFP_ATOMIC, &iova);
		if (err)
			goto err_undo;

		sg_dma_address(s) = iova + off;
		sg_dma_len(s)	  = len;
		mapped++;
	}

	return nents;

err_undo:
	for_each_sg(sg, s, mapped, i) {
		dma_addr_t iova = sg_dma_address(s);
		unsigned int off = iova & ~PAGE_MASK;
		unsigned int len = sg_dma_len(s);

		(void)vfmig_iova_user_page_unmap_phys(dom, iova - off,
						      PAGE_ALIGN(len + off));
		sg_dma_address(s) = 0;
		sg_dma_len(s)	  = 0;
	}
	dev_warn_ratelimited(dev,
			     "vfmig_dma_ops: map_sg failed at entry %d/%d: %d\n",
			     mapped, nents, err);
	return err;
}

static void vfmig_dma_ops_unmap_sg(struct device *dev, struct scatterlist *sg,
				   int nents, enum dma_data_direction dir,
				   unsigned long attrs)
{
	struct vfmig_iova_domain *dom = vfmig_dma_ops_dom_for(dev);
	struct scatterlist *s;
	int i;

	if (unlikely(!dom))
		return;

	for_each_sg(sg, s, nents, i) {
		dma_addr_t iova = sg_dma_address(s);
		unsigned int len = sg_dma_len(s);
		unsigned int off;

		if (!iova && !len)
			continue;	/* never mapped (partial map_sg) */

		off = iova & ~PAGE_MASK;
		(void)vfmig_iova_user_page_unmap_phys(dom, iova - off,
						      PAGE_ALIGN(len + off));
		sg_dma_address(s) = 0;
		sg_dma_len(s)	  = 0;
	}
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
 * CQ buffer setup). Backing pages are kernel-owned, physically
 * contiguous and mapped into the per-VF domain from the kcoherent
 * arena; the IOVA is not part of any SAVE manifest. @attrs is ignored:
 * the arena always returns a kernel-virtual, IOMMU_CACHE region, the
 * strongest coherent contract, which satisfies every kernel caller seen
 * so far.
 */
static void *vfmig_dma_ops_alloc(struct device *dev, size_t size,
				 dma_addr_t *dma_handle, gfp_t gfp,
				 unsigned long attrs)
{
	struct vfmig_dma_ops_priv *priv = vfmig_dma_ops_priv_get(dev);
	struct vfmig_kcoherent_page *kp;
	size_t aligned;
	gfp_t gfp_pages;
	unsigned long flags;
	u64 iova;
	void *vaddr;
	int err;

	if (unlikely(!priv)) {
		dev_warn_ratelimited(dev,
				     "vfmig_dma_ops: alloc(%zu) with no shim state (caller bug)\n",
				     size);
		return NULL;
	}
	if (!size)
		return NULL;

	aligned = ALIGN(size, PAGE_SIZE);

	/*
	 * Sanitize gfp for alloc_pages_exact + iommu_map + page_address:
	 * no highmem (page_address must be valid), no compound, no
	 * DMA-zone constraints (the IOMMU provides translation).
	 * __GFP_ZERO matches dma_alloc_coherent semantics.
	 */
	gfp_pages = (gfp & ~(__GFP_HIGHMEM | __GFP_COMP |
			     __GFP_DMA | __GFP_DMA32)) | __GFP_ZERO;

	/* Backing allocations happen OUTSIDE the cursor spinlock. */
	kp = kzalloc(sizeof(*kp), gfp_pages);
	if (!kp)
		return NULL;

	vaddr = alloc_pages_exact(aligned, gfp_pages);
	if (!vaddr)
		goto err_free_kp;

	iova = vfmig_kcoherent_reserve(priv, aligned);
	if (iova == U64_MAX) {
		dev_warn_ratelimited(dev,
				     "vfmig_dma_ops: kcoherent alloc exhausted (asked %zu, end 0x%llx)\n",
				     aligned, priv->end);
		goto err_free_pages;
	}

	err = iommu_map(priv->iommu_dom, iova, virt_to_phys(vaddr), aligned,
			IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, GFP_ATOMIC);
	if (err) {
		dev_warn_ratelimited(dev,
				     "vfmig_dma_ops: kcoherent iommu_map(0x%llx, %zu) failed: %d\n",
				     iova, aligned, err);
		goto err_free_pages;	/* iova leaks; bump-only arena */
	}

	kp->iova  = iova;
	kp->len   = aligned;
	kp->vaddr = vaddr;

	spin_lock_irqsave(&priv->lock, flags);
	list_add_tail(&kp->node, &priv->pages);
	priv->n_pages++;
	spin_unlock_irqrestore(&priv->lock, flags);

	*dma_handle = iova;
	return vaddr;

err_free_pages:
	free_pages_exact(vaddr, aligned);
err_free_kp:
	kfree(kp);
	return NULL;
}

static void vfmig_dma_ops_free(struct device *dev, size_t size,
			       void *vaddr, dma_addr_t dma_handle,
			       unsigned long attrs)
{
	struct vfmig_dma_ops_priv *priv = vfmig_dma_ops_priv_get(dev);
	struct vfmig_kcoherent_page *kp;
	struct vfmig_kcoherent_page found;
	unsigned long flags;
	size_t aligned;

	if (unlikely(!priv))
		return;

	aligned = ALIGN(size, PAGE_SIZE);

	spin_lock_irqsave(&priv->lock, flags);
	kp = vfmig_kcoherent_find_locked(priv, (u64)dma_handle);
	if (!kp) {
		spin_unlock_irqrestore(&priv->lock, flags);
		dev_warn_ratelimited(dev,
				     "vfmig_dma_ops: free: no kcoherent entry at IOVA 0x%llx (asked %zu); ignoring\n",
				     (u64)dma_handle, aligned);
		return;
	}
	if (kp->len != aligned)
		dev_warn_ratelimited(dev,
				     "vfmig_dma_ops: free size mismatch at IOVA 0x%llx: have %zu, asked %zu; using recorded size\n",
				     (u64)dma_handle, kp->len, aligned);
	if (vaddr && vaddr != kp->vaddr)
		dev_warn_ratelimited(dev,
				     "vfmig_dma_ops: free vaddr mismatch at IOVA 0x%llx: have %p, asked %p\n",
				     (u64)dma_handle, kp->vaddr, vaddr);

	/*
	 * Snapshot + unlink under the spinlock; the iommu_unmap and
	 * free_pages_exact run outside it so they can sleep freely.
	 */
	found = *kp;
	list_del(&kp->node);
	priv->n_pages--;
	spin_unlock_irqrestore(&priv->lock, flags);

	kfree(kp);
	(void)iommu_unmap(priv->iommu_dom, found.iova, found.len);
	free_pages_exact(found.vaddr, found.len);
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
	u64 win_len;
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
	priv->orig_dma_iommu	= dev->dma_iommu;
#endif
	priv->orig_dma_ops	= dev->dma_ops;

	err = vfmig_iova_kcoherent_window(dom, &priv->iommu_dom,
					  &priv->base, &win_len);
	if (err)
		goto err_free;
	priv->end    = priv->base + win_len;
	priv->cursor = priv->base;
	spin_lock_init(&priv->lock);
	INIT_LIST_HEAD(&priv->pages);

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
		 "vfmig_dma_ops: attached on tracked VF (kcoherent [0x%llx, 0x%llx), orig_dma_iommu=%d, orig_dma_ops=%pS)\n",
		 priv->base, priv->end, vfmig_dma_iommu_read(dev),
		 priv->orig_dma_ops);
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

	/*
	 * Reclaim any outstanding kcoherent mappings while the domain is
	 * still attached (the caller detaches/frees it after we return).
	 */
	vfmig_kcoherent_drain(dev, priv);

	dev_info(dev,
		 "vfmig_dma_ops: detached (restored dma_iommu=%d, dma_ops=%pS)\n",
		 vfmig_dma_iommu_read(dev), priv->orig_dma_ops);
	kfree(priv);
}
