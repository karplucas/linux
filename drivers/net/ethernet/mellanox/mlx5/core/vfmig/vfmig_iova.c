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

#include "vfmig_dma_ops.h"
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

	/*
	 * @external: the backing page is owned by the caller, not by the
	 * registry. Set on entries created by the
	 * vfmig_iova_user_page_map_phys() path (umem-pinned MR / CQ / QP /
	 * SRQ buffers + doorbell records flowing through vfmig_dma_ops).
	 * For these entries the registry tracks the (iova, len) bookkeeping
	 * and owns the iommu_map slot, but @page and @vaddr are NULL: it
	 * does not alloc_pages() at install nor __free_pages() at destroy,
	 * and vfmig_iova_for_each() skips them (its callback dereferences
	 * @vaddr, which is meaningless here).
	 */
	bool		 external;

	/*
	 * @awaiting_bind: LOAD/destination-side placeholder flag. Set on
	 * external entries pre-installed by vfmig_iova_replay_external()
	 * from a HOST_USER_PAGE wire record, before any destination-side
	 * umem has been pinned: the (iova, len) window is reserved and the
	 * (kind, fw_id) identity recorded in @instance_key, but there is no
	 * iommu_map yet (@page / @vaddr stay NULL, as for any external
	 * entry). A later slice's restore path consumes the placeholder,
	 * maps the freshly-pinned umem at @iova, and clears this flag. Only
	 * ever true when @external is; always false on a fresh / SAVE-side
	 * external entry.
	 */
	bool		 awaiting_bind;
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

	/*
	 * Host-page replay accounting (LOAD/destination side).
	 *
	 * @expected_count[s] is the number of HOST_PAGE records replayed
	 * into slot s by vfmig_iova_replay_page(); it records the source
	 * VF's footprint in that slot at SAVE time.
	 *
	 * @drift_armed is flipped once by vfmig_iova_arm_drift_detection()
	 * after a LOAD has replayed every promised record. While armed,
	 * alloc_slot() diagnoses (logs) a mismatch between the caller's
	 * resolved instance_key and the replayed entry it re-claims, so a
	 * destination-side allocation-sequence drift from the source is
	 * visible in dmesg. Both are zero on a fresh / SET_TRACKED-but-not-
	 * LOADed domain, where alloc_slot behaves exactly as before slice 8.
	 */
	u32		     expected_count[VFMIG_IOVA_NR_SLOTS];
	bool		     drift_armed;

	struct vfmig_transient_arena transient;
};

static inline u64
vfmig_iova_slot_base(const struct vfmig_iova_domain *dom,
		     enum vfmig_iova_slot slot)
{
	return dom->base + (u64)slot * VFMIG_IOVA_SLOT_BYTES;
}

/*
 * USER_PAGE's effective starting IOVA after the kcoherent carve. The
 * bottom VFMIG_IOVA_KCOHERENT_BYTES of slot USER_PAGE's window are
 * reserved for the (future) non-migrated kcoherent sub-arena, so the
 * user-MR IOVA range proper begins here. Range checks and cursor
 * initialization for USER_PAGE MUST use this rather than
 * vfmig_iova_slot_base(dom, VFMIG_SLOT_USER_PAGE) directly.
 */
static inline u64
vfmig_iova_user_page_start(const struct vfmig_iova_domain *dom)
{
	return vfmig_iova_slot_base(dom, VFMIG_SLOT_USER_PAGE) +
	       VFMIG_IOVA_KCOHERENT_BYTES;
}

/*
 * End (exclusive) of @slot's window. Kernel slots are uniform 510-MiB
 * windows; VFMIG_SLOT_USER_PAGE is expand-to-fill and ends at the
 * transient arena's base (set by vfmig_iova_domain_create()).
 */
static inline u64
vfmig_iova_slot_end(const struct vfmig_iova_domain *dom,
		    enum vfmig_iova_slot slot)
{
	if (slot == VFMIG_SLOT_USER_PAGE)
		return dom->transient.base;
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
	if (idx <= VFMIG_SLOT_INVALID)
		return VFMIG_SLOT_INVALID;
	/*
	 * Asymmetric layout: everything at or above slot_base(USER_PAGE)
	 * is either the reserved kcoherent carve (not a deterministic slot
	 * -> INVALID, so replay can never install there) or the USER_PAGE
	 * expand-to-fill window.
	 */
	if (idx >= VFMIG_SLOT_USER_PAGE) {
		if (iova < vfmig_iova_user_page_start(dom))
			return VFMIG_SLOT_INVALID;	/* kcoherent range */
		return VFMIG_SLOT_USER_PAGE;
	}
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
 * dom->lock held. Install one external (caller-owned-page) registry
 * entry at @iova, mapping @phys for @len bytes. Used by the
 * vfmig_dma_ops shim to plumb umem-pinned pages through the per-VF
 * iommu_domain.
 *
 * Differs from vfmig_iova_install_page_locked in that it does not
 * alloc_pages(): the caller supplies @phys directly (from sg_phys()).
 * The entry is flagged @external = true so destroy_page_locked skips
 * __free_pages, and @page / @vaddr are left NULL (the registry has no
 * kernel-virtual handle to caller-owned memory). Same window/alignment
 * validation, same -EEXIST on duplicate IOVA, same -ERANGE on
 * slot/IOVA mismatch.
 */
static int
vfmig_iova_install_external_phys_locked(struct vfmig_iova_domain *dom,
					enum vfmig_iova_slot slot,
					u64 instance_key, u64 iova,
					phys_addr_t phys, size_t len,
					gfp_t gfp,
					struct vfmig_iova_page **out_p)
{
	struct vfmig_iova_page *p;
	int err;

	if (!IS_ALIGNED(iova, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(len, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(phys, VFMIG_IOVA_GRANULE) ||
	    len == 0)
		return -EINVAL;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR)
		return -EINVAL;
	{
		u64 lo = (slot == VFMIG_SLOT_USER_PAGE)
			? vfmig_iova_user_page_start(dom)
			: vfmig_iova_slot_base(dom, slot);
		if (iova < lo || iova + len > vfmig_iova_slot_end(dom, slot))
			return -ERANGE;
	}
	if (vfmig_iova_find_locked(dom, iova))
		return -EEXIST;

	p = kzalloc(sizeof(*p), gfp);
	if (!p)
		return -ENOMEM;

	p->page		= NULL;
	p->vaddr	= NULL;
	p->iova		= iova;
	p->len		= len;
	p->slot		= slot;
	p->instance_key	= instance_key;
	p->external	= true;

	err = iommu_map(dom->iommu_dom, iova, phys, len,
			IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, gfp);
	if (err) {
		kfree(p);
		return err;
	}

	vfmig_iova_insert_locked(dom, p);
	*out_p = p;
	return 0;
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
	u64 base;
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
	 * USER_PAGE is expand-to-fill with a kcoherent carve at its base,
	 * so its cursor starts past the carve, not at slot_base.
	 */
	dom->cursor[VFMIG_SLOT_USER_PAGE] = vfmig_iova_user_page_start(dom);

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

	/*
	 * Install the per-VF dma_map_ops shim now that the unmanaged
	 * domain is attached. From here the DMA API on this VF dispatches
	 * through vfmig_dma_ops instead of the (now-detached) default
	 * dma-iommu path. Must be undone before iommu_detach_device on
	 * every teardown path.
	 */
	err = vfmig_dma_ops_attach(vf_pdev, dom);
	if (err) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: dma_ops attach failed: %d\n", err);
		goto err_pci_put;
	}

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u domain attached: kernel slots [0x%llx, 0x%llx) (%u x 0x%llx) + kcoherent carve 0x%llx + USER_PAGE [0x%llx, 0x%llx) + transient [0x%llx, 0x%llx) within IOMMU aperture [0x%llx, 0x%llx]\n",
		 vf_id, dom->base,
		 vfmig_iova_slot_base(dom, VFMIG_SLOT_USER_PAGE),
		 VFMIG_IOVA_KERNEL_NR_SLOTS, (u64)VFMIG_IOVA_SLOT_BYTES,
		 (u64)VFMIG_IOVA_KCOHERENT_BYTES,
		 vfmig_iova_user_page_start(dom), dom->transient.base,
		 dom->transient.base, dom->transient.end,
		 dom->iommu_dom->geometry.aperture_start,
		 dom->iommu_dom->geometry.aperture_end);

	*out = dom;
	return 0;

err_pci_put:
	pci_dev_put(dom->vf_pdev);
	dom->vf_pdev = NULL;
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

	/* Uninstall the shim before detaching the domain (reverse of create). */
	vfmig_dma_ops_detach(vf_pdev);
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
			vfmig_dma_ops_detach(vf_pdev);
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

int vfmig_iova_kcoherent_window(struct vfmig_iova_domain *dom,
				struct iommu_domain **iommu_dom,
				u64 *base, u64 *len)
{
	if (!dom || !iommu_dom || !base || !len)
		return -EINVAL;

	*iommu_dom = dom->iommu_dom;
	*base = vfmig_iova_slot_base(dom, VFMIG_SLOT_USER_PAGE);
	*len = VFMIG_IOVA_KCOHERENT_BYTES;
	return 0;
}

int vfmig_iova_alloc_slot(struct vfmig_iova_domain *dom,
			  enum vfmig_iova_slot slot, u64 instance_key,
			  size_t size, gfp_t gfp,
			  dma_addr_t *iova_out, void **vaddr_out)
{
	struct vfmig_iova_page *p;
	size_t aligned;
	u64 iova, slot_end;
	u64 caller_key;
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
	 * as-is and don't bump the counter. @caller_key remembers the
	 * pre-resolution value so the drift diagnostic can tell a 0/auto
	 * caller from a pinned one.
	 */
	caller_key = instance_key;
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
		/*
		 * Diagnostic only: once drift detection is armed (LOAD
		 * finished replaying), a resolved-key mismatch means the
		 * destination's pinned/auto allocation sequence in this
		 * slot diverged from the source's. Log it but still
		 * re-claim the page -- strict -EPROTO enforcement (and the
		 * kcoherent fallback for legitimate post-restore growth)
		 * arrives with the USER_PAGE work.
		 */
		if (dom->drift_armed && p->instance_key != instance_key)
			dev_warn_ratelimited(&dom->vf_pdev->dev,
				"vfmig_iova: vf %u slot %u DRIFT: caller key %s (resolved 0x%llx) but replayed entry at IOVA 0x%llx carries key 0x%llx\n",
				dom->vf_id, slot,
				caller_key == 0 ? "0/auto" : "pinned",
				(unsigned long long)instance_key,
				(unsigned long long)iova,
				(unsigned long long)p->instance_key);
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

int vfmig_iova_user_page_map_phys(struct vfmig_iova_domain *dom,
				  phys_addr_t phys, size_t len, gfp_t gfp,
				  dma_addr_t *iova_out)
{
	struct vfmig_iova_page *p;
	size_t aligned;
	u64 iova, slot_end;
	int err;

	if (!dom || !iova_out || len == 0)
		return -EINVAL;
	if (!IS_ALIGNED(phys, VFMIG_IOVA_GRANULE))
		return -EINVAL;

	aligned = ALIGN(len, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);

	iova = dom->cursor[VFMIG_SLOT_USER_PAGE];
	slot_end = vfmig_iova_slot_end(dom, VFMIG_SLOT_USER_PAGE);
	if (iova + aligned > slot_end) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE slot exhausted at cursor 0x%llx (slot_end 0x%llx, asked %zu)\n",
				     dom->vf_id, iova, slot_end, aligned);
		err = -ENOSPC;
		goto out_unlock;
	}

	/*
	 * The cursor tracks the next fresh USER_PAGE IOVA, so a registry
	 * hit here means another allocator (kernel slot, transient, or a
	 * leaked prior map) already sits on it -- a kernel bug. Fail
	 * cleanly rather than silently overwrite. (Stage 2's LOAD-side
	 * placeholder replay will introduce a legitimate hit path here.)
	 */
	p = vfmig_iova_find_locked(dom, iova);
	if (p) {
		dev_err_ratelimited(&dom->vf_pdev->dev,
				    "vfmig_iova: vf %u USER_PAGE cursor 0x%llx already mapped (slot %u key 0x%llx len %zu); refusing to overwrite\n",
				    dom->vf_id, iova, p->slot,
				    (unsigned long long)p->instance_key,
				    p->len);
		err = -EEXIST;
		goto out_unlock;
	}

	err = vfmig_iova_install_external_phys_locked(dom,
						      VFMIG_SLOT_USER_PAGE,
						      ++dom->next_auto_key[VFMIG_SLOT_USER_PAGE],
						      iova, phys, aligned, gfp, &p);
	if (err)
		goto out_unlock;

	dom->cursor[VFMIG_SLOT_USER_PAGE] = iova + aligned;
	*iova_out = iova;
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

int vfmig_iova_user_page_unmap_phys(struct vfmig_iova_domain *dom,
				    dma_addr_t iova, size_t len)
{
	struct vfmig_iova_page *p;
	size_t aligned;
	int err;

	if (!dom)
		return -EINVAL;

	aligned = ALIGN(len, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);

	p = vfmig_iova_find_locked(dom, (u64)iova);
	if (!p) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE unmap: no registry entry at IOVA 0x%llx (asked %zu)\n",
				     dom->vf_id, (u64)iova, aligned);
		err = -ENOENT;
		goto out_unlock;
	}
	if (p->slot != VFMIG_SLOT_USER_PAGE || !p->external) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE unmap: IOVA 0x%llx is slot %u external=%d (expected USER_PAGE external)\n",
				     dom->vf_id, (u64)iova, p->slot,
				     p->external);
		err = -EINVAL;
		goto out_unlock;
	}
	if (p->len != aligned)
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE unmap size mismatch at IOVA 0x%llx: have %zu, asked %zu\n",
				     dom->vf_id, (u64)iova, p->len, aligned);

	list_del(&p->node);
	dom->n_pages--;
	vfmig_iova_destroy_page_locked(dom, p);
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
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
	if (slot == VFMIG_SLOT_USER_PAGE) {
		/*
		 * USER_PAGE uses a separate replay path: its wire records
		 * (added in a later patch) carry identity only, not
		 * contents, and install as awaiting-bind placeholders. A
		 * HOST_PAGE record, which does carry contents, must never
		 * target the USER_PAGE slot.
		 */
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u replay_page: USER_PAGE slot is not content-replayable\n",
			 dom->vf_id);
		return -EOPNOTSUPP;
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

	/*
	 * Replay after arming is anomalous: the LOAD path arms exactly
	 * once, after every promised HOST_PAGE record has been replayed. A
	 * late replay would grow expected_count[] after the footprint was
	 * declared frozen. Refuse it.
	 */
	if (WARN_ON_ONCE(dom->drift_armed)) {
		err = -EBUSY;
		goto out_unlock;
	}

	err = vfmig_iova_install_page_locked(dom, slot, instance_key,
					     (u64)iova, len, GFP_KERNEL, &p);
	if (err)
		goto out_unlock;

	memcpy(p->vaddr, contents, len);
	dom->expected_count[slot]++;

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

/*
 * Install a LOAD-side awaiting-bind placeholder: an external USER_PAGE
 * registry entry that reserves the wire-provided [iova, iova+len) window
 * and records the (kind, fw_id) identity in @instance_key, but installs
 * no iommu_map (there is no umem to point at yet -- @page / @vaddr stay
 * NULL). @instance_key must already be retagged (kind byte != NONE); the
 * placeholder is what a later slice's restore path looks up and binds.
 * Same window/alignment/duplicate validation as the phys installer.
 * Caller holds @dom->lock.
 */
static int
vfmig_iova_install_external_placeholder_locked(struct vfmig_iova_domain *dom,
					       enum vfmig_iova_slot slot,
					       u64 instance_key, u64 iova,
					       size_t len, gfp_t gfp,
					       struct vfmig_iova_page **out_p)
{
	struct vfmig_iova_page *p;

	if (!IS_ALIGNED(iova, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(len, VFMIG_IOVA_GRANULE) ||
	    len == 0)
		return -EINVAL;
	if (slot != VFMIG_SLOT_USER_PAGE)
		return -EINVAL;
	if (VFMIG_HUOBJ_KIND(instance_key) == VFMIG_HUOBJ_KIND_NONE)
		return -EINVAL;
	if (iova < vfmig_iova_user_page_start(dom) ||
	    iova + len > vfmig_iova_slot_end(dom, slot))
		return -ERANGE;
	if (vfmig_iova_find_locked(dom, iova))
		return -EEXIST;

	p = kzalloc(sizeof(*p), gfp);
	if (!p)
		return -ENOMEM;

	p->page		 = NULL;
	p->vaddr	 = NULL;
	p->iova		 = iova;
	p->len		 = len;
	p->slot		 = slot;
	p->instance_key	 = instance_key;
	p->external	 = true;
	p->awaiting_bind = true;

	vfmig_iova_insert_locked(dom, p);
	*out_p = p;
	return 0;
}

/*
 * Replay one HOST_USER_PAGE record into @dom on the LOAD/destination
 * side: install an awaiting-bind placeholder at the wire-provided @iova
 * for the (kind, fw_id) packed into @instance_key. Unlike
 * vfmig_iova_replay_page() this carries no contents -- the umem pages are
 * the migration tool's to restore -- so it only reserves the IOVA window
 * and records the identity. Bumps the USER_PAGE expected-count and pushes
 * the slot cursor past the placeholder, mirroring replay_page. Must run
 * before the domain is drift-armed. Returns 0 or a negative errno.
 */
int vfmig_iova_replay_external(struct vfmig_iova_domain *dom,
			       enum vfmig_iova_slot slot, u64 instance_key,
			       dma_addr_t iova, size_t len)
{
	struct vfmig_iova_page *p;
	int err;

	if (!dom)
		return -EINVAL;

	mutex_lock(&dom->lock);

	if (WARN_ON_ONCE(dom->drift_armed)) {
		err = -EBUSY;
		goto out_unlock;
	}

	err = vfmig_iova_install_external_placeholder_locked(dom, slot,
							     instance_key,
							     (u64)iova, len,
							     GFP_KERNEL, &p);
	if (err)
		goto out_unlock;

	dom->expected_count[slot]++;
	if ((u64)iova + len > dom->cursor[slot])
		dom->cursor[slot] = (u64)iova + len;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

int vfmig_iova_retag_external_range(struct vfmig_iova_domain *dom,
				    dma_addr_t iova_base, size_t length,
				    u64 new_instance_key)
{
	u8 new_kind = VFMIG_HUOBJ_KIND(new_instance_key);
	struct vfmig_iova_page *p;
	u64 base = (u64)iova_base;
	unsigned int retagged = 0;
	int err = 0;
	u64 limit;

	if (!dom || length == 0)
		return -EINVAL;
	limit = base + length;
	if (!IS_ALIGNED(base, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(length, VFMIG_IOVA_GRANULE))
		return -EINVAL;
	if (new_kind == VFMIG_HUOBJ_KIND_NONE)
		return -EINVAL;

	mutex_lock(&dom->lock);

	/*
	 * dom->pages is sorted by IOVA, so a single forward walk covers
	 * every entry overlapping [base, limit). A multi-page user object
	 * (umem spanning >1 page) yields multiple entries in the range,
	 * one per source-side sg, all retagged with the same key.
	 */
	list_for_each_entry(p, &dom->pages, node) {
		u64 p_end = p->iova + p->len;

		if (p->iova >= limit)
			break;		/* sorted: past the range */
		if (p_end <= base)
			continue;	/* before the range */
		if (!p->external)
			continue;	/* kernel slot; skip silently */

		if (p->instance_key == new_instance_key) {
			retagged++;	/* idempotent re-retag: no-op */
			continue;
		}
		if (VFMIG_HUOBJ_KIND(p->instance_key) !=
		    VFMIG_HUOBJ_KIND_NONE) {
			/*
			 * Already claimed by a different (kind, fw_id):
			 * overlapping umem ranges from two uobject
			 * creations. Source-side bug; roll back below.
			 */
			err = -EEXIST;
			break;
		}
		p->instance_key = new_instance_key;
		retagged++;
	}

	if (err) {
		/*
		 * Revert every entry we retagged in this call -- the only
		 * entries in [base, limit) whose key now equals
		 * @new_instance_key -- back to the auto-numbered sentinel.
		 */
		list_for_each_entry(p, &dom->pages, node) {
			u64 p_end = p->iova + p->len;

			if (p->iova >= limit)
				break;
			if (p_end <= base)
				continue;
			if (!p->external)
				continue;
			if (p->instance_key != new_instance_key)
				continue;
			p->instance_key = 0;
		}
		goto out_unlock;
	}

	if (retagged == 0) {
		/*
		 * No external entries on the range: the umem was mapped
		 * through a non-vfmig DMA path, or the callsite ran before
		 * the dma_map that plants the entries.
		 */
		err = -ENOENT;
	}

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom)
{
	unsigned int s;
	u64 user_page_hwm;

	if (!dom)
		return;
	mutex_lock(&dom->lock);
	/*
	 * Snapshot the USER_PAGE cursor BEFORE the per-slot reset.
	 * Stage-2 replay (vfmig_iova_replay_external) bumps this cursor
	 * monotonically to (highest_replayed_iova + length) so fresh
	 * post-restore user_page_map_phys() calls land above source-side
	 * replayed placeholders. The loop below would otherwise clobber
	 * that high-water mark with slot_base, and the post-loop adjust
	 * only lifts to user_page_start -- the bottom of the replay
	 * region, where the lowest-IOVA placeholder lives. The first
	 * fresh post-restore user_page allocation would then collide at
	 * user_page_start with the replayed placeholder there and return
	 * -EEXIST. max_t() with user_page_start preserves the replayed
	 * high-water mark on LOAD probe arcs and still ratchets up to
	 * user_page_start on first-time / non-replay arcs (where the
	 * pre-loop value is slot_base, below user_page_start).
	 */
	user_page_hwm = dom->cursor[VFMIG_SLOT_USER_PAGE];

	for (s = 0; s < VFMIG_IOVA_NR_SLOTS; s++) {
		dom->cursor[s] = vfmig_iova_slot_base(dom,
						      (enum vfmig_iova_slot)s);
		dom->next_auto_key[s] = 0;
	}
	/* USER_PAGE floors at user_page_start but preserves the HWM. */
	dom->cursor[VFMIG_SLOT_USER_PAGE] =
		max_t(u64, user_page_hwm, vfmig_iova_user_page_start(dom));
	mutex_unlock(&dom->lock);
}

void vfmig_iova_arm_drift_detection(struct vfmig_iova_domain *dom)
{
	unsigned int s;
	u32 total = 0;

	if (!dom)
		return;
	mutex_lock(&dom->lock);
	if (!dom->drift_armed) {
		dom->drift_armed = true;
		for (s = 0; s < VFMIG_IOVA_NR_SLOTS; s++)
			total += dom->expected_count[s];
		dev_info(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u drift detection armed (replays: cmd_ring=%u fw_page=%u eq_buf=%u frag_buf=%u db_page=%u dma_coherent=%u, total=%u)\n",
			 dom->vf_id,
			 dom->expected_count[VFMIG_SLOT_CMD_RING],
			 dom->expected_count[VFMIG_SLOT_FW_PAGE],
			 dom->expected_count[VFMIG_SLOT_EQ_BUF],
			 dom->expected_count[VFMIG_SLOT_FRAG_BUF],
			 dom->expected_count[VFMIG_SLOT_DB_PAGE],
			 dom->expected_count[VFMIG_SLOT_DMA_COHERENT],
			 total);
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
		/*
		 * External (USER_PAGE, caller-owned) entries have no
		 * kernel-virtual @vaddr for the callback to read; SAVE's
		 * HOST_PAGE memcpy would dereference NULL. They are emitted
		 * separately via the external-iteration path, so skip them
		 * here.
		 */
		if (p->external)
			continue;
		ret = cb(p->slot, p->instance_key, p->iova, p->vaddr,
			 p->len, ctx);
		if (ret)
			break;
	}
	mutex_unlock(&dom->lock);
	return ret;
}

int vfmig_iova_for_each_external(struct vfmig_iova_domain *dom,
				 vfmig_iova_for_each_external_fn cb,
				 void *ctx)
{
	struct vfmig_iova_page *p;
	int ret = 0;

	if (!dom || !cb)
		return -EINVAL;

	mutex_lock(&dom->lock);
	list_for_each_entry(p, &dom->pages, node) {
		if (!p->external)
			continue;
		ret = cb(VFMIG_HUOBJ_KIND(p->instance_key),
			 VFMIG_HUOBJ_FWID(p->instance_key),
			 p->iova, p->len, ctx);
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
