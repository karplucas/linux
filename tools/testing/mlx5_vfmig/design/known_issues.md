# mlx5_vfmig — known issues

> **Status (2026-05-21):** triage document for cross-host SAVE/LOAD
> issues that survive the current `mlx5_vfmig` + `vfmig_iova` +
> `mlx5_ib_restore_*` stack. Active issues in §1; pre-existing
> known limitations / out-of-scope items in §2; resolved issues
> with commit references in §A.

Scope: only issues that affect the cross-host SAVE on one host →
LOAD on a different host path (the `vfmig_driver_override_and_bind`
flow in the CRIU plugin). Single-host round-trip (same kernel,
same PF) is regression-tested by
`tools/testing/mlx5_vfmig/save_load/test_iova_tracked_save_load.sh`
and considered green; entries here describe deltas from that
baseline.

This doc is intentionally narrow. Per-stage open work is tracked
in the respective design doc (`user_mr_dma.md` §A, `uar_restore.md`
§10, `uobject_restore.md` §13). Items land here when they're
either (a) production-blocking and not yet under design-doc
coverage, or (b) cross-cutting across multiple design docs.

## 1. Active issues

### 1.1 Slot-0 cmd hang on first post-LOAD command  *(active investigation, 2026-05-21)*

**Symptom.** On a destination VF that has just been bound to
`mlx5_core` after a successful `LOAD_VHCA_STATE`, the first
firmware command issued on `cmd[0]` after probe completes the
caps/IO sequence hangs for the full `MLX5_CMD_TIMEOUT_MSEC`
window (60s) and then `wait_func` reports `No done completion`.
The next cmd allocated to a different slot succeeds; subsequent
cmds (including reuse of slot 0) succeed without delay.

Observed commands hitting the hang in end-to-end CRIU restore
runs:

| Command | Caller | Effect of the 60s hang |
|---|---|---|
| `CREATE_MKEY` (0x200) | `mlx5e_create_mdev_resources` | `_mlx5e_resume` fails -110, no netdev created (orchestrator step `discover_restored_vf_host*` times out at 30s) |
| `QUERY_NIC_VPORT_CONTEXT` (0x754) | `mlx5_ib` port bring-up | mlx5_ib gracefully continues; mlx5e probe still succeeds afterward |

A pre-`SAVE` source-side `sleep 60` between the application's
last RDMA verb and CRIU checkpoint **eliminates** the
`CREATE_MKEY` hang and leaves only the (non-fatal)
`QUERY_NIC_VPORT_CONTEXT` hang. Source-side dmesg also shows
a `mlx5_cmd_comp_handler: Command completion arrived after
timeout (entry idx = 0)` line ~28 ms after `LOAD_VHCA_STATE`
applies, which corresponds to the duplicate-completion path
in `cmd.c` (line 1817).

**Mechanism (validated end-to-end, 2026-05-21).** The bitmask
drain + dest-side cmd-ring scrub landed by commit `3f354a919947`
were not sufficient on their own. Sequence observed on the
destination in run_2 *before* the barrier landed:

1. `LOAD_VHCA_STATE` applies at T.
2. VF probe starts; first cmd lands on slot 0.
3. ~28 ms after T, `mlx5_cmd_comp_handler` fires the "Command
   completion arrived after timeout (entry idx = 0)" warning.
   The only way for `PENDING_COMP` to be already-cleared in 28
   ms is for `comp_handler` to have been invoked **twice** for
   slot 0 -- once for the dest's own cmd (legit) and once for
   a stale completion FW carried across `LOAD_VHCA_STATE`.
4. Probe continues for ~65 s doing other cmds (which work).
5. `CREATE_MKEY` from `mlx5e_create_mdev_resources` lands on
   slot 0 and hangs the full 60 s `wait_func` window.

Root cause: **FW carries per-VHCA "owed completion" state
across SAVE/LOAD**. `SAVE_VHCA_STATE` captures the source VHCA
before FW has finished delivering every completion EQE it owes
the driver. `LOAD` restores that state on the destination. The
dest's first cmd on slot 0 races against FW's queued stale
completion. The cmd-ring buffer staleness was a red herring --
the dest-side scrub doesn't help (and the EQ buffer is already
zeroed by `mlx5_dma_zalloc_coherent_node`).

**Fix (landed).** `vfmig_save_drain_vf_cmd_iface` now has two
layers, in order:

1. **Bitmask drain.** Wait for the source VF mdev's
   `cmd->vars.bitmask` all-bits-set. Driver-visible invariant
   only. Catches the easier case where the workload has a cmd
   genuinely in flight at SAVE.

2. **FW-flush barrier.** After the bitmask drain, issue a
   **single** `QUERY_ISSI` cmd through the source VF mdev. The
   barrier lands on the lowest-free slot (slot 0 after a clean
   drain); if FW has an internally-queued completion it still
   owes on that slot, FW emits the stale EQE before processing
   the barrier. The driver consumes both EQEs back-to-back; the
   stale one fires the "Command completion arrived after
   timeout" warning on the **source** host (harmless -- the
   source process is being checkpointed anyway), and FW's
   owed-completion queue is empty by the time `SUSPEND_VHCA`
   snapshots it.

A single barrier is empirically sufficient on FW 28.48.1000.
Multi-cmd barrier rounds and explicit settle delays were tried
during diagnosis (commit `50e22b8ee071`, superseded) but not
needed -- one round-trip kicks FW past whatever post-cmd
internal work was lagging.

The dest-side cmd-ring scrub in `alloc_cmd_page` (commit
`3f354a919947`) stays as defense-in-depth: it doesn't address
this symptom but matches the EQ-buf hygiene the rest of the
codebase already provides, and it covers a hypothetical future
"source had a cmd descriptor mid-write" scenario the barrier
doesn't.

**Validation matrix (kernel commit at runtime).**

| barriers | settle ms | CREATE_MKEY | Outcome |
|---|---|---|---|
| 0 | 0 | hangs 60s | swap_after_mr fails |
| 0 | 60000 | resolved | (earlier session) |
| 0 | 120000 | resolved | run_2, also resolves §1.2 |
| **1** | **0** | **resolved** | **swap_after_mr passes (current default)** |
| 4 | 0 | resolved | overcompleted; same outcome as N=1 |

**Tracking.** Diagnostic dmesg captures in
`scratch/run_2/dmesg_*.txt` (pre-fix). Validation captures
post-fix in this session's transcript. Final landed shape in
commits `3f354a919947` (drain skeleton + cmd-ring scrub),
`50e22b8ee071` (barrier exploration -- superseded), and the
followup that strips the params back to the single-cmd shape
under `vfmig_save_drain_vf_cmd_iface`.

### 1.2 `SET_ROCE_ADDRESS` rejected on the destination post-LOAD  *(new, 2026-05-21)*

**Symptom.** Every `SET_ROCE_ADDRESS(0x761)` issued by the
destination's `ib_cache_gid_add` path fails with status
`bad parameter` (0x3), syndrome `0x63c66`, returning `-EINVAL`.
The destination's GID table ends up empty of entries matching
its own netdev IPs:

```
mlx5_core 0000:08:00.2: SET_ROCE_ADDRESS(0x761) op_mod(0x0)
    failed, status bad parameter(0x3), syndrome (0x63c66),
    err(-22)
infiniband mlx5_2: add_roce_gid GID add failed port=1 index=0
__ib_cache_gid_add: unable to add gid fe80:...c9b9:16ff:feff:1800
    error=-22
```

The orchestrator's `create_mlx5_vf_dev_host2_after_restore`
step then fails because no GID matches the dest VF's
`192.168.100.4/24`. RDMA verbs that resolve a GID by IP
(`ibv_query_gid_table`, `rdma_resolve_addr`, `rdma_bind_addr`)
all fail.

**Mechanism.** `SAVE_VHCA_STATE` is designed for the VFIO
passthrough model where source and destination netdev personas
are identical (the same guest VM is being live-migrated).
The VHCA blob includes the ROCE_ADDRESS table state, and
`LOAD_VHCA_STATE` re-applies it on the destination. In our
native (non-VM) deployment the destination's netdev has a
*different* MAC, different link-local GID, different IP, so
the table state restored from the blob describes
*the source's* identity. When the destination's
`mlx5_en` netdev brings up and the IB core's GID cache walks
the netdev's IPs and calls `add_roce_gid`, FW sees a request
to write GID indices that are already populated with the
source's entries and rejects with a "bad parameter" against
the not-cleared slot.

**Impact.** Post-restore destination has functional RDMA verbs
(PD/MR/CQ/QP `RESTORE_*` flows complete) for the verbs that
operate on adopted FW resources, but cannot establish any
**new** RDMA connection that needs a GID lookup against dest's
own IP. Workloads that re-bind to local IPs after restore are
broken; workloads that only use already-restored QPs are
unaffected.

**Status (2026-05-21).** Resolved as a side-effect of the §1.1
barrier-cmd fix. Follow-up validation with `barriers=1,
settle=0` showed `SET_ROCE_ADDRESS` succeeds on the destination
and the destination's GID table populates correctly. This was
unexpected: the GID table is conceptually independent of
cmd-iface owed completions, and the only obvious link is that
both symptoms required FW to "settle" before the destination
could safely drive the cmd interface. The single `QUERY_ISSI`
round-trip on the source apparently flushes whatever
ROCE-table-related FW state was also lagging.

Marking this active rather than resolved (kept in §1) because
the mechanism connecting `QUERY_ISSI` to `SET_ROCE_ADDRESS`
isn't understood. If a production run re-surfaces the symptom
with the §1.1 fix in place, the architectural options below
remain available.

**Architectural options (held in reserve).** Two ways the
kernel could explicitly handle this if it reappears:

1. **Strip ROCE_ADDRESS state from SAVE.** Treat the GID table
   like a per-host personality (analogous to MAC address): not
   part of the migrated VHCA state. The destination's
   `add_roce_gid` then writes into a fresh table without
   conflict. Wire-incompatible with the VFIO passthrough model
   (which *does* want GIDs preserved), so this needs to be
   gated on a SAVE-side flag controlled by the
   `mlx5_vfmig` PF cdev (the user owns the policy choice).

2. **Clear ROCE_ADDRESS on the destination post-LOAD.**
   `mlx5_function_open()`'s restored-VF branch (after
   `LOAD_VHCA_STATE`, before `mlx5_ib` bring-up) issues a sweep
   of `DELETE_ROCE_ADDRESS` (or `SET_ROCE_ADDRESS` with a
   sentinel "clear" payload) across all port-1 GID indices.
   Keeps the SAVE blob format unchanged but adds a destination-
   side scrubber. Cost is one cmd per GID index per port at
   bind time.

Option 2 is the smaller landing and doesn't change the on-wire
format; option 1 is cleaner architecturally. The choice should
be revisited together with the broader "what state really should
cross hosts" review (§1.4).

**Tracking.** Diagnostic dmesg captures in
`scratch/dmesg_host_*.txt` lines around
`add_roce_gid GID add failed`.

### 1.3 DEVX adoption blind spot  *(documented elsewhere, surfaced for triage)*

**Symptom.** Opening a destination ucontext with
`MLX5_IB_ALLOC_UCTX_DEVX` or
`MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID` after a cross-host LOAD
gives a ucontext whose `devx_uid` is either fresh (not the
source's) or unusable for adopting source-allocated FW
resources. `CREATE_MKEY(pdn, uid=source_devx_uid)` and similar
`(pdn, uid)` calls reject with syndrome `0x76555f`.

**Mechanism.** `LOAD_VHCA_STATE` preserves the FW's
`next_free_uctx` counter (so freshly-allocated uids on dest
land past the source's high-water mark) but does **NOT**
preserve the per-uid uctx-registration table
(`uid -> uctx_attrs` map). All low-range uids are unregistered
post-LOAD, so any FW operation that gates on uid-registration
fails. The high `0xfff_*` host-privileged range is a separate
fallback bucket that bypasses the registry — it accepts the
operation but isn't a usable mitigation since it routes around
`(pdn, uid)` ownership entirely.

Full empirical matrix in `uobject_restore.md` §6.2.

**Impact.** Restored processes cannot use DEVX paths
(`mlx5dv_devx_obj_create`, `mlx5dv_devx_qp_create`,
DEVX-rooted UAR alloc, …). Standard verbs (PD/MR/CQ/QP/SRQ for
RC/UD/UC) work via the `uid=0` host-privileged lane.

**Workaround.** v0 CRIU plugin opens the destination ucontext
**without** `MLX5_IB_ALLOC_UCTX_DEVX`, leaving
`context->devx_uid = 0`. All adopted resources land in the
`uid=0` lane (ungated). Forward-compat: kernel keeps the
`MLX5_IB_ALLOC_UCTX_ADOPT_DEVX_UID` plumbing in tree but
documents the field as `VESTIGIAL FOR v0 -- DO NOT SET FROM
USERSPACE` (`include/uapi/rdma/mlx5-abi.h`). The
`mlx5_ib_restore_pd_fw_probe` check stays as defense-in-depth.

**Fix plan.** Requires future FW work — see
`uobject_restore.md` §6.2 "Future-FW options" (LOAD_VHCA_STATE
preserves uctx registry, or per-resource `MODIFY_*_UID` family,
or source-side `uid=0` negotiation). No in-tree work planned for
v0.

**Tracking.** `uobject_restore.md` §6.2 + §A.B.

### 1.4 What VHCA state should actually cross hosts  *(meta, scoping)*

§1.2 and §1.3 are instances of a broader question: the
`SAVE_VHCA_STATE` / `LOAD_VHCA_STATE` pair was originally
designed for guest-VM live migration, where the destination
inherits the source's complete network identity. In our
native (CRIU-mediated) deployment, source and destination
are separate hosts with separate identities, and some VHCA
state should not transfer:

- **ROCE_ADDRESS table** (§1.2) — different netdev IPs.
- **MAC address** — different netdev MAC; relevant if the
  CRIU plugin ever needs the dest's mlx5_en netdev to use
  the dest host's MAC rather than source's.
- **Hardware counters** — observed-bytes/packets numbers carry
  forward across migration and confuse monitoring; not blocking
  but worth noting.
- **uctx registry** (§1.3) — separate user-process identities
  on the two hosts.

These need a per-category SAVE-side policy: which slice of
VHCA state migrates and which is dest-rebuilt. The PF cdev's
`MLX5_VFMIG_IOC_SAVE_VHCA_STATE` is the natural place to
expose this as a flags bitmap once we have more than one
"strip from SAVE" case in flight.

**Status.** Not actively in design. Surface here so that the
fixes for §1.2 / §1.3 can converge on a shared mechanism if
that turns out to be the right shape.

## 2. Known limitations (pre-existing, in scope for v0)

These are documented in the respective design docs and are
not new issues — listed here for one-stop triage.

| Limitation | Doc | Status |
|---|---|---|
| Homogeneous-fleet only (same-CPU/IOMMU class for WC, `lib_uar_4k`/`dyn`, `cqe_version`) | `uar_restore.md` §8 | v0 constraint; cross-class is M3+ |
| DM (on-chip device memory) MRs bypass host DMA | `user_mr_dma.md` §2 "Out of scope" | not in v0 critical path |
| DMA-buf user MRs (`reg_user_mr_dmabuf`) | `user_mr_dma.md` §2 | deferred — sg-granularity stability across LOAD is a known unknown |
| ODP (on-demand paging) MRs | `user_mr_dma.md` §2 | uses `hmm_dma_map_pfn` not `dma_map_sgtable`; separate hook needed |
| `IB_UCONTEXT_RESTORE_MODE` generic flag | `uobject_restore.md` §A.B | mlx5-specific flag in tree today; generic IB-core flag is a future cleanup |
| `ibv_create_srq` EINVAL on restored VFs (user-mode SRQ creation post-restore) | TODO list `investigate_restored_vf_srq_einval` | symptom only; root cause not yet investigated |

## A. Appendix: resolved issues

Resolved entries stay here permanently as a triage breadcrumb:
"if you're hitting the symptom and it matches this entry, the
fix landed in commit X — verify your build includes it."

### A.1 Multi-page user MR replay → `-EEXIST` collision in secondary index  *(fixed 2026-05-21)*

**Was symptomatic of.** End-to-end CRIU restore tests using
non-page-aligned user MRs (any `umem_offset + umem_length >
PAGE_SIZE`) failed with `RESTORE_MR ... failed: -2`. SAVE
emitted zero `HOST_USER_PAGE` records for the affected MR;
LOAD's `vfmig_iova_install_external_placeholder` succeeded for
the first page of the MR's umem and rejected with `-EEXIST` on
every subsequent page. Source-side dmesg showed
`vfmig_iova: vf %u secondary index already contains entry for
key 0x%llx`. The MR was silently marked "not CRIU-restorable"
on the source.

**Root cause.** `vfmig_iova_user_index` (the secondary `(kind,
fw_id) -> vfmig_iova_page` rb-tree) used `instance_key` as the
unique sort key. `vfmig_iova_retag_external_range` walks every
page in `[iova_base, iova_base + length)` and stamps each with
the same `new_instance_key`, then inserts each into the
secondary index. The first insert succeeded; subsequent inserts
for the same `instance_key` rejected. Affected all four retag
callsites — MR, CQ, QP, SRQ — for any multi-page user uobject.

**Fix shape.** Composite-key indexing: the rb-tree sort key
becomes `(instance_key, iova)`. `vfmig_iova_user_index_lookup`
finds the leftmost (lowest-iova) match for a given
`instance_key` and serves as the head of a sibling chain.
`vfmig_iova_bind_user_object` walks the chain, validates
contiguity, sums lengths, maps destination SGs sequentially
across the source IOVA range, and marks every sibling
`awaiting_bind=false` atomically. New helper
`vfmig_iova_user_index_next_sibling_locked` traverses siblings
sharing an `instance_key`.

**Validation.** `test_user_object_replay.sh
NUM_UNALIGNED_MRS=2` exercises 4 aligned + 2 non-page-aligned
MRs (each spanning 2 pages at offset 0x800 in a 2-page buffer).
Verdict block reports an `[expected_mr_min..expected_mr_max]`
band because contiguous siblings collapse into a single
multi-page registry entry; `obs_mr` in `[6..8]` is the band
top-typical case. Re-run with `NUM_UNALIGNED_MRS=0` is exact-
match `obs_mr=4`. End-to-end CRIU
`rdma_test_agent_vfmig_criu_swap_after_mr` showed both hosts
reach `RESTORE_MR ... ok` for the previously-failing MR
(post-fix dmesg in `scratch/swap_after_mr_2.txt` and
`scratch/dmesg_host_*.txt`).

**Commits.**

- `e87824286e62` (kernel): `mlx5_vfmig: support multi-page
  user objects in the secondary index`
- `7f8d3be04846` (tools): `tools/mlx5_vfmig: regression
  coverage for multi-page user MRs`

**See also.** `user_mr_dma.md` §6 (stage-2 identity
infrastructure) for the original design that this fix
extends.
