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
 * Maximum number of pages the per-VF transient arena can grow to.
 * Sized from VFMIG_IOVA_TRANSIENT_BYTES; fixed at compile time so the
 * arena's by-index slot table can be a flat array.
 */
#define VFMIG_IOVA_TRANSIENT_MAX_PAGES \
	(VFMIG_IOVA_TRANSIENT_BYTES / PAGE_SIZE)

/*
 * One backing page in the transient arena. Lives in one of two states:
 * on dom->transient.free (available for the next transient_get) or off
 * the list with arena->slots[idx] still pointing at it (handed out).
 *
 * The IOMMU mapping is set up exactly once when the page is first grown
 * into the arena; transient_get/put never call iommu_map / iommu_unmap
 * on the hot path. Pages are only unmapped at domain_destroy time.
 */
struct vfmig_transient_page {
	struct list_head free_node;	/* on arena->free when free */
	u64		 iova;
	void		*vaddr;
	struct page	*page;
};

/*
 * Per-domain transient arena: the topmost VFMIG_IOVA_TRANSIENT_BYTES of
 * the per-VF IOVA window, [base, end) with end == dom->base + PER_VF.
 * Lazily populated: pages are mapped from the cursor on the first _get()
 * that finds the freelist empty, up to VFMIG_IOVA_TRANSIENT_MAX_PAGES.
 * Once mapped, pages stay mapped for the lifetime of the domain and are
 * recycled via the freelist. Protected by dom->lock.
 *
 * The arena backs short-lived, single-page, non-migrated allocations
 * (cmd mailbox blocks): IOVAs here are never recorded in the SAVE
 * manifest and need no source/destination determinism.
 */
struct vfmig_transient_arena {
	u64		 base;
	u64		 end;
	u64		 cursor;	/* next IOVA to map on grow */
	struct list_head free;		/* of vfmig_transient_page */
	struct vfmig_transient_page **slots;	/* by-index lookup */
	unsigned int	 n_mapped;	/* total pages currently mapped */
	unsigned int	 n_free;	/* len of @free, for diagnostics */
	unsigned int	 max_pages;	/* arena ceiling, in pages */
};

/*
 * A per-VF unmanaged paging domain the PF driver fully owns, attached
 * in place of the VF's default DMA domain. @vf_pdev is pinned for the
 * domain's lifetime; @vf_id derives the IOVA window and labels log
 * lines. The deterministic range [base, base + NR_SLOTS * SLOT_BYTES)
 * is partitioned across the slot windows; the topmost
 * VFMIG_IOVA_TRANSIENT_BYTES of the per-VF window is the transient
 * arena (cmd mailboxes).
 */
struct vfmig_iova_domain {
	struct iommu_domain *iommu_dom;
	struct pci_dev	    *vf_pdev;	/* held via pci_dev_get() */
	u32		     vf_id;

	/*
	 * Set by vfmig_iova_domain_detach_dev() once the iommu_dom has
	 * been detached from @vf_pdev. Guards vfmig_iova_domain_destroy()
	 * from doing the detach a second time. The split exists because
	 * the iommu_dom attach MUST be torn down before pci_disable_sriov()
	 * fires device_del on the VF (otherwise the iommu core's
	 * BUS_NOTIFY_REMOVED_DEVICE notifier WARNs -- the per-VF group goes
	 * empty while still holding our unmanaged domain instead of the
	 * default), but the rest of the domain teardown (page-list drain,
	 * kfree) must run AFTER pci_disable_sriov returns because per-VF
	 * mlx5_core remove_one paths free DMA mappings through
	 * vfmig_iova_free_slot() which still derefs @dom. Splitting
	 * "detach from device" from "destroy domain" lets both hold.
	 */
	bool		     dev_detached;

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

	struct vfmig_transient_arena transient;
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

/*
 * Inverse of vfmig_iova_slot_base(): which slot does @iova fall into, or
 * VFMIG_SLOT_INVALID if it is outside the deterministic slot range (below
 * dom->base, or at/above the transient arena). Used by replay to
 * cross-check that a wire record's claimed slot agrees with the
 * destination's own IOVA partitioning.
 */
static enum vfmig_iova_slot
vfmig_iova_slot_from_iova(const struct vfmig_iova_domain *dom, u64 iova)
{
	u64 idx;

	if (iova < dom->base || iova >= dom->transient.base)
		return VFMIG_SLOT_INVALID;
	idx = (iova - dom->base) / VFMIG_IOVA_SLOT_BYTES;
	if (idx <= VFMIG_SLOT_INVALID || idx >= VFMIG_SLOT_NR)
		return VFMIG_SLOT_INVALID;
	return (enum vfmig_iova_slot)idx;
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

/* -------- transient arena ----------------------------------------------- */

/*
 * dom->lock held. Grow the arena by one page: alloc_pages, iommu_map at
 * the next cursor IOVA, install in slots[], return the new descriptor
 * (NOT on the freelist; caller hands it to its requester directly).
 */
static struct vfmig_transient_page *
vfmig_transient_grow_locked(struct vfmig_iova_domain *dom, gfp_t gfp)
{
	struct vfmig_transient_arena *a = &dom->transient;
	struct vfmig_transient_page *tp;
	unsigned int idx;
	int err;

	if (a->n_mapped >= a->max_pages)
		return ERR_PTR(-ENOMEM);

	tp = kzalloc(sizeof(*tp), gfp);
	if (!tp)
		return ERR_PTR(-ENOMEM);
	INIT_LIST_HEAD(&tp->free_node);	/* enables list_empty() double-free
					 * detection in transient_put() */

	tp->page = alloc_pages(gfp | __GFP_ZERO, 0);
	if (!tp->page) {
		kfree(tp);
		return ERR_PTR(-ENOMEM);
	}
	tp->vaddr = page_address(tp->page);
	tp->iova  = a->cursor;

	err = iommu_map(dom->iommu_dom, tp->iova, page_to_phys(tp->page),
			PAGE_SIZE, IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE,
			gfp);
	if (err) {
		__free_pages(tp->page, 0);
		kfree(tp);
		return ERR_PTR(err);
	}

	idx = (tp->iova - a->base) >> PAGE_SHIFT;
	a->slots[idx] = tp;
	a->cursor    += PAGE_SIZE;
	a->n_mapped++;

	return tp;
}

/*
 * dom->lock held. Tear down every page in the transient arena: walk
 * arena->slots[], iommu_unmap each mapped page, free the backing page
 * and the bookkeeping. Drains via slots[] rather than the freelist so a
 * leaked (never _put()) page is still torn down. Does not free the
 * slots[] array itself (the caller does).
 */
static void vfmig_transient_drain_locked(struct vfmig_iova_domain *dom)
{
	struct vfmig_transient_arena *a = &dom->transient;
	unsigned int i;

	if (!a->slots)
		return;
	for (i = 0; i < a->max_pages; i++) {
		struct vfmig_transient_page *tp = a->slots[i];

		if (!tp)
			continue;
		(void)iommu_unmap(dom->iommu_dom, tp->iova, PAGE_SIZE);
		__free_pages(tp->page, 0);
		kfree(tp);
		a->slots[i] = NULL;
	}
	INIT_LIST_HEAD(&a->free);
	a->n_mapped = 0;
	a->n_free   = 0;
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

	/*
	 * Transient arena owns the topmost VFMIG_IOVA_TRANSIENT_BYTES of the
	 * per-VF window, [base + PER_VF - TRANSIENT_BYTES, base + PER_VF).
	 * The static_assert in vfmig_iova.h guarantees it does not overlap
	 * the deterministic slot range. Pages are mapped lazily on demand.
	 */
	INIT_LIST_HEAD(&dom->transient.free);
	dom->transient.base      = base + VFMIG_IOVA_PER_VF -
				   VFMIG_IOVA_TRANSIENT_BYTES;
	dom->transient.end       = base + VFMIG_IOVA_PER_VF;
	dom->transient.cursor    = dom->transient.base;
	dom->transient.max_pages = VFMIG_IOVA_TRANSIENT_MAX_PAGES;
	dom->transient.slots = kcalloc(dom->transient.max_pages,
				       sizeof(*dom->transient.slots),
				       GFP_KERNEL);
	if (!dom->transient.slots) {
		err = -ENOMEM;
		goto err_free_dom;
	}

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
	 * Validate that the full IOVA window (deterministic slots +
	 * transient arena) fits inside the IOMMU's geometry aperture. The
	 * underlying iommu driver picks aperture_end from the hardware
	 * address width (e.g. 39 bits on some Intel VT-d), and iommu_map()
	 * returns -ERANGE for any IOVA outside it. Catch the mismatch here
	 * so the failure surfaces at "set_tracked enable=1" with a printed
	 * reason rather than deep inside a later cmd-ring DMA.
	 */
	det_end = base + (u64)VFMIG_IOVA_NR_SLOTS * VFMIG_IOVA_SLOT_BYTES;
	if (dom->base < dom->iommu_dom->geometry.aperture_start ||
	    dom->transient.end - 1 > dom->iommu_dom->geometry.aperture_end) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u IOVA window [0x%llx, 0x%llx) does not fit IOMMU aperture [0x%llx, 0x%llx]\n",
			 vf_id, dom->base, dom->transient.end,
			 dom->iommu_dom->geometry.aperture_start,
			 dom->iommu_dom->geometry.aperture_end);
		err = -EOPNOTSUPP;
		goto err_detach;
	}

	dom->vf_pdev = pci_dev_get(vf_pdev);

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u domain attached, IOVA window [0x%llx, 0x%llx) (%u slots x 0x%llx) + [0x%llx, 0x%llx) (transient) within IOMMU aperture [0x%llx, 0x%llx]\n",
		 vf_id, dom->base, det_end,
		 VFMIG_IOVA_NR_SLOTS, (u64)VFMIG_IOVA_SLOT_BYTES,
		 dom->transient.base, dom->transient.end,
		 dom->iommu_dom->geometry.aperture_start,
		 dom->iommu_dom->geometry.aperture_end);

	*out = dom;
	return 0;

err_detach:
	iommu_detach_device(dom->iommu_dom, &vf_pdev->dev);
err_free_idom:
	iommu_domain_free(dom->iommu_dom);
err_free_dom:
	kfree(dom->transient.slots);
	mutex_destroy(&dom->lock);
	kfree(dom);
	return err;
}

/*
 * Detach the per-VF iommu_dom from the VF's PCI device, leaving the
 * domain struct (page list, transient slots, iommu_dom pointer) intact
 * so vfmig_iova_free_slot() still works for any teardown DMA that races
 * after the detach. Idempotent: a second call is a no-op.
 *
 * Required call ordering for the SR-IOV teardown path:
 *
 *   pci_disable_sriov(pf_pdev)             // tears down each VF:
 *     for each bound vf:
 *       device_release_driver(vf)
 *         mlx5_core remove_one(vf)
 *           ... FW commands, EQ drain, DMA frees ...
 *           [HOOK] vfmig_iova_domain_detach_dev(dom_for_this_vf)
 *       pci_remove_bus_device(vf)
 *         device_del(vf)                   // iommu core: no WARN
 *   mlx5_vfmig_pf_drop_iova_domains(pf)    // frees the domain structs
 *       vfmig_iova_domain_destroy(dom)     // drain pages, skip detach
 */
void vfmig_iova_domain_detach_dev(struct vfmig_iova_domain *dom)
{
	struct pci_dev *vf_pdev;

	if (!dom || dom->dev_detached)
		return;
	vf_pdev = dom->vf_pdev;
	if (!vf_pdev)
		return;

	iommu_detach_device(dom->iommu_dom, &vf_pdev->dev);
	dom->dev_detached = true;

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u domain detached from device (struct kept for cleanup)\n",
		 dom->vf_id);
}

void vfmig_iova_domain_detach_dev_if_unbound(struct vfmig_iova_domain *dom)
{
	if (!dom || dom->dev_detached || !dom->vf_pdev)
		return;
	if (dom->vf_pdev->driver)
		return;
	vfmig_iova_domain_detach_dev(dom);
}

void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom)
{
	struct vfmig_iova_page *p, *tmp;
	struct pci_dev *vf_pdev;

	if (!dom)
		return;
	vf_pdev = dom->vf_pdev;

	mutex_lock(&dom->lock);
	vfmig_transient_drain_locked(dom);
	list_for_each_entry_safe(p, tmp, &dom->pages, node) {
		list_del(&p->node);
		vfmig_iova_destroy_page_locked(dom, p);
	}
	dom->n_pages = 0;
	mutex_unlock(&dom->lock);

	if (vf_pdev) {
		/*
		 * If the caller already invoked
		 * vfmig_iova_domain_detach_dev() during the VF's mlx5_core
		 * remove_one (bound VFs) or the PF-side detach-unbound pass
		 * (never-bound VFs), this is a no-op; otherwise do the
		 * detach now.
		 */
		if (!dom->dev_detached) {
			iommu_detach_device(dom->iommu_dom, &vf_pdev->dev);
			dom->dev_detached = true;
		}
		dev_info(&vf_pdev->dev,
			 "vfmig_iova: vf %u domain detached and freed\n",
			 dom->vf_id);
	}
	iommu_domain_free(dom->iommu_dom);
	if (vf_pdev)
		pci_dev_put(vf_pdev);

	kfree(dom->transient.slots);
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

	/*
	 * Lookup-or-alloc at the per-slot cursor. An entry already mapped
	 * at @iova came from a prior vfmig_iova_replay_page(): the
	 * destination's probe is re-claiming a page the source snapshotted,
	 * so hand back the replayed page (which carries the source's
	 * contents) instead of installing a fresh zeroed one -- this is
	 * what makes a *migrated* VF's cmd ring / FW pages / EQ buffers
	 * functional after LOAD. The size must match the source's
	 * allocation at this slot position; a mismatch is a determinism
	 * break, not a page we can safely hand back.
	 */
	p = vfmig_iova_find_locked(dom, iova);
	if (p) {
		if (p->len != aligned) {
			dev_warn(&dom->vf_pdev->dev,
				 "vfmig_iova: vf %u slot %u replay/alloc size mismatch at IOVA 0x%llx: replayed %zu, requested %zu\n",
				 dom->vf_id, slot, iova, p->len, aligned);
			err = -EINVAL;
			goto out_unlock;
		}
		p->slot		= slot;
		p->instance_key	= instance_key;
		dom->cursor[slot] = iova + aligned;
		*iova_out  = p->iova;
		*vaddr_out = p->vaddr;
		err = 0;
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

int vfmig_iova_replay_page(struct vfmig_iova_domain *dom,
			   enum vfmig_iova_slot slot, u64 instance_key,
			   dma_addr_t iova, const void *contents, size_t len)
{
	struct vfmig_iova_page *p;
	enum vfmig_iova_slot iova_slot;
	int err;

	if (!dom || !contents)
		return -EINVAL;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u replay: slot %u out of range\n",
			 dom->vf_id, slot);
		return -EINVAL;
	}

	/*
	 * Cross-check that the wire-claimed slot agrees with the slot the
	 * destination's own IOVA partitioning assigns to @iova. A mismatch
	 * means source and destination disagree about the slot layout
	 * (wire-incompatible CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB / slot set):
	 * the determinism guarantee is broken, so refuse rather than
	 * install at an unexpected slot.
	 */
	iova_slot = vfmig_iova_slot_from_iova(dom, (u64)iova);
	if (iova_slot != slot) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u replay: wire claims slot %u for IOVA 0x%llx but destination maps it to slot %u\n",
			 dom->vf_id, slot, (u64)iova, iova_slot);
		return -ERANGE;
	}

	mutex_lock(&dom->lock);

	err = vfmig_iova_install_page_locked(dom, slot, instance_key,
					     (u64)iova, len, GFP_KERNEL, &p);
	if (err)
		goto out_unlock;

	memcpy(p->vaddr, contents, len);

	/*
	 * Push the slot cursor past the highest replayed IOVA so a later
	 * vfmig_iova_reset_cursor() rewinds to the slot base, and so that
	 * absent a reset fresh allocs still don't collide with replays.
	 */
	if ((u64)iova + len > dom->cursor[slot])
		dom->cursor[slot] = (u64)iova + len;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom)
{
	unsigned int s;

	if (!dom)
		return;
	mutex_lock(&dom->lock);
	for (s = 0; s < VFMIG_IOVA_NR_SLOTS; s++) {
		dom->cursor[s] = vfmig_iova_slot_base(dom,
						      (enum vfmig_iova_slot)s);
		dom->next_auto_key[s] = 0;
	}
	mutex_unlock(&dom->lock);
}

int vfmig_iova_for_each(struct vfmig_iova_domain *dom,
			vfmig_iova_for_each_fn cb, void *ctx)
{
	struct vfmig_iova_page *p;
	int ret = 0;

	if (!dom || !cb)
		return -EINVAL;

	mutex_lock(&dom->lock);
	list_for_each_entry(p, &dom->pages, node) {
		ret = cb(p->slot, p->instance_key, p->iova, p->vaddr,
			 p->len, ctx);
		if (ret)
			break;
	}
	mutex_unlock(&dom->lock);
	return ret;
}

int vfmig_iova_transient_get(struct vfmig_iova_domain *dom,
			     size_t size, gfp_t gfp,
			     void **vaddr_out, dma_addr_t *iova_out)
{
	struct vfmig_transient_arena *a;
	struct vfmig_transient_page *tp;
	int err;

	if (!dom || !vaddr_out || !iova_out || size == 0)
		return -EINVAL;

	if (size > PAGE_SIZE) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: transient_get(size=%zu) > PAGE_SIZE not supported\n",
				     size);
		return -EINVAL;
	}

	if (gfp & (__GFP_COMP | __GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM)) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: transient_get: rejected gfp 0x%x (must not include __GFP_HIGHMEM/COMP/DMA/DMA32)\n",
				     gfp);
		return -EINVAL;
	}

	a = &dom->transient;
	mutex_lock(&dom->lock);

	tp = list_first_entry_or_null(&a->free,
				      struct vfmig_transient_page, free_node);
	if (tp) {
		/*
		 * list_del_init() so list_empty(&tp->free_node) is true
		 * while @tp is out with the caller; _put() uses that for
		 * double-free detection.
		 */
		list_del_init(&tp->free_node);
		a->n_free--;
	} else {
		tp = vfmig_transient_grow_locked(dom, gfp);
		if (IS_ERR(tp)) {
			err = PTR_ERR(tp);
			mutex_unlock(&dom->lock);
			return err;
		}
	}

	*iova_out  = tp->iova;
	*vaddr_out = tp->vaddr;
	mutex_unlock(&dom->lock);
	return 0;
}

void vfmig_iova_transient_put(struct vfmig_iova_domain *dom,
			      dma_addr_t iova, size_t size)
{
	struct vfmig_transient_arena *a;
	struct vfmig_transient_page *tp;
	unsigned int idx;

	if (!dom)
		return;

	a = &dom->transient;
	if ((u64)iova < a->base || (u64)iova >= a->end ||
	    !IS_ALIGNED((u64)iova, PAGE_SIZE)) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: transient_put: IOVA 0x%llx outside arena [0x%llx, 0x%llx) or unaligned\n",
			 (u64)iova, a->base, a->end);
		return;
	}
	if (size > PAGE_SIZE) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: transient_put: size=%zu > PAGE_SIZE\n",
			 size);
		return;
	}

	idx = ((u64)iova - a->base) >> PAGE_SHIFT;

	mutex_lock(&dom->lock);
	tp = a->slots[idx];
	if (!tp) {
		mutex_unlock(&dom->lock);
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: transient_put: IOVA 0x%llx never allocated\n",
			 (u64)iova);
		return;
	}
	if (WARN_ON_ONCE(tp->iova != (u64)iova)) {
		mutex_unlock(&dom->lock);
		return;
	}
	if (!list_empty(&tp->free_node)) {
		mutex_unlock(&dom->lock);
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: transient_put: double-free of IOVA 0x%llx\n",
			 (u64)iova);
		return;
	}
	list_add(&tp->free_node, &a->free);
	a->n_free++;
	mutex_unlock(&dom->lock);
}
