# DESIGN: reserving restorable IOVA ranges with `dma_iova_reserve()`

> **Status (2026-08-25): proposal; not implemented.**
>
> This document records the proposed DMA API needed to replace VFMIG's
> `dma_ops` override with an ordinary DMA-IOMMU-managed address space.

## 1. Problem

VFMIG must restore device-visible addresses because the mlx5 firmware image
contains IOVAs for objects such as user MRs, queue buffers, doorbell records,
and selected mlx5_core resources. Destination physical pages may differ from
the source pages, but the IOVAs referenced by restored firmware must remain
the same.

The normal DMA API chooses a new IOVA for each mapping. The existing
IOVA-based DMA API separates IOVA allocation from physical-page mapping, but
`dma_iova_try_alloc()` can only allocate an arbitrary range:

```c
bool dma_iova_try_alloc(struct device *dev,
			struct dma_iova_state *state,
			phys_addr_t phys, size_t size);
```

Its `phys` argument is used only to preserve the physical address's offset
within the IOMMU granule. Passing zero is valid for page-aligned mappings, but
the API cannot reserve a source-selected address during restore. Its boolean
return also conflates unsupported devices with allocation failure, which is
appropriate for an optional DMA optimization but not a mandatory migration
operation.

## 2. Proposed API

```c
#define DMA_IOVA_RESERVE_FIXED	BIT(0)

int dma_iova_reserve(struct device *dev,
		     struct dma_iova_state *state,
		     dma_addr_t requested_addr,
		     size_t size,
		     unsigned int flags);
```

The same API supports both sides of migration:

```c
/* Source: reserve any suitable range. state.addr is the result. */
ret = dma_iova_reserve(dev, &arena, 0, arena_size, 0);

/* Destination: reserve the range recorded by the source. */
ret = dma_iova_reserve(dev, &arena, saved_addr, arena_size,
		       DMA_IOVA_RESERVE_FIXED);
```

Using an explicit flag is preferable to assigning special meaning to address
zero or `DMA_MAPPING_ERROR`. It keeps the fixed-address request unambiguous
and leaves `state->addr` as the single output location.

The proposed return values are:

* `0`: range reserved and recorded in `state`.
* `-EOPNOTSUPP`: the device is not using DMA-IOMMU.
* `-EINVAL`: invalid size, alignment, DMA mask, bus limit, or aperture.
* `-EBUSY`: a fixed range overlaps an existing allocation or reservation.
* `-ENOSPC`: no suitable arbitrary range is available.
* `-ENOMEM`: allocator metadata allocation failed.

The reservation allocates only IOVA address space. It allocates no physical
memory and installs no IOMMU page-table entries.

## 3. Implementation model

For an arbitrary request, dma-iommu can reuse the existing path:

```text
iommu_get_dma_domain()
  -> domain->iova_cookie
    -> cookie->iovad
      -> iommu_dma_alloc_iova()
        -> alloc_iova_fast()
```

This preserves DMA-mask, bus-limit, aperture, deferred-attach, and IOVA-cache
handling.

A fixed request needs a new IOVA-layer primitive, conceptually:

```c
int alloc_iova_fixed(struct iova_domain *iovad,
		     unsigned long pfn_lo,
		     unsigned long pfn_hi,
		     struct iova **result);
```

It must atomically allocate exactly the requested interval or fail. In
particular, it must account for the IOVA range caches and reject every
overlap.

The existing `reserve_iova()` is not sufficient as the public implementation.
It is intended to mark platform-owned windows unavailable, may merge
overlapping reservations, does not provide an independently owned runtime
allocation, and does not by itself provide the DMA-IOMMU validation above.

## 4. Mapping within the reservation

No new page-mapping API is required for the initial implementation.
`dma_iova_link()` already maps a physical range at an offset within a
`dma_iova_state`:

```c
ret = dma_iova_link(dev, &arena, phys, object_offset, object_size,
		    DMA_BIDIRECTIONAL, attrs);
```

The resulting device address is:

```text
arena.addr + object_offset
```

Multiple links may be batched before making the page-table updates visible:

```c
for_each_page(page) {
	ret = dma_iova_link(dev, &arena, page_to_phys(page), offset,
			    PAGE_SIZE, direction, attrs);
	if (ret)
		goto rollback;
	offset += PAGE_SIZE;
}

ret = dma_iova_sync(dev, &arena, object_offset, object_size);
```

`dma_iova_sync()` synchronizes IOMMU page-table updates; it is not a CPU-cache
ownership operation. `dma_iova_link()` and `dma_iova_unlink()` retain the
normal DMA direction, protection, SWIOTLB, and non-coherent architecture
handling.

Individual objects are removed with:

```c
dma_iova_unlink(dev, &arena, object_offset, object_size,
		direction, attrs);
```

After every object has been unlinked, `dma_iova_free()` releases the complete
arena reservation.

## 5. Existing precedents

Current `dma_iova_*` users fall into two patterns:

* **VFIO mlx5** maps separately allocated pages backing migration-image and
  page-tracker MKEYs into one dense IOVA range. Sequential IOVAs are written
  into the MKEY MTT, all page-table updates share one `dma_iova_sync()`, and
  `dma_map_page()` per page is the fallback. This is an optimization; the MTT
  can also contain unrelated DMA addresses.
* **The block layer** similarly coalesces a request's physical vectors into
  one DMA range, reducing device descriptor pressure and batching IOTLB
  synchronization.
* **HMM/ODP** is the closest arena precedent. It reserves an IOVA state for
  the full faultable range, then dynamically links and unlinks individual PFNs
  at index-derived offsets as pages enter and leave the working set. HMM
  rejects devices that require CPU cache synchronization, as users of this
  path cannot perform streaming-DMA ownership transitions for every faulted
  page; it also rejects DMA-address-limited devices to avoid SWIOTLB buffering.
* **The dma-buf physical-vector helper** uses `DMA_ATTR_MMIO` to place P2P
  device-memory ranges behind an IOVA-backed SG entry when traffic traverses
  the host bridge. Its current multi-range loop passes offset zero for every
  vector and should not be treated as a VFMIG model until that apparent
  overlap is resolved.
* **mlx5_core VFMIG staging buffers** duplicate the VFIO mlx5 MKEY pattern.
  These are temporary SAVE/LOAD command buffers, not the deterministic IOVAs
  referenced by restored firmware objects.

None of these callers supplies a numerical base IOVA. They accept the base
chosen by `dma_iova_try_alloc()` and control only offsets within it. VFMIG
combines HMM's long-lived, dynamically populated arena with VFIO mlx5's dense
page-linking pattern; restoring the arena at a caller-specified base is the
new capability.

## 6. VFMIG lifecycle

```text
Source SET_TRACKED
  -> reserve arbitrary arena
  -> suballocate mlx5 objects within the arena
  -> link physical pages at object offsets
  -> save arena base, size, and object offsets

Destination SET_TRACKED / LOAD
  -> reserve the saved arena base and size with FIXED
  -> recreate or pin destination physical pages
  -> link them at their saved object offsets
  -> restore firmware state that references those IOVAs

Teardown
  -> unlink every live object
  -> free the arena reservation
```

VFMIG may keep its slot or range allocator for suballocation. The DMA layer
owns the outer arena in the normal DMA-IOMMU allocator; VFMIG owns placement
inside that arena.

## 7. Coherent allocations

This proposal initially covers existing or pinned pages mapped with
`dma_iova_link()`. It does not by itself provide a fixed-IOVA replacement for
`dma_alloc_coherent()`.

Coherent allocation couples three lifetimes that a restorable arena separates:

1. CPU-visible backing memory and architecture-specific cache attributes.
2. IOMMU page-table mappings for an individual object.
3. The outer IOVA reservation retained across many object allocations.

A future fixed-IOVA coherent helper could reuse dma-iommu's page allocation,
`arch_dma_prep_coherent()`, CPU remapping, and IOMMU mapping internals, but it
would need a paired free operation that removes only the object's mapping and
backing pages without returning the containing arena to the IOVA allocator.
Atomic pools, non-coherent ARM devices, DMA attributes, and memory encryption
would all need defined behavior. This is deliberately separate from the
minimal reservation proposal.

## 8. Coherent-only initial profile

VFMIG can avoid proposing a fixed-IOVA variant of `dma_alloc_coherent()` in
its initial implementation by requiring a DMA-coherent device:

```c
if (!use_dma_iommu(dev))
	return -EOPNOTSUPP;
if (!dev_is_dma_coherent(dev))
	return -EOPNOTSUPP;
```

On such a device, `dma_iova_link()` installs mappings with `IOMMU_CACHE`,
ordinary cached kernel mappings are valid for CPU/device-shared pages, and
the architecture cache-maintenance operations in the link/unlink path are
unnecessary. VFMIG can therefore:

1. Reserve the migratable arena with `dma_iova_reserve()`.
2. Allocate ordinary, page-aligned backing pages for persistent kernel
   resources and link them into their assigned arena offsets with
   `DMA_BIDIRECTIONAL`.
3. Link pinned user pages into their saved offsets in the same way.
4. Keep non-restorable resources, such as command mailboxes, on the ordinary
   DMA API outside the arena.
5. Reject VFMIG on genuinely non-coherent devices.

The reserved arena must satisfy `dev->coherent_dma_mask` when it contains
resources that previously came from `dma_alloc_coherent()`. Call sites must
also be audited for assumptions beyond cache coherence, including contiguous
CPU virtual mappings, atomic allocation, special DMA attributes, highmem,
and memory encryption. The current mlx5 tracked resources are favorable:
the command ring is one page, and EQ, queue, and doorbell buffers are already
managed as page fragments.

This is a deliberately constrained implementation, not a general claim that
streaming mappings replace coherent allocation. `dma_iova_link()` belongs to
the streaming DMA API family even when its hardware mapping is coherent.
DMA maintainers may require an explicit coherent-at-reserved-IOVA API to
preserve the formal coherent-allocation contract. If so, that becomes a
follow-on to the reservation API rather than a prerequisite for validating
the underlying IOVA design.

The check is on the device property, not the CPU architecture. An arm64
platform may provide fully coherent PCIe DMA, while an embedded x86-attached
device or a non-PCI device may have different constraints. The authoritative
runtime predicate is `dev_is_dma_coherent(dev)`.

## 9. Initial scope

The minimal proposed change is:

1. Add an exact, collision-detecting internal IOVA allocation primitive.
2. Add `dma_iova_reserve()` for arbitrary source and fixed destination ranges.
3. Use existing `dma_iova_link()`, `dma_iova_sync()`,
   `dma_iova_unlink()`, and `dma_iova_free()` for page-backed objects.
4. Keep fixed-IOVA coherent allocation and optional page/SG convenience
   wrappers as follow-on work.

This removes the need to override `dma_ops` for page-backed VFMIG objects,
keeps the device on the normal DMA-IOMMU path, and makes restored IOVA
ownership explicit in the DMA layer.
