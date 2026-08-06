// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * vfmig_iova: per-VF unmanaged IOVA domain + deterministic slot
 * allocator. See vfmig_iova.h for the high-level rationale; this file
 * is the implementation. All entry points serialize on dom->lock; the
 * iommu_domain is reentrant under iommu_map / iommu_unmap, so no extra
 * serialization of the underlying iommu API is needed.
 */

#include <linux/align.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "vfmig_iova.h"

/*
 * One backing page (or higher-order compound page) registered in a
 * domain. @iova/@len identify the IOMMU mapping; @page/@vaddr are the
 * host-side handles. @len is always a multiple of PAGE_SIZE.
 *
 * @slot tags which IOVA sub-window the entry belongs to; @instance_key
 * is the per-slot identifier described in the vfmig_iova.h header doc.
 * Both are stamped by alloc_slot() at create time.
 */
struct vfmig_iova_page {
	struct list_head node;	/* dom->pages, sorted by iova ascending */
	u64		 iova;
	size_t		 len;
	struct page	*page;
	void		*vaddr;
	enum vfmig_iova_slot slot;
	u64		 instance_key;
};

/*
 * A per-VF unmanaged paging domain the PF driver fully owns, attached
 * in place of the VF's default DMA domain. @vf_pdev is pinned for the
 * domain's lifetime; @vf_id derives the IOVA window and labels log
 * lines. The deterministic range [base, base + NR_SLOTS * SLOT_BYTES)
 * is partitioned across the slot windows.
 */
struct vfmig_iova_domain {
	struct iommu_domain *iommu_dom;
	struct pci_dev	    *vf_pdev;	/* held via pci_dev_get() */
	u32		     vf_id;
	u64		     base;

	struct mutex	     lock;
	/*
	 * Per-slot bump cursor. cursor[N] is the next fresh IOVA in
	 * slot N's window, valid in [slot_base(N), slot_base(N+1)).
	 * Initialized to slot_base(N) at domain create.
	 */
	u64		     cursor[VFMIG_IOVA_NR_SLOTS];
	/*
	 * Per-slot auto-key counter, incremented on each alloc_slot
	 * call that passes instance_key == 0. Skipped when the caller
	 * passes a non-zero (caller-pinned) key.
	 */
	u64		     next_auto_key[VFMIG_IOVA_NR_SLOTS];

	struct list_head     pages;	/* of vfmig_iova_page, sorted */
	unsigned int	     n_pages;
};

static inline u64
vfmig_iova_slot_base(const struct vfmig_iova_domain *dom,
		     enum vfmig_iova_slot slot)
{
	return dom->base + (u64)slot * VFMIG_IOVA_SLOT_BYTES;
}

static inline u64
vfmig_iova_slot_end(const struct vfmig_iova_domain *dom,
		    enum vfmig_iova_slot slot)
{
	return dom->base + (u64)(slot + 1) * VFMIG_IOVA_SLOT_BYTES;
}

/* dom->lock held. Returns the entry mapped at exactly @iova, or NULL. */
static struct vfmig_iova_page *
vfmig_iova_find_locked(struct vfmig_iova_domain *dom, u64 iova)
{
	struct vfmig_iova_page *p;

	list_for_each_entry(p, &dom->pages, node) {
		if (p->iova == iova)
			return p;
		if (p->iova > iova)
			return NULL;	/* sorted: gone past it */
	}
	return NULL;
}

/* dom->lock held. Inserts @new keyed by iova; sorted ascending. */
static void
vfmig_iova_insert_locked(struct vfmig_iova_domain *dom,
			 struct vfmig_iova_page *new)
{
	struct vfmig_iova_page *p;

	list_for_each_entry(p, &dom->pages, node) {
		if (p->iova > new->iova) {
			list_add_tail(&new->node, &p->node);
			dom->n_pages++;
			return;
		}
	}
	list_add_tail(&new->node, &dom->pages);
	dom->n_pages++;
}

/*
 * dom->lock held. Allocate a backing page or higher-order compound,
 * iommu_map it at @iova for @len bytes, and append the registry entry.
 * Does NOT advance the cursor; callers do that themselves.
 *
 * @slot is used to validate that @iova falls inside that slot's window;
 * a callsite passing the wrong slot for an IOVA returns -ERANGE.
 *
 * Returns 0 with *out_p set on success, negative errno otherwise.
 */
static int
vfmig_iova_install_page_locked(struct vfmig_iova_domain *dom,
			       enum vfmig_iova_slot slot, u64 instance_key,
			       u64 iova, size_t len, gfp_t gfp,
			       struct vfmig_iova_page **out_p)
{
	struct vfmig_iova_page *p;
	unsigned int order;
	int err;

	if (!IS_ALIGNED(iova, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(len, VFMIG_IOVA_GRANULE) ||
	    len == 0)
		return -EINVAL;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR)
		return -EINVAL;
	if (iova < vfmig_iova_slot_base(dom, slot) ||
	    iova + len > vfmig_iova_slot_end(dom, slot))
		return -ERANGE;
	if (vfmig_iova_find_locked(dom, iova))
		return -EEXIST;

	/*
	 * iommu_map() rejects __GFP_HIGHMEM/COMP/DMA/DMA32 with WARN +
	 * -EINVAL, and we additionally need page_address() to work on
	 * the backing page. Reject the offending flags here with a clear
	 * errno so callers don't get a stack-trace-shaped surprise from
	 * the iommu layer.
	 */
	if (gfp & (__GFP_COMP | __GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM)) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: install_page: rejected gfp 0x%x (must not include __GFP_HIGHMEM/COMP/DMA/DMA32)\n",
				     gfp);
		return -EINVAL;
	}

	p = kzalloc(sizeof(*p), gfp);
	if (!p)
		return -ENOMEM;

	order = get_order(len);
	p->page = alloc_pages(gfp | __GFP_ZERO, order);
	if (!p->page) {
		err = -ENOMEM;
		goto err_free_p;
	}
	p->vaddr	= page_address(p->page);
	p->iova		= iova;
	p->len		= len;
	p->slot		= slot;
	p->instance_key	= instance_key;

	err = iommu_map(dom->iommu_dom, iova, page_to_phys(p->page), len,
			IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, gfp);
	if (err)
		goto err_free_page;

	vfmig_iova_insert_locked(dom, p);
	*out_p = p;
	return 0;

err_free_page:
	__free_pages(p->page, order);
err_free_p:
	kfree(p);
	return err;
}

/*
 * dom->lock held. Tear down a single registry entry: iommu_unmap,
 * release backing pages, free the bookkeeping struct. List unlink is
 * the caller's responsibility (so we can be called from list iteration).
 */
static void
vfmig_iova_destroy_page_locked(struct vfmig_iova_domain *dom,
			       struct vfmig_iova_page *p)
{
	(void)iommu_unmap(dom->iommu_dom, p->iova, p->len);
	if (p->page)
		__free_pages(p->page, get_order(p->len));
	kfree(p);
}

/* -------- exported API -------------------------------------------------- */

int vfmig_iova_domain_create(struct pci_dev *vf_pdev, u32 vf_id,
			     struct vfmig_iova_domain **out)
{
	struct vfmig_iova_domain *dom;
	struct iommu_domain *idom;
	u64 base, det_end;
	unsigned int s;
	int err;

	if (!vf_pdev || !out)
		return -EINVAL;
	if ((u64)vf_id >= U32_MAX / 2)	/* defensive: catch overflow */
		return -EINVAL;

	base = VFMIG_IOVA_BASE + (u64)vf_id * VFMIG_IOVA_PER_VF;
	if (base < VFMIG_IOVA_BASE)	/* wrapped */
		return -ERANGE;

	dom = kzalloc(sizeof(*dom), GFP_KERNEL);
	if (!dom)
		return -ENOMEM;

	mutex_init(&dom->lock);
	INIT_LIST_HEAD(&dom->pages);
	dom->vf_id = vf_id;
	dom->base  = base;

	/*
	 * Per-slot bump cursors: each starts at its slot's window base.
	 * Slot 0 (VFMIG_SLOT_INVALID) gets a cursor too, to keep the
	 * indexing trivial -- alloc_slot rejects SLOT_INVALID before it
	 * ever touches cursor[0]. next_auto_key[] is zero-initialized by
	 * kzalloc above; first auto-assignment yields key 1.
	 */
	for (s = 0; s < VFMIG_IOVA_NR_SLOTS; s++)
		dom->cursor[s] = vfmig_iova_slot_base(dom,
						      (enum vfmig_iova_slot)s);

	idom = iommu_paging_domain_alloc(&vf_pdev->dev);
	if (IS_ERR(idom)) {
		err = PTR_ERR(idom);
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: paging_domain_alloc failed: %d\n", err);
		goto err_free_dom;
	}
	dom->iommu_dom = idom;

	err = iommu_attach_device(dom->iommu_dom, &vf_pdev->dev);
	if (err) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: attach failed: %d\n", err);
		goto err_free_idom;
	}

	/*
	 * Validate that the deterministic IOVA window fits inside the
	 * IOMMU's geometry aperture. The underlying iommu driver picks
	 * aperture_end from the hardware address width (e.g. 39 bits on
	 * some Intel VT-d), and iommu_map() returns -ERANGE for any IOVA
	 * outside it. Catch the mismatch here so the failure surfaces at
	 * "set_tracked enable=1" with a printed reason rather than deep
	 * inside a later cmd-ring DMA.
	 */
	det_end = base + (u64)VFMIG_IOVA_NR_SLOTS * VFMIG_IOVA_SLOT_BYTES;
	if (dom->base < dom->iommu_dom->geometry.aperture_start ||
	    det_end - 1 > dom->iommu_dom->geometry.aperture_end) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u IOVA window [0x%llx, 0x%llx) does not fit IOMMU aperture [0x%llx, 0x%llx]\n",
			 vf_id, dom->base, det_end,
			 dom->iommu_dom->geometry.aperture_start,
			 dom->iommu_dom->geometry.aperture_end);
		err = -EOPNOTSUPP;
		goto err_detach;
	}

	dom->vf_pdev = pci_dev_get(vf_pdev);

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u domain attached, IOVA window [0x%llx, 0x%llx) (%u slots x 0x%llx) within IOMMU aperture [0x%llx, 0x%llx]\n",
		 vf_id, dom->base, det_end,
		 VFMIG_IOVA_NR_SLOTS, (u64)VFMIG_IOVA_SLOT_BYTES,
		 dom->iommu_dom->geometry.aperture_start,
		 dom->iommu_dom->geometry.aperture_end);

	*out = dom;
	return 0;

err_detach:
	iommu_detach_device(dom->iommu_dom, &vf_pdev->dev);
err_free_idom:
	iommu_domain_free(dom->iommu_dom);
err_free_dom:
	mutex_destroy(&dom->lock);
	kfree(dom);
	return err;
}

void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom)
{
	struct vfmig_iova_page *p, *tmp;
	struct pci_dev *vf_pdev;

	if (!dom)
		return;
	vf_pdev = dom->vf_pdev;

	mutex_lock(&dom->lock);
	list_for_each_entry_safe(p, tmp, &dom->pages, node) {
		list_del(&p->node);
		vfmig_iova_destroy_page_locked(dom, p);
	}
	dom->n_pages = 0;
	mutex_unlock(&dom->lock);

	if (vf_pdev) {
		iommu_detach_device(dom->iommu_dom, &vf_pdev->dev);
		dev_info(&vf_pdev->dev,
			 "vfmig_iova: vf %u domain detached and freed\n",
			 dom->vf_id);
	}
	iommu_domain_free(dom->iommu_dom);
	if (vf_pdev)
		pci_dev_put(vf_pdev);

	mutex_destroy(&dom->lock);
	kfree(dom);
}

int vfmig_iova_alloc_slot(struct vfmig_iova_domain *dom,
			  enum vfmig_iova_slot slot, u64 instance_key,
			  size_t size, gfp_t gfp,
			  dma_addr_t *iova_out, void **vaddr_out)
{
	struct vfmig_iova_page *p;
	size_t aligned;
	u64 iova, slot_end;
	int err;

	if (!dom || !iova_out || !vaddr_out || size == 0)
		return -EINVAL;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR)
		return -EINVAL;

	aligned = ALIGN(size, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);

	/*
	 * Auto-assign instance_key if the caller passed 0. Per-slot
	 * counter, so adding allocations in another slot doesn't perturb
	 * this slot's keys. Caller-pinned (non-zero) keys are recorded
	 * as-is and don't bump the counter.
	 */
	if (instance_key == 0)
		instance_key = ++dom->next_auto_key[slot];

	iova = dom->cursor[slot];
	slot_end = vfmig_iova_slot_end(dom, slot);
	if (iova + aligned > slot_end) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u slot %u exhausted at cursor 0x%llx (slot_end 0x%llx, asked %zu)\n",
				     dom->vf_id, slot, iova, slot_end,
				     aligned);
		err = -ENOSPC;
		goto out_unlock;
	}

	err = vfmig_iova_install_page_locked(dom, slot, instance_key,
					     iova, aligned, gfp, &p);
	if (err)
		goto out_unlock;

	dom->cursor[slot] = iova + aligned;
	*iova_out  = p->iova;
	*vaddr_out = p->vaddr;
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

void vfmig_iova_free_slot(struct vfmig_iova_domain *dom,
			  enum vfmig_iova_slot slot,
			  dma_addr_t iova, size_t size)
{
	struct vfmig_iova_page *p;
	size_t aligned;

	if (!dom)
		return;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: free_slot: bad slot %u for IOVA 0x%llx\n",
			 slot, (u64)iova);
		return;
	}
	aligned = ALIGN(size, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);
	p = vfmig_iova_find_locked(dom, iova);
	if (!p) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u free of unknown IOVA 0x%llx (slot %u)\n",
			 dom->vf_id, (u64)iova, slot);
		goto out_unlock;
	}
	WARN_ON_ONCE(p->slot != slot);
	if (p->len != aligned) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u free size mismatch at IOVA 0x%llx (slot %u): have %zu, asked %zu\n",
			 dom->vf_id, (u64)iova, slot, p->len, aligned);
		/* still proceed: the entry is what it is */
	}
	list_del(&p->node);
	dom->n_pages--;
	vfmig_iova_destroy_page_locked(dom, p);

out_unlock:
	mutex_unlock(&dom->lock);
}
