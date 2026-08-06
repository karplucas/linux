/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * Per-VF unmanaged IOVA domain + deterministic slot allocator for
 * mlx5 host-driven VF migration.
 *
 * Why
 * ---
 * SAVE_VHCA_STATE captures, inside the firmware blob, the source
 * VHCA's own IOVAs (cmd ring, host pages, EQ/UAR buffers). On native
 * (non-VFIO, non-VM) mlx5_core the destination's dma-iommu layer hands
 * out fresh, unrelated IOVAs, so after LOAD the firmware would
 * dereference addresses that point nowhere. The fix, built in layers:
 *
 *   1. Give each migratable VF an unmanaged paging iommu_domain that
 *      the PF driver fully owns (this file's domain lifecycle).
 *   2. Lay out a fixed per-VF IOVA window at a high, well-known base so
 *      the source's IOVAs are reproducible on the destination.
 *   3. Provide a slot-tagged allocator (vfmig_iova_alloc_slot) that the
 *      mlx5_core probe path uses instead of dma_alloc_coherent for
 *      every DMA buffer the firmware records an IOVA for. Each caller
 *      declares which kind of allocation it is (CMD_RING, FW_PAGE, ...)
 *      and gets a deterministic IOVA from that slot's dedicated
 *      sub-window (routing of the individual call sites lands in later
 *      patches).
 *
 * Determinism contract
 * --------------------
 * Every converted call site declares which slot its allocation belongs
 * to (enum vfmig_iova_slot). Each slot owns its own VFMIG_IOVA_SLOT_BYTES
 * sub-window and its own bump cursor, so adding, removing, or reordering
 * allocations in one slot cannot shift IOVAs in another -- the per-slot
 * windows are a fixed partition. Within a slot, source/destination IOVA
 * equivalence rests on call-site discipline: the same set of allocations
 * of the same sizes in the same order. The instance_key argument lets a
 * caller pin a specific (slot, key) -> IOVA mapping when it has a stable
 * identifier; passing 0 falls back to per-slot auto-numbering.
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
 * Slot identity. Each value names one category of converted allocation;
 * the (slot, instance_key) pair plus size identifies a specific
 * allocation within that category.
 *
 * Slots are introduced one at a time, together with the call site that
 * routes through them (see the layered restore plan). Always append new
 * values before VFMIG_SLOT_NR and never renumber existing enumerators --
 * the numeric value is the slot's IOVA base offset within the per-VF
 * window, and stable IOVAs are the point of this subsystem.
 *
 *   VFMIG_SLOT_INVALID  -- sentinel; alloc_slot rejects it.
 *   VFMIG_SLOT_CMD_RING -- cmd ring DMA buffer (one per VF), allocated
 *                          by mlx5_cmd_enable during probe.
 *   VFMIG_SLOT_FW_PAGE  -- FW-owned pages handed over via MANAGE_PAGES
 *                          OP_GIVE (boot/init/dynamic), one allocation
 *                          per page (see alloc_system_page).
 *   VFMIG_SLOT_EQ_BUF   -- EQ frag buffers (async/cmd/comp EQs). Kept
 *                          separate so adding or removing an EQ on the
 *                          destination cannot shift the IOVAs of other
 *                          consumers (see create_map_eq / eq.c).
 *   VFMIG_SLOT_FRAG_BUF -- work-queue frag buffers (RQ/SQ/CQ rings),
 *                          allocated by the mlx5_wq_*_create helpers
 *                          (wq.c).
 *   VFMIG_SLOT_DB_PAGE  -- doorbell pgdir pages (mlx5_db_alloc_node /
 *                          mlx5_alloc_db_pgdir, alloc.c).
 *   VFMIG_SLOT_DMA_COHERENT -- catch-all for coherent host buffers not
 *                          routed to a per-purpose slot: the exported
 *                          mlx5_frag_buf_alloc_node() ABI used by
 *                          mlx5_ib / vfio_pci_mlx5 / vdpa. With this
 *                          slot every coherent allocation on a tracked
 *                          VF's probe path routes through the allocator,
 *                          so a fresh tracked VF can bind.
 */
enum vfmig_iova_slot {
	VFMIG_SLOT_INVALID	= 0,
	VFMIG_SLOT_CMD_RING	= 1,
	VFMIG_SLOT_FW_PAGE	= 2,
	VFMIG_SLOT_EQ_BUF	= 3,
	VFMIG_SLOT_FRAG_BUF	= 4,
	VFMIG_SLOT_DB_PAGE	= 5,
	VFMIG_SLOT_DMA_COHERENT	= 6,
	VFMIG_SLOT_NR,		/* count; drives VFMIG_IOVA_NR_SLOTS */
};

/*
 * IOVA window layout (hardware-visible addresses).
 *
 *   VFMIG_IOVA_BASE     -- per-PF base, 4 GiB. The window
 *                          [BASE, BASE + N * PER_VF) must fit entirely
 *                          inside the IOMMU's geometry aperture, whose
 *                          upper bound the iommu driver derives from the
 *                          hardware address width (e.g. 39-bit on some
 *                          Intel VT-d -> [0, 0x7fffffffff]). iommu_map()
 *                          returns -ERANGE outside it, so the window is
 *                          validated against the live aperture at attach
 *                          (vfmig_iova_domain_create()) and SET_TRACKED
 *                          fails with -EOPNOTSUPP + a printed diagnostic
 *                          rather than a later opaque cmd-ring DMA
 *                          failure. Base 4 GiB leaves the bottom 4 GiB
 *                          untouched and makes IOVAs visually distinct
 *                          from anything the default allocator produces.
 *                          VFs of the same PF get distinct sub-windows
 *                          (BASE + vf_id * PER_VF) so an IOVA value alone
 *                          identifies the owning VF in dmesg.
 *   VFMIG_IOVA_PER_VF   -- IOVA space per VF, configurable via
 *                          CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB (GiB;
 *                          default 4). Ample for the cmd ring +
 *                          MANAGE_PAGES host pages + EQ/UAR buffers a
 *                          later layer maps.
 *   VFMIG_IOVA_GRANULE  -- minimum allocation alignment; matches
 *                          PAGE_SIZE (also the mlx5 hardware page size).
 */

#if IS_ENABLED(CONFIG_MLX5_VFMIG)

#define VFMIG_IOVA_BASE		0x100000000ULL		/* 4 GiB */
#define VFMIG_IOVA_PER_VF \
	((u64)CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB << 30)
#define VFMIG_IOVA_GRANULE	PAGE_SIZE

/*
 * The deterministic per-VF range is partitioned across
 * VFMIG_IOVA_NR_SLOTS fixed-size slot windows, each
 * VFMIG_IOVA_SLOT_BYTES wide. Slot N occupies
 *   [base + N * SLOT_BYTES, base + (N + 1) * SLOT_BYTES).
 * Slot 0 (VFMIG_SLOT_INVALID) is reserved and never allocated from.
 * NR_SLOTS tracks the enum, so appending a slot enumerator grows the
 * deterministic window (and the per-slot cursor arrays) by one slot;
 * existing slots keep their IOVA base offset.
 *
 * The 510 MiB slot size is pinned (not scaled with PER_VF): the kernel
 * call-site footprint is bounded by hardware capabilities, not by how
 * much IOVA the admin hands us, and pinning it keeps a SAVE blob's
 * kernel-slot IOVAs PER_VF-independent.
 */
#define VFMIG_IOVA_NR_SLOTS	((unsigned int)VFMIG_SLOT_NR)
#define VFMIG_IOVA_SLOT_BYTES	(510ULL << 20)	/* 510 MiB, fixed */

/*
 * Transient sub-window: the topmost slice of each VF's IOVA window,
 * reserved for vfmig_iova_transient_get/put (short-lived, freelist-
 * recycled, single-page allocations -- cmd mailbox blocks). Sized to
 * hold the worst-case cmd-mailbox-cache footprint (~3900 pages); 16 MiB
 * == 4096 pages leaves headroom. It sits above the deterministic slot
 * range and must not overlap it (see static_assert below).
 */
#define VFMIG_IOVA_TRANSIENT_BYTES	(16ULL << 20)	/* 16 MiB */

static_assert(VFMIG_IOVA_PER_VF >
	      (u64)VFMIG_IOVA_NR_SLOTS * VFMIG_IOVA_SLOT_BYTES +
	      VFMIG_IOVA_TRANSIENT_BYTES,
	      "CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB too small: must fit all fixed 510-MiB slots + the transient arena");
static_assert(VFMIG_IOVA_SLOT_BYTES >= (8ULL << 20),
	      "VFMIG_IOVA_SLOT_BYTES must be >= 8 MiB to host worst-case kernel allocations");

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
 * Detach @dom's iommu_dom from its VF's PCI device, keeping the domain
 * struct alive for a later vfmig_iova_domain_destroy(). Must run while
 * the VF's struct device still exists (before pci_disable_sriov fires
 * device_del) to avoid the iommu core's empty-group WARN. Idempotent;
 * safe with @dom == NULL. See the implementation for the teardown
 * ordering contract.
 */
void vfmig_iova_domain_detach_dev(struct vfmig_iova_domain *dom);

/*
 * Like vfmig_iova_domain_detach_dev(), but only acts when @dom's VF is
 * not currently driver-bound. Bound VFs detach their own domain from
 * mlx5_core's remove_one() tail (after the cmd ring + EQs are drained);
 * this covers tracked VFs that were never bound and thus have no
 * remove_one() to run that hook. Safe with @dom == NULL.
 */
void vfmig_iova_domain_detach_dev_if_unbound(struct vfmig_iova_domain *dom);

/*
 * Detach @dom from its VF, unmap + free every registry page, free the
 * iommu_domain, and drop the pinned VF reference. The VF must be
 * unbound. Safe with @dom == NULL.
 */
void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom);

/*
 * Allocate @size bytes of DMA-able memory from @slot's sub-window at the
 * slot's next deterministic IOVA. Backing pages are kernel-owned and
 * zeroed; the mapping is installed in the per-VF iommu_domain with
 * IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE.
 *
 * @instance_key: caller-pinned per-slot identity, or 0 for per-slot
 *		  auto-numbering.
 * @gfp:	  must not include __GFP_HIGHMEM/COMP/DMA/DMA32 (the
 *		  backing page must be lowmem and iommu_map-compatible).
 * @iova_out:	  the deterministic IOVA the firmware will see.
 * @vaddr_out:	  the kernel-virtual base of the backing memory.
 *
 * Returns 0 on success, -ENOSPC if @slot's window is exhausted,
 * -EINVAL on a bad slot / argument, or a negative errno from the page
 * allocator / iommu core.
 */
int  vfmig_iova_alloc_slot(struct vfmig_iova_domain *dom,
			   enum vfmig_iova_slot slot, u64 instance_key,
			   size_t size, gfp_t gfp,
			   dma_addr_t *iova_out, void **vaddr_out);

/*
 * Free a mapping previously returned by vfmig_iova_alloc_slot(). @slot,
 * @iova and @size must match the alloc. Safe with @dom == NULL; logs a
 * warning on an unknown IOVA or size mismatch.
 */
void vfmig_iova_free_slot(struct vfmig_iova_domain *dom,
			  enum vfmig_iova_slot slot,
			  dma_addr_t iova, size_t size);

/*
 * Allocate a single transient page from @dom's transient arena (the top
 * of the per-VF window). Transient allocations are short-lived, single
 * page (@size must be <= PAGE_SIZE), freelist-recycled, and -- unlike
 * slot allocations -- do NOT have a deterministic IOVA: they never
 * appear in a SAVE blob. Intended for the cmd mailbox block cache.
 *
 * @gfp:	must not include __GFP_HIGHMEM/COMP/DMA/DMA32.
 * @vaddr_out:	kernel-virtual base of the page.
 * @iova_out:	IOVA the firmware will see.
 *
 * Returns 0 on success, -ENOMEM if the arena is exhausted, -EINVAL on a
 * bad argument, or a negative errno from the page allocator / iommu core.
 */
int  vfmig_iova_transient_get(struct vfmig_iova_domain *dom,
			      size_t size, gfp_t gfp,
			      void **vaddr_out, dma_addr_t *iova_out);

/*
 * Return a page previously handed out by vfmig_iova_transient_get() to
 * the arena freelist (the mapping stays installed for reuse). @iova must
 * be an arena IOVA and @size <= PAGE_SIZE. Safe with @dom == NULL; logs
 * a warning on an out-of-range IOVA or a double free.
 */
void vfmig_iova_transient_put(struct vfmig_iova_domain *dom,
			      dma_addr_t iova, size_t size);

/*
 * Replay one HOST_PAGE record into @dom on the LOAD/destination side:
 * install a backing page at the wire-provided deterministic @iova in
 * @slot and memcpy @len bytes of @contents into it. Must run before the
 * destination VF probes (so its allocator re-claims the replayed pages
 * with the source's contents). @iova must fall inside @slot's window
 * (cross-checked against the destination's own partitioning). Returns 0,
 * or a negative errno (-ERANGE on a slot/IOVA mismatch, -EEXIST on a
 * duplicate IOVA, etc).
 */
int  vfmig_iova_replay_page(struct vfmig_iova_domain *dom,
			    enum vfmig_iova_slot slot, u64 instance_key,
			    dma_addr_t iova, const void *contents, size_t len);

/*
 * Rewind every per-slot bump cursor to its slot base (and per-slot
 * auto-key counters to 0) so the destination VF's probe re-claims the
 * replayed pages from the bottom of each slot in the same order the
 * source allocated them. Called once at LOAD-fd release, after all
 * HOST_PAGE records have been replayed. Safe with @dom == NULL.
 */
void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom);

#else /* !CONFIG_MLX5_VFMIG */

/*
 * Stubs for the not-config-gated call sites (e.g. cmd.c). The tracked-VF
 * routing is reached only when mlx5_vf_get_vfmig_iova_domain() returns
 * non-NULL, which its !CONFIG stub never does, so these are unreachable
 * at CONFIG_MLX5_VFMIG=n and exist purely to keep those call sites free
 * of #ifdef.
 */
static inline int
vfmig_iova_alloc_slot(struct vfmig_iova_domain *dom,
		      enum vfmig_iova_slot slot, u64 instance_key,
		      size_t size, gfp_t gfp,
		      dma_addr_t *iova_out, void **vaddr_out)
{
	return -EOPNOTSUPP;
}

static inline void
vfmig_iova_free_slot(struct vfmig_iova_domain *dom,
		     enum vfmig_iova_slot slot,
		     dma_addr_t iova, size_t size)
{
}

static inline int
vfmig_iova_transient_get(struct vfmig_iova_domain *dom,
			 size_t size, gfp_t gfp,
			 void **vaddr_out, dma_addr_t *iova_out)
{
	return -EOPNOTSUPP;
}

static inline void
vfmig_iova_transient_put(struct vfmig_iova_domain *dom,
			 dma_addr_t iova, size_t size)
{
}

#endif /* CONFIG_MLX5_VFMIG */

#endif /* __MLX5_CORE_VFMIG_IOVA_H__ */
