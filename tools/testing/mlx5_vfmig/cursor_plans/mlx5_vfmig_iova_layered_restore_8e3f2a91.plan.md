---
name: mlx5 vfmig deterministic-IOVA layered restore
overview: "Supersedes the M2-revised plan. M2-revised proved the SAVE+LOAD ioctl plumbing is clean: blobs round-trip end-to-end, LOAD_VHCA_STATE returns success, and the VHCA reaches a state firmware considers loaded. What it then exposed is a fundamental, non-driver-bug constraint: SAVE_VHCA_STATE captures source-side host-physical/IOVA addresses (cmd ring, MANAGE_PAGES pool, EQ buffers, MTT roots) inside the FW state blob, and on a native (non-VFIO-passthrough) destination those addresses are not reproducible -- so although LOAD succeeds, the destination VHCA's command interface is permanently dead because firmware is dereferencing source addresses that point to nothing on the destination host. This plan is the work to *make* those addresses reproducible by giving each migratable VF its own IOMMU domain plus a deterministic IOVA allocator under our control, then progressively bringing every kernel-internal DMA-mapped buffer under that allocator. Work is organized as five layers. Each layer has a single, narrow, falsifiable success criterion. We commit code only to one layer at a time and re-plan after each result."
todos:
  - id: l0_kconfig
    content: "Layer 0 prereq: gate the entire vfmig + vfmig_iova subsystem behind a new CONFIG_MLX5_VFMIG (default y when CONFIG_MLX5_CORE=y). All vfmig.c, vfmig_iova.c, and related ioctl/cdev plumbing compile out cleanly when disabled, leaving stock mlx5_core behavior. This closes a gap that was already latent in M2-revised: the existing vfmig.c had no Kconfig and shipped unconditionally."
    status: pending
  - id: l0_probe_mode
    content: "Layer 0 prereq: introduce a per-VF 'tracked' mode that is decided at probe time, not at restore time. New ioctl MLX5_VFMIG_IOC_SET_TRACKED { vf_id, enable } toggles a flag on the PF's vfs_ctx[vf_id]. mlx5_cmd_enable, pages.c, and friends consult this flag at probe time to decide between vfmig_iova_alloc_coherent vs the stock dma_alloc_coherent path. Setting must happen before VF bind; toggle is rejected if VF is currently bound."
    status: pending
  - id: l0_iommu_domain
    content: "Layer 0: per-VF unmanaged IOMMU domain + deterministic IOVA bump allocator + DMA page registry, in a new drivers/net/ethernet/mellanox/mlx5/core/vfmig_iova.c. APIs: vfmig_iova_attach(pdev) / vfmig_iova_detach(pdev), vfmig_iova_alloc_coherent(dev, size, *iova_out, **vaddr_out), vfmig_iova_map_existing(dev, iova, page, len), vfmig_iova_replay(dev, saved[]). Domain is owned by vfmig PF state, persists across VF bind/unbind cycles, destroyed only by explicit MLX5_VFMIG_IOC_SET_TRACKED { enable=0 } or PF unload. No mlx5_core integration yet; sanity check: bring a VF up, traffic still flows through our domain."
    status: pending
  - id: l1_cmd_ring
    content: "Layer 1: route mlx5_cmd_enable's cmd-page allocation through vfmig_iova_alloc_coherent for migratable VFs; record the cmd-ring IOVA + page contents into the SAVE blob (new wire tag VFMIG_WIRE_TAG_HOST_PAGE); on LOAD, replay the page at the recorded IOVA before issuing LOAD_VHCA_STATE. Pass criterion: post-LOAD QUERY_HCA_CAP completes (no DISABLE_HCA timeout in dmesg)."
    status: pending
  - id: l1_milestone_check
    content: "Layer 1 review gate: confirm result, decide whether to proceed to Layer 2. If [1] fails, do not start [2]; root-cause the cmd-ring path before any further allocator hooks."
    status: pending
  - id: l2_manage_pages
    content: "Layer 2: hook the MANAGE_PAGES path in pages.c so every page given to FW (boot, init, dynamic OP_GIVE) comes from vfmig_iova_alloc_coherent. SAVE captures (iova, contents) for every live entry in the per-VF page registry. LOAD replays them all before LOAD_VHCA_STATE. Pass criterion: mlx5_function_open completes on the destination (QUERY_HCA_CAP, SET_HCA_CAP, etc. all succeed)."
    status: pending
  - id: l3_eqs_uars
    content: "Layer 3: skip mlx5_eq_table_create / mlx5_alloc_bfreg on restored VFs and rehydrate struct mlx5_eq, struct mlx5_uars_page, struct mlx5_bfreg_info from saved state instead. Capture EQ ring buffers + UAR pages + doorbell records via the same wire tag as L2. Preserve FW-side EQN/UAR-index identity. Pass criterion: mlx5_load completes, mlx5_ib attaches, /sys/class/net/<vfX> appears, ip link up + basic ping works."
    status: pending
  - id: l_crosshost
    content: "Cross-host milestone (after Layer 3): take a blob produced on host A and restore it on host B. Validates that we have not silently encoded host-A-specific assumptions. Will likely surface FW-version skew and BAR-layout differences. Trigger to provision a sister host."
    status: pending
  - id: l4_user_resources
    content: "Layer 4: user-side CQ/QP/MR buffer IOVA preservation. Either SVA-based (user VA == IOVA enforced via the IOMMU SVA path) or strict policy in ib_umem. CRIU plugin orchestrates SUSPEND, blob+page dump, user memory dump, and replay. Acceptance: rping survives checkpoint/restore on the same host; later, across hosts."
    status: pending
  - id: rxe_track
    content: "Parallel track (background): exercise the SAVE/LOAD ioctl surface and CRIU integration against rxe (soft-RoCE) so the userspace + CRIU side can advance independently of mlx5 hardware-state preservation. Decoupled from layers above; useful for shaking out CRIU plugin / ibverbs interaction issues."
    status: pending
isProject: true
---

## Why this plan supersedes M2-revised

M2-revised (`mlx5_vfmig_m2r_save_load_roundtrip_c3a9f2e1.plan.md`) finished its implementation goals. Concretely, it delivered:

- A complete in-driver SAVE ioctl mirroring the LOAD ioctl, with anon-inode fd, parser, FSM, PD/MKEY/page registration.
- Round-trip plumbing on a single host with no VFIO and no IOMMU dependency.
- A deferred-load architecture in which `LOAD_VHCA_STATE` and `RESUME_VHCA` execute during the destination VF's `mlx5_core` probe rather than at LOAD-ioctl time.
- Fixes for several intermediate failure modes: missing migration cap/migratable bit, latent FSM bug in `vfmig_load_write`, command-ordering issues around `LOAD` vs the destination VHCA's `ENABLE_HCA(self)`.

What it then exposed -- and what is documented in `tools/testing/mlx5_vfmig/README.md` and the comment block in `main.c`'s restored-VF branch -- is a structural problem that no amount of command reordering can fix:

> `SAVE_VHCA_STATE` produces a firmware-opaque blob whose internal pointers are the **source host's IOVAs** (or DMA addresses, in pre-IOMMU mode). On a destination with a different memory layout, those IOVAs do not refer to the same pages. `LOAD_VHCA_STATE` therefore *succeeds* (FW accepts the blob), but every subsequent operation that dereferences a saved IOVA -- starting with the very first command sent through the cmd ring -- silently goes nowhere, and the destination VHCA's command interface is permanently unresponsive.

This is by design: the `SAVE_VHCA_STATE`/`LOAD_VHCA_STATE` pair was specified for the **VFIO mlx5 live-migration** use case, where source and destination guests share a guest-physical address space (the IOVAs that appear in the blob refer to guest pages, and the host-side IOMMU on the destination is configured by VFIO/QEMU to map the same guest-physical pages back to *some* host-physical pages). In a guest-less, host-driver-native setting, no equivalent indirection exists.

This plan replaces "make the firmware accept the blob" (already done) with "give the firmware an address space it can keep using after migration." It does so by:

1. Putting each migratable VF in a per-VF unmanaged IOMMU domain we own.
2. Allocating IOVAs deterministically out of that domain, recording them.
3. On restore, recreating the same IOVA → page bindings before any FW command.

## Strategic shape

The end goal is full RDMA workload checkpoint/restore, but each layer below is independently useful and has a sharp, falsifiable success criterion. We re-plan between layers; the timeline beyond the current layer is intentionally not committed.

| Layer | Deliverable | Falsifiable criterion |
|---|---|---|
| 0 | Per-VF IOMMU domain + deterministic allocator + page registry. No mlx5 integration. | VF still passes traffic with our domain attached; allocator unit tests pass. |
| 1 | Cmd ring at deterministic IOVA, contents preserved across SAVE/LOAD. | Post-LOAD `QUERY_HCA_CAP` returns success in dmesg (no `DISABLE_HCA timeout`). |
| 2 | All MANAGE_PAGES pages preserved at deterministic IOVAs, contents shipped. | Destination `mlx5_function_open` completes without error. |
| 3 | EQs + UARs preserved; restored-VF probe skips their creation and rehydrates kernel data structures. | Destination `mlx5_load` completes; netdev appears; basic ping works. |
| Cross-host | Blob from host A restored on host B, post-Layer-3 functionality. | Same as Layer 3 acceptance, but across machines. |
| 4 | User-resource (CQ/QP/MR) IOVA preservation; CRIU plugin. | rping survives C/R; later, real workload survives C/R across hosts. |

Layer ordering is not negotiable: each layer's success criterion presupposes the prior layer working, and each layer's code touches a strict superset of the prior layer's hooks.

## Layer 0: IOMMU domain + deterministic allocator + page registry

### Layer 0 prereq A -- Kconfig

We add `CONFIG_MLX5_VFMIG` in `drivers/net/ethernet/mellanox/mlx5/core/Kconfig`, default `y` when `MLX5_CORE=y` for development convenience but cleanly tristate so distros / production can disable. Files newly gated:

- `drivers/net/ethernet/mellanox/mlx5/core/vfmig.c` (already present, currently unconditional -- closing this gap is a prereq, not new code)
- `drivers/net/ethernet/mellanox/mlx5/core/vfmig.h`
- `drivers/net/ethernet/mellanox/mlx5/core/vfmig_iova.{c,h}` (new in this plan)
- The vfmig cdev registration in `dev.c` / `main.c`
- The restored-VF branches in `mlx5_function_enable` / `mlx5_function_open`
- The `MLX5_VFMIG_IOC_*` ioctls in `include/uapi/linux/mlx5_vfmig.h`

When `CONFIG_MLX5_VFMIG=n`, `mlx5_core` builds and runs identically to a tree without any of this work; no cdev appears, no per-VF state, no probe-path branches.

### Layer 0 prereq B -- per-VF tracked mode at probe time

The existing `MLX5_VFMIG_IOC_ENABLE_MIGRATABLE` ioctl sets the firmware's "migratable" capability bit on a VF before probe. That is *necessary but not sufficient*: it tells FW that migration commands are allowed, but says nothing about whether mlx5_core's host-side allocators should use our IOVA layer or the stock DMA layer.

We introduce a separate per-VF flag, `vfs_ctx[vf_id].vfmig_tracked`, settable via a new ioctl:

```c
struct mlx5_vfmig_set_tracked {
    __u32 vf_id;     /* in  */
    __u32 enable;    /* in  -- 0 = detach domain + clear flag; 1 = attach domain + set flag */
    __u32 flags;     /* in  -- reserved, must be 0 */
    __u32 reserved;
};
#define MLX5_VFMIG_IOC_SET_TRACKED \
    _IOWR(MLX5_VFMIG_IOC_MAGIC, 0x06, struct mlx5_vfmig_set_tracked)
```

Semantics:

- **Must be called before the VF binds to mlx5_core.** Returns `-EBUSY` if a driver is currently bound to the VF.
- On `enable=1`: allocates the per-VF IOMMU domain (if not already present), attaches it to the VF's `pci_dev`, sets `vfs_ctx[vf_id].vfmig_tracked = true`. Persists until either `enable=0` or PF unload.
- On `enable=0`: detaches and frees the domain. Returns `-EBUSY` if the VF is currently bound (caller must unbind first).
- The existing `ENABLE_MIGRATABLE` ioctl is unchanged. The two flags are orthogonal in the kernel; userspace will typically call both.

Probe-side consumers:

```c
static inline bool mlx5_vf_is_vfmig_tracked(struct mlx5_core_dev *dev) {
    /* Looks up vfs_ctx on the parent PF; safe to call from VF probe context. */
    ...
}
```

`mlx5_cmd_enable`, `pages.c`, and (later) `eq.c` / `uar.c` test this and route accordingly. When false, behavior is bit-identical to today.

This decoupling means we can ship Layer 0 + Layer 1 with confidence that non-tracked VFs are unaffected (a strong argument for landing changes incrementally without disrupting the production hot path).

### Why we need our own domain (not the kernel default)

1. **Address determinism.** `dma_alloc_coherent`/`dma_map_*` under the default `iommu_dma` allocator return arbitrary IOVAs from a global pool. We need to *request* a specific IOVA. That requires an `IOMMU_DOMAIN_UNMANAGED` domain and the `iommu_map(domain, iova_we_pick, page, len)` API.
2. **Lifetime control.** When mlx5_core unbinds, the kernel-managed default domain is torn down and IOVAs released. We need recorded IOVAs to persist (or be recreated identically) across the bind boundary on the destination.
3. **Bookkeeping discipline.** A clean slate domain we fully own means every entry is one we placed; no surprises from page tables already populated by the DMA layer.
4. **No cross-VF synchronization required.** Each VF is in its own IOMMU group (validated on the dev host: VFs `08:00.2` and `08:00.3` show up as IOMMU groups 18 and 19). One mutex per VF in our state suffices.

### Files / APIs

New file: `drivers/net/ethernet/mellanox/mlx5/core/vfmig_iova.c` and `vfmig_iova.h`.

```c
struct vfmig_iova_domain;            /* opaque */

struct vfmig_dma_page {
    u64        iova;
    u32        len;
    struct page *page;
    void       *vaddr;               /* kmap if non-coherent */
    /* kept on a sorted linked list keyed by iova */
};

int  vfmig_iova_attach(struct pci_dev *pdev, struct vfmig_iova_domain **out);
void vfmig_iova_detach(struct vfmig_iova_domain *d);

int  vfmig_iova_alloc_coherent(struct vfmig_iova_domain *d,
                               size_t size,
                               dma_addr_t *iova_out,
                               void **vaddr_out);
int  vfmig_iova_map_existing(struct vfmig_iova_domain *d,
                             dma_addr_t iova, struct page *p, size_t len);
int  vfmig_iova_replay(struct vfmig_iova_domain *d,
                       const struct vfmig_dma_page *pages, unsigned int n);
void vfmig_iova_for_each(struct vfmig_iova_domain *d,
                         void (*fn)(const struct vfmig_dma_page *, void *),
                         void *cookie);
```

### Allocator design

Bump allocator over a per-domain reserved IOVA range (e.g. `0x1_0000_0000_0000` upward; well above any address the kernel default allocator would hand out, well below the IOMMU's max). Allocations are 4 KB-aligned. Each entry recorded in the registry keyed by IOVA. Free is allowed (decrements a refcount; range is not reclaimed -- a long migration-capable VF lifetime can leak fragmentation, acceptable for v1).

### Sanity check

In Layer 0 we do *not* hook mlx5_core. We add a self-test (loadable from the test scaffold) that:

1. Attaches a domain to a VF.
2. Allocates a page at IOVA X, writes a marker.
3. Writes the IOVA into a scratch FW command (e.g. SET_DRIVER_VERSION DMA -- TBD which is safe) and confirms FW reaches the page.
4. Detaches.

If the VF is still operable after attach/detach, Layer 0 is complete.

### Settled: domain lifetime is decoupled from VF bind state

Domain is owned by `mlx5_vfmig_pf` state, keyed by `vf_id`. It is created lazily on the first `MLX5_VFMIG_IOC_SET_TRACKED { enable=1 }` for that VF, and destroyed *only* on:

1. Explicit `MLX5_VFMIG_IOC_SET_TRACKED { enable=0 }` (caller must have unbound the VF first; `-EBUSY` otherwise).
2. PF unload / `mlx5_vfmig_pf_cleanup`.
3. SR-IOV teardown (`sriov_numvfs=0`) -- the VFs themselves disappear; their domains go with them.

Critically, **domain survives a VF unbind/rebind cycle** and **survives a SAVE -> teardown -> recreate -> LOAD round-trip on the same host** (the modal use case in the next phase of testing, including HW-failure recovery on a single host). Recorded IOVAs and the page registry persist across these transitions. This avoids repeated `iommu_domain_alloc()` overhead -- which is non-trivial on systems with many migratable VFs -- and avoids the "lose the IOVAs we'd like to replay" failure of option (a) in the original options list.

The trade-off: we hold the domain (and its page-table memory) across the unbound interval. For typical SR-IOV deployments with O(VFs) total migratable VFs and bounded MANAGE_PAGES sizes per VF, this is well-bounded host RAM (low MB per VF). Acceptable.

## Layer 1: cmd ring round-trip

### Hook

`drivers/net/ethernet/mellanox/mlx5/core/cmd.c`, function `mlx5_cmd_enable` (and the corresponding teardown in `mlx5_cmd_disable`). Today:

```c
cmd->cmd_alloc_buf = dma_alloc_coherent(dev->device, ...);
```

becomes (sketch):

```c
if (mlx5_vfmig_vf_is_migratable(dev)) {
    err = vfmig_iova_alloc_coherent(domain, MLX5_ADAPTER_PAGE_SIZE,
                                    &cmd->dma, (void **)&cmd->cmd_alloc_buf);
    cmd->cmd_dma = cmd->dma; /* iseg writes this */
} else {
    cmd->cmd_alloc_buf = dma_alloc_coherent(...);
}
```

The iseg writes (`cmdq_addr_h/l_sz`) are unchanged -- they take whatever `cmd->dma` is.

### SAVE side

Extend the SAVE blob wire format with a new tag:

```c
#define VFMIG_WIRE_TAG_HOST_PAGE  0x4842 /* 'HB' */

struct vfmig_wire_host_page {
    __le64 iova;
    __le32 len;
    __le32 flags; /* reserved */
    /* followed by len bytes of page contents */
} __packed;
```

`vfmig_save_read` emits, before the existing `FW_DATA` record, one `HOST_PAGE` record per entry in the per-VF page registry. For Layer 1 the registry has exactly one entry: the cmd ring.

### LOAD side

`vfmig_load_write` parses `HOST_PAGE` records. For each, allocate a page, `vfmig_iova_map_existing(domain, iova, page, len)`, copy contents in. On `vfmig_load_run_load`, before issuing `LOAD_VHCA_STATE`, the page registry on the destination is fully populated.

When the destination's `mlx5_cmd_enable` runs, instead of `vfmig_iova_alloc_coherent` allocating fresh, it asks "is this IOVA already mapped in our domain?" -- yes, the saved cmd ring -- so it reuses that page directly.

### Pass criterion

Re-run `tools/testing/mlx5_vfmig/test_m2r.sh`. Expected dmesg change:

- Today (baseline): post-LOAD bind hangs with `cmd timeout: QUERY_HCA_CAP(0x100)` and eventually `DISABLE_HCA(0x105) timeout`.
- With Layer 1: `QUERY_HCA_CAP(0x100)` *returns success*. We then fail at *some other* command that references a not-yet-restored IOVA -- exactly the result that justifies Layer 2.

**This is the keystone experiment.** A pass here validates the entire IOMMU-based approach in a single, unambiguous data point.

### Falsification cases for Layer 1

If post-LOAD `QUERY_HCA_CAP` still times out:
- (F1) Cmd ring IOVA preservation alone is insufficient -- FW also reads other source-side state during command processing. → Inspect FW behavior with `mlxcmd` traces or request FW team input.
- (F2) Some non-cmd-ring path on the destination (e.g. iseg register writes) is incompatible with restoring an in-progress FW state. → Investigate iseg/PCI BAR semantics around LOAD_VHCA_STATE.
- (F3) Our IOMMU domain is not actually being used for the cmd ring (some path bypasses it). → Verify with `iommu_iova_to_phys` and `dma_debug`.

If F1 holds, the entire approach is in question and we stop. If F2 or F3 hold, fix and re-test.

## Layer 2: all MANAGE_PAGES pages

### Hook

`drivers/net/ethernet/mellanox/mlx5/core/pages.c`. Specifically `give_pages` / `mlx5_satisfy_startup_pages`. For migratable VFs, route page allocation through `vfmig_iova_alloc_coherent`, registry-track each.

### Probe-time skip

`mlx5_function_enable` already skips `mlx5_satisfy_startup_pages(dev, 1/0)` on restored VFs. What it does *not* yet skip is the FW-driven `MANAGE_PAGES_OP_GIVE` path -- the FW can ask for pages mid-probe via async events, and the destination would currently hand it new pages alongside the saved ones. We add a guard: while a load image is staged on the VF, the pagealloc worker queues GIVE requests but does not service them; once `apply_pending_load` completes, normal servicing resumes.

### Pass criterion

`mlx5_function_open` completes on the destination. We will then fail at `mlx5_load`, specifically at `mlx5_eq_table_create`, which justifies Layer 3.

## Layer 3: EQs + UARs (rehydrate, do not recreate)

### Approach

For restored VFs:
- `mlx5_eq_table_create` becomes `vfmig_eq_table_rehydrate` -- populates `struct mlx5_eq_table` from saved metadata, allocates kernel-side wrappers around the saved EQ buffers, *does not* call `CREATE_EQ`.
- Same shape for UARs and bfregs.

The complexity is that EQNs, UAR indices, MSI-X vector mappings, and IRQ handlers all need to agree with the saved blob's view of the world. This is where we'll discover the next set of layering problems.

### Pass criterion

`mlx5_load` returns 0; mlx5_ib attaches; `/sys/class/net/<vfX>` exists; `ip link set <vfX> up`; ping a peer.

## Cross-host milestone

After Layer 3, take the blob produced on host A and restore it on host B. We expect failures from things we haven't been thinking about on a single host: FW microversion differences, BAR layout differences, MSI-X vector counts. This is the trigger to provision a sister host.

## Layer 4: user resources + CRIU

Out of scope for now. Tracked for visibility.

## Parallel track: rxe / soft-RoCE

The user is exercising rxe in parallel to develop the userspace + CRIU plugin side without waiting on the mlx5 hardware-state preservation work. Decoupled. Useful because:

- ibverbs surface and CRIU plugin shape can be designed and tested without mlx5 hardware.
- Issues we hit on rxe (e.g. QP re-establishment with peers, lkey/rkey forwarding) will hit mlx5 too.
- Work that ports cleanly back to mlx5 is value already realized when we get there.

This plan does not gate or depend on the rxe track.

## Acceptance criteria for this plan as a whole

This plan is *complete* when, on a single host:

1. The Layer 1 keystone experiment passes (post-LOAD `QUERY_HCA_CAP` succeeds).
2. The Layer 2 milestone passes (`mlx5_function_open` completes on the destination).
3. The Layer 3 milestone passes (`mlx5_load` completes, netdev usable).
4. The cross-host milestone passes (Layer 3 functionality across two hosts).

Layer 4 is tracked separately and will get its own plan when we get there.

## Immediate scope

Only Layer 0 + Layer 1 are committed. We will not start Layer 2 until Layer 1 has passed and we have re-evaluated whether the original IOVA-preservation premise actually moves the destination forward.

## Open questions / risks

- **(Q0.1)** ~~Domain lifetime across unbind/rebind~~ -- settled: domain owned by `mlx5_vfmig_pf`, persists across VF bind/unbind, destroyed only by explicit `SET_TRACKED { enable=0 }` or PF/SR-IOV teardown. See Layer 0 discussion.
- **(Q0.2)** Does the existing `MLX5_VFMIG_IOC_ENABLE_MIGRATABLE` need to be a strict prerequisite for `SET_TRACKED`, or are they fully orthogonal? Tentative: orthogonal, but error out at `apply_pending_load` time if the VF is tracked but not migratable (no useful FW-side migration possible). Confirm during implementation.
- **(Q0.3)** Should `SET_TRACKED { enable=0 }` while there is a staged-but-unapplied LOAD blob be allowed? Tentative: no, return `-EBUSY` -- the staged blob references IOVAs in this domain.
- **(Q1.1)** Does FW expect the cmd ring IOVA to be already mapped *before* iseg writes, or does it lazily resolve on first command? Should not matter (we'll have the IOVA mapped before the destination's mlx5_cmd_enable runs), but worth confirming with `dma_debug`.
- **(Q1.2)** Page contents -- are there bits FW writes to the cmd ring page that we need to capture (status bytes, doorbell tail), or does FW initialize from scratch? Inspect `cmd_alloc_buf` layout in `cmd.c`.
- **(Q2.1)** Some MANAGE_PAGES pages may be huge-page backed in production. Layer 0 allocator is 4 KB only for v1; flag if real workloads hit this.
- **(Q3.1)** UAR pages are mmaped to userspace (uverbs). Migrating them with userspace mappings preserved is a separate problem; for Layer 3 we focus on kernel UARs.
- **(R1)** Layer 1 may pass and Layer 2 may still expose that there's *more* than IOVA in the FW's source-side dependency. If so, Layer 2 effectively becomes a research deep-dive rather than an engineering task. Acceptable cost; we'll know then.

## Effort framing (deliberately rough)

Only Layer 0 + Layer 1 are committed. Estimate ~1.5 weeks for both together, capped at 2 weeks before re-plan. Layers 2+ are not estimated until prior layers pass.

## File touch summary (Layer 0 prereqs + Layer 0 + Layer 1)

- `drivers/net/ethernet/mellanox/mlx5/core/Kconfig` -- add `CONFIG_MLX5_VFMIG`.
- `drivers/net/ethernet/mellanox/mlx5/core/Makefile` -- gate `vfmig.o` and add `vfmig_iova.o` behind the new symbol.
- `drivers/net/ethernet/mellanox/mlx5/core/vfmig_iova.{c,h}` -- new.
- `drivers/net/ethernet/mellanox/mlx5/core/vfmig.c` -- new `SET_TRACKED` ioctl handler; emit/consume `VFMIG_WIRE_TAG_HOST_PAGE` records; replay before LOAD; per-VF domain handle on PF state.
- `drivers/net/ethernet/mellanox/mlx5/core/vfmig.h` -- per-VF domain pointer + `vfmig_tracked` flag on `mlx5_vfmig_pf::vfs_ctx[]`; helper `mlx5_vf_is_vfmig_tracked()`.
- `drivers/net/ethernet/mellanox/mlx5/core/cmd.c` -- consult `mlx5_vf_is_vfmig_tracked()` in `mlx5_cmd_enable` / `mlx5_cmd_disable`; route to `vfmig_iova_alloc_coherent` when tracked.
- `drivers/net/ethernet/mellanox/mlx5/core/main.c`, `dev.c` -- guard vfmig cdev / probe-path branches with `IS_ENABLED(CONFIG_MLX5_VFMIG)` (compiles out cleanly).
- `include/uapi/linux/mlx5_vfmig.h` -- new `MLX5_VFMIG_IOC_SET_TRACKED` ioctl + struct; the wire format extension is kernel-internal.
- `tools/testing/mlx5_vfmig/mlx5_vfmig.c` -- new `set_tracked <vf_id> <0|1>` verb.
- `tools/testing/mlx5_vfmig/test_m2r.sh` -- call `set_tracked 1` before bind on source, before recreate on destination; update expected dmesg output.
- `tools/testing/mlx5_vfmig/README.md` -- update architectural section to reflect the IOMMU-based approach and the two-flag (migratable + tracked) model.
