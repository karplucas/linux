# DESIGN: split PASID domains for mlx5 VFMIG

> **Status (2026-08-31): proposal; firmware capability investigation
> required before implementation.**
>
> This document explores moving restorable user DMA into an ib_core-managed
> PASID domain, replacing VFMIG's device-wide `dma_ops` override. It also
> compares, as a separate second-stage decision, PASID and non-PASID options
> for tracked mlx5_core allocations. It separates what Linux can implement
> today from what mlx5 hardware and firmware must already support or expose
> through new commands.

## 1. Goal

VFMIG must restore the IOVAs embedded in `SAVE_VHCA_STATE` while mapping
them to newly allocated destination pages. The current proof of concept
attaches one unmanaged IOMMU domain to the VF and overrides `dma_ops`.
That gives every mlx5 DMA transaction deterministic IOVAs, but it also:

* replaces the normal DMA-IOMMU path for the entire VF;
* requires broad interception of unrelated mlx5_core and mlx5e DMA;
* relies on architecture-specific `dma_ops` behavior; and
* makes coherent allocation and teardown difficult to reason about.

PASID can let one PCI function use multiple IOMMU domains concurrently.
The primary opportunity is to give ib_core-managed user memory an isolated,
restorable IOVA namespace without replacing the VF's ordinary DMA domain.

## 2. Two-stage design

### 2.1 Stage 1: ib_core user domain

The primary design has two address spaces:

```text
Destination VF RID
  |
  +-- no PASID --> Ddefault
  |                 mlx5e
  |                 all mlx5_core allocations
  |                 temporary command mailboxes and staging buffers
  |
  +-- Puser ----> Duser
                    user MR pages
                    user QP/SRQ/WQ buffers
                    user CQ buffers
                    user doorbell-record pages
```

`Ddefault` is the normal DMA domain attached to the VF's IOMMU group.
Traffic using it carries the PCI Requester ID (RID) but no PASID.

`Duser` is an additional paging domain attached with
`iommu_attach_device_pasid()`. Its IOVA namespace is independent of
`Ddefault`.

The destination RID need not equal the source RID. Attaching the domains
to the destination VF binds them to the destination RID. VFMIG preserves
the IOVAs used inside each logical domain, not the source PCI topology.

The architectural goal is to abstract `Puser/Duser` into the RDMA core:
ib_core owns the logical user DMA context, IOVA allocation, and restored
page binding; the hardware driver supplies device-specific operations for
creating the context and associating uobjects with it. The first
implementation may remain mlx5-specific until the abstraction is proven.

Tracked mlx5_core allocations remain in `Ddefault` and use one of the
placement mechanisms compared in Stage 2.

### 2.2 Stage 2: choose the mlx5_core placement mechanism

Core objects such as the command ring and firmware pages still require
stable IOVAs because their addresses are present in the firmware image.
There are three candidate approaches:

1. Keep the existing explicit mlx5 allocation/deallocation hooks, but make
   them operate safely within the normal DMA domain.
2. Reserve a DMA-managed fixed-IOVA carveout and suballocate tracked core
   objects within it, as explored by `dma_iova_reserve.md`.
3. Add a second PASID/domain, `Pcore/Dcore`, for tracked core resources.

`Pcore/Dcore` provides an independent IOVA namespace and stronger
isolation from ordinary mlx5e/core DMA. Those are real benefits, but they
come with a command-ring bootstrap problem and require every relevant
firmware DMA engine to emit `Pcore`. Unless firmware already provides
pre-bootstrap, vHCA-scoped PASID steering, the explicit-hook or
DMA-carveout approaches are likely simpler and less invasive.

Stage 1 therefore does not depend on `Pcore/Dcore`. Stage 2 evaluates it
alongside the non-PASID alternatives rather than treating it as the target.

## 3. Linux domain-selection semantics

An IOVA does not select an IOMMU domain. The effective lookup key is:

```text
(destination RID, optional PASID, IOVA)
```

The IOMMU first uses the RID and optional PASID to select a domain, then
looks up the IOVA in that domain.

Linux represents untagged DMA with `IOMMU_NO_PASID`. IOMMUFD makes the
two attachment paths explicit:

```c
if (pasid == IOMMU_NO_PASID)
	iommu_attach_group_handle(domain, group, handle);
else
	iommu_attach_device_pasid(domain, dev, pasid, handle);
```

The IOMMU core stores the current/default RID domain separately from
PASID attachments:

```c
struct iommu_group {
	struct xarray pasid_array;
	struct iommu_domain *default_domain;
	struct iommu_domain *domain;
	/* ... */
};
```

Consequently, `Ddefault` and `Duser` can exist at the same time; an
optional `Dcore` could be added later. The default domain must be
PASID-compatible, and the IOMMU driver must implement `set_dev_pasid`.

Calling `iommu_map(Duser, saved_iova, new_phys, ...)` only installs a
translation. It does not cause mlx5 to issue DMA using `Puser`.

```text
RID + saved_iova         --> Ddefault
RID + Puser + saved_iova --> Duser
```

The endpoint must put `Puser` in the PCIe transaction. This is the
principal hardware/firmware dependency of the design.

## 4. Externally managed PASID contract

The initial design deliberately preserves the numerical PASID across
migration. PASID availability is a provisioning requirement, like other
source/destination compatibility requirements, rather than something
VFMIG negotiates during restore.

```text
Host/PF initialization:
  reserve a small PASID pool for migration
  advertise that pool to the migration manager

Before source uobjects are created:
  top-level manager chooses Puser from the pool
  attach Duser at Puser
  configure the UID/uobjects to emit Puser

Destination admission:
  require the same Puser to be reserved and supported

Destination restore:
  attach restored Duser at the saved Puser
  install saved IOVAs with destination physical pages
  LOAD and restore uobjects
  RESUME only after all mappings are ready
```

The image or its host-side manifest records `Puser`:

```text
{ user_pasid, object identity, IOVA, length }
```

The existing `(kind, fw_id, iova, length)` user-page records already
identify user objects. A stream-level `user_pasid` is sufficient if all
user records in the image share one domain.

### 4.1 Reservation requirements

PASIDs are stable for the lifetime of an attachment, and
`iommu_attach_device_pasid()` accepts a caller-selected number. Linux's
global PASID allocator does not, however, provide a public API to reserve
a specific number without attaching it:

```c
/* Conceptual; does not exist today. */
int iommu_reserve_global_pasid(struct device *dev, ioasid_t pasid);
```

The externally managed design needs one of:

1. a kernel-configured PASID range excluded from ordinary/SVA allocation;
2. a fixed-number global reservation API; or
3. a boot-time owner that reserves the migration pool before competing
   PASID users start.

Once a VF exists, the driver can additionally hold its selected PASID by
attaching an empty or blocked domain, then atomically replace that
attachment with `Duser` at restore. This protects the VF/group slot but
does not replace global pool reservation.

Hardware translation is keyed by `(RID, PASID)`, so the same numerical
PASID can in principle be reused by isolated VF RIDs. Linux also has a
global PASID allocator for uses such as SVA. The initial implementation
should use a globally reserved pool rather than bypassing that ownership
model.

Migration admission fails early if the destination:

* did not reserve the image's `Puser`;
* cannot represent that PASID width;
* already attached it to an incompatible domain; or
* cannot provide the required mlx5 PASID steering.

### 4.2 Optional future late binding

A future, more portable profile may allow:

```text
source Puser = 17
destination Puser = 42
```

That would require firmware PASID remapping or stopped-state rebinding.
It is not required for the externally managed fixed-PASID profile.

## 5. Host implementation outline

### 5.1 Domain setup

During controlled VF initialization, before user objects can issue DMA:

1. Leave the normal PASID-compatible DMA domain attached as `Ddefault`.
2. Claim externally selected `Puser` from the PF's reserved pool.
3. Allocate a PASID-capable paging domain `Duser`.
4. Attach `Duser` at `Puser` with `iommu_attach_device_pasid()`.
5. Keep the PASID attachment alive until every associated DMA engine is
   quiesced and every object is destroyed.

Unlike the current design, do not replace the RID domain, install custom
`dma_ops`, or change `dev->dma_iommu`.

`iommu_attach_device_pasid()` is a device-driver API. The `Puser`
attachment belongs in RDMA-device or ucontext initialization, not in the
current pre-bind PF ioctl path. It need only precede creation of the first
object that can emit `Puser`.

### 5.2 User memory

On ordinary creation, the mlx5_ib path must allocate an IOVA in `Duser`,
map the pinned pages there, and populate the umem SG DMA addresses with
those IOVAs.

On restore:

```text
RESTORE_X(kind, fw_id, user VA, length)
  -> pin destination user pages
  -> find saved USER_DMA IOVA range by (kind, fw_id)
  -> iommu_map(Duser, saved IOVA, destination physical pages)
  -> populate sg_dma_address()
  -> adopt/reseed the saved firmware object
  -> associate its DMA with Puser
```

The existing explicit `ib_umem_pin()` plus
`vfmig_iova_bind_user_object()` shape remains useful. The primary change
is that binding targets `Duser`, while fresh registrations also need a
normal `Duser` allocation path that no longer depends on `dma_ops`.

`ib_core` may own generic pinning and logical DMA-context plumbing, but
mlx5_ib must perform device-specific PASID programming. An IOMMU
attachment tells the IOMMU how to translate an incoming PASID; it does
not tell the HCA which PASID to emit.

### 5.3 Core memory

Stage 1 leaves all core DMA untagged in `Ddefault`. Only
FW-image-referenced allocations need deterministic placement. Stage 2
chooses between explicit mlx5 allocation hooks, a DMA-managed carveout,
and the optional `Pcore/Dcore` design after comparing implementation cost
and firmware support.

## 6. Firmware contracts to validate

These are capability questions first. A firmware change is needed only
where the current device cannot satisfy the contract.

### F1. User-domain steering scope

Determine whether firmware can associate `Puser` with:

1. an entire vHCA;
2. a UID/ucontext and all objects below it; or
3. only individual object contexts, such as an MKEY.

A vHCA-wide PASID would also capture kernel and mlx5e traffic and would
defeat the Stage 1 split. A UID-wide setting is the preferred user-object
model:

```text
UID U --> Puser
all QP/CQ/SRQ/MKEY/DBR DMA under U carries Puser
```

If PASID is programmable only in MKEY context, MR payload DMA may work
while WQE fetch, CQE write, and doorbell-record access remain untagged.
Those engines need equivalent steering.

### F2. Complete user-object coverage

The following accesses must carry `Puser`:

* MR payload reads and writes;
* QP/SQ/RQ WQE fetches and writes;
* CQE writes;
* SRQ/WQ accesses; and
* doorbell-record reads and writes.

PDs, AHs, and other objects with no host DMA backing need no IOMMU
mapping, although their parent UID may determine the PASID inherited by
dependent DMA-producing objects.

### F3. Optional core-domain steering and bootstrap

This contract is a Stage 2 investigation, not a Stage 1 requirement. For
`Pcore/Dcore` to beat the non-PASID alternatives, firmware must issue
`Pcore` for:

* command-ring DMA;
* firmware page accesses;
* EQ DMA; and
* every other FW-image-referenced mlx5_core buffer.

`Pcore` must be configurable before the first access to the command ring.
Potential firmware interfaces include:

* a PF-mediated command that assigns a PASID to a child vHCA before VF
  probe;
* a PASID supplied with the migration-enable or pre-load operation; or
* a bootstrap ring in `Ddefault` used only to install the final `Pcore`
  ring before normal operation.

Without one of these, core resources remain in `Ddefault` and use one of
the non-PASID placement approaches.

### F4. SAVE/LOAD fixed-PASID persistence

We must determine whether `SAVE_VHCA_STATE`:

* excludes PASIDs;
* saves and restores the literal PASID value; or
* requires the host to reapply the same PASID before or after `LOAD`.

The initial contract requires:

```text
source Puser = 17
destination Puser = 17
```

Acceptable firmware behavior is:

1. `LOAD_VHCA_STATE` restores the saved PASID unchanged;
2. PASID association is external to the image and configured with the
   saved value before `LOAD`; or
3. the same PASID can be reapplied after `LOAD` while the vHCA remains
   stopped, before `RESUME_VHCA`.

No PASID-remapping firmware interface is required for this profile.
Different-PASID restore remains an optional future extension.

### F5. Quiesce, ATS, and invalidation

PASID attachment and any post-LOAD reapplication must occur while the
vHCA is stopped. Before resume, firmware and the IOMMU must invalidate
stale address-translation state, including any PASID-tagged device ATC
entries when ATS is enabled.

The destination test must use different physical backing for the same
IOVA so that stale translation reuse is observable.

### F6. Fault containment

An object assigned to `Puser` must not silently fall back to untagged
DMA if its PASID translation is missing. It must fault or report a
deterministic device error. Silent fallback would defeat both isolation
and the ability to prove coverage.

## 7. Firmware-validation harness plan

The tests should distinguish domain selection by mapping the same IOVA
to different sentinel pages:

```text
Ddefault: IOVA X --> page A
Duser:    IOVA X --> page B
```

An operation that writes a known value to IOVA `X` proves its selected
domain by whether page A or page B changes. An unmapped guard page and
IOMMU fault logging should be used where a read-only discriminator is
safer.

### Gate 0: platform capability

Add `pasid/test_pasid_domain_coexistence.sh` and a small kernel-side
test hook:

1. Report VF PASID width and IOMMU support.
2. Confirm the default DMA domain remains attached.
3. Attach `Duser` at a test PASID.
4. Verify ordinary untagged mlx5 DMA still works through `Ddefault`.
5. Detach `Duser` and verify clean teardown with no outstanding faults.

Pass proves only Linux/IOMMU plumbing, not mlx5 PASID emission.

### Gate 1: one explicit mlx5 DMA operation

Add `pasid/test_pasid_dma_select.sh`:

1. Install conflicting sentinel mappings at IOVA `X`.
2. Program one firmware object with test PASID `Puser`.
3. Trigger one bounded DMA operation.
4. Assert only the `Duser` sentinel changed.
5. Remove the PASID association and assert the same operation uses
   `Ddefault` or fails exactly as specified.

Start with whichever object has an existing documented PASID field.
This gate answers whether current firmware can emit a host-programmed
PASID at all.

### Gate 2: user-object matrix

Add `pasid/test_pasid_uobject_matrix.sh` with one independent subtest per
DMA-producing path:

```text
MR payload       expected Duser
QP WQE fetch     expected Duser
CQE write        expected Duser
SRQ/WQ access    expected Duser
DBR access       expected Duser
```

Do not infer complete coverage from a successful MR test. Record the
smallest firmware scope that passes: MKEY, object, UID, or vHCA.

Extend the existing restore probes after basic steering works:

* `uobject_restore/mr_restore/test_mr_restore_mlx5_vfmig.sh`
* `uobject_restore/cq_restore/test_cq_restore_mlx5_vfmig.sh`
* `uobject_restore/qp_restore/test_qp_restore_mlx5_vfmig.sh`
* `save_load/user_object_replay/test_user_object_replay.sh`

Each must assert end-to-end data movement, not only restored FW identity
or `awaiting_bind` counts.

### Gate 3: UID isolation

Add `pasid/test_pasid_multi_uid.sh`:

1. Create two ucontexts with distinct UIDs.
2. Assign different PASIDs/domains.
3. Give both domains the same numerical IOVA mapped to different pages.
4. Run concurrent QP/MR/CQ traffic.
5. Assert each UID reaches only its own backing pages.

This is the decisive test for a UID-wide design and for multiple
processes sharing one VF.

### Gate 4: optional core-object matrix

Add `pasid/test_pasid_core_dma.sh`:

1. Bootstrap `Pcore` using the proposed firmware interface.
2. Place the command ring in `Dcore` and execute a harmless command.
3. Exercise EQ delivery and firmware-page access independently.
4. Deliberately omit each required mapping in turn and verify a bounded,
   diagnosable failure instead of a 60-second probe wedge.

This is a Stage 2 comparison input. Failure does not block `Puser`; it
selects explicit core hooks or a DMA-managed carveout. Even a pass does
not automatically select `Pcore`: its complexity must still be compared
with those alternatives.

### Gate 5: externally managed fixed-PASID SAVE/LOAD

This is the Stage 1 migration acceptance test:

```text
source Puser = destination Puser
```

Extend `save_load/test_iova_tracked_save_load.sh` to leave `Ddefault`
active, select `Puser` from the externally reserved pool, attach `Duser`,
record `Puser` in the manifest, replay user IOVAs into it, restore
uobjects, and run an RDMA data-path operation.

The destination must reject the image before `LOAD` if the recorded PASID
is unavailable. Pass proves that SAVE/LOAD preserves or permits
reapplication of the externally managed PASID.

### Gate 6: optional different-PASID SAVE/LOAD

This is a future portability test, not a Stage 1 requirement:

```text
source Puser      = 17
destination Puser = 42
same saved IOVAs
different destination physical pages
```

The harness must:

1. prove source traffic used `Puser=17`;
2. save and tear down the source;
3. attach destination `Duser` at 42;
4. load with the firmware remapping/rebinding mechanism;
5. restore every user mapping at its source IOVA;
6. resume only after PASID and ATS invalidation complete; and
7. prove traffic reaches only destination pages through PASID 42.

This gate must fail if the source PASID is accidentally retained. Passing
Gate 5 alone is sufficient for the externally managed profile, but makes
no claim about late binding.

### Gate 7: negative and lifecycle coverage

Add assertions for:

* unsupported PASID capability and insufficient PASID width;
* configured migration-pool reservation conflicts;
* PASID collision during attach;
* destination admission when the image PASID is not reserved;
* missing `Duser` mapping;
* wrong PASID programmed into an object;
* detach attempted before DMA quiesce;
* repeated create/destroy without PASID or IOVA leakage;
* source/destination ATS enabled and disabled; and
* rollback after partial RESTORE failure.

All potentially wedging operations need bounded waits and stable dmesg
sentinels, following `bind_vf_safe()` in the existing SAVE/LOAD harness.

## 8. Decision gates

Proceed with Stage 1 after answering:

1. Can the VF and host IOMMU concurrently support a default domain and
   at least one PASID paging domain?
2. What firmware scope selects PASID: MKEY, object, UID, or vHCA?
3. Does that scope cover every user-memory DMA path?
4. Can the OS reserve an externally selected PASID pool before competing
   users, and can migration admission verify the image's PASID?
5. Does SAVE/LOAD preserve or allow reapplication of the same PASID?
6. Are PASID-tagged ATS entries invalidated before resume?

Then make the independent Stage 2 decision:

7. Can core DMA select `Pcore` before command-ring bootstrap?
8. Does `Pcore/Dcore` provide enough isolation or implementation
   simplification to beat explicit hooks or a DMA-managed carveout?

Expected outcomes:

```text
Gates 0-3, 5, and applicable Gate 7 checks pass:
  adopt Stage 1: externally managed fixed Puser/Duser + Ddefault.

Gate 1 or Gate 2 fails:
  PASID does not currently solve VFMIG user-memory placement;
  continue with explicit allocation in the normal DMA domain.

Gate 5 passes and optional Gate 6 fails:
  the externally managed profile is supported; do not claim
  different-PASID portability.

Optional Gate 4 passes and beats the alternatives:
  add Pcore/Dcore as a Stage 2 extension.

Optional Gate 4 fails or offers no net simplification:
  keep core DMA in Ddefault and use explicit mlx5 hooks or a
  DMA-managed carveout.
```

## 9. Out of scope

This first investigation does not cover:

* ODP/PRI fault-driven mappings;
* GPU dma-buf and PCIe P2P;
* DEVX-created objects that bypass the managed restore paths;
* non-coherent DMA platforms; or
* a generic cross-driver ib_core PASID ABI.

Those should follow only after ordinary pinned user memory passes the
fixed-PASID SAVE/LOAD gate.

## 10. Source references

* `include/linux/iommu.h` — `IOMMU_NO_PASID`,
  `iommu_attach_device_pasid()`, and `iommu_detach_device_pasid()`.
* `drivers/iommu/iommu.c` — separate `default_domain`, current `domain`,
  and `pasid_array`; group-wide PASID attachment.
* `drivers/iommu/iommufd/device.c` — explicit no-PASID versus PASID
  attachment dispatch and compatibility checks.
* `tools/testing/criu_rdma/design/user_mr_dma.md` — current user-page
  replay and explicit restored-umem binding design.
* `tools/testing/criu_rdma/design/dma_iova_reserve.md` — alternative
  fixed-IOVA reservation design for a normal DMA domain.
* `tools/testing/criu_rdma/save_load/test_iova_tracked_save_load.sh` —
  current primary end-to-end deterministic-IOVA harness.
