# Upstream submission inventory (criu-dev-poc → mainline)

Oracle branch: **`criu-dev-poc` @ `0da0f097`** — **266 commits** over the true
base `8fb0d17` ("Add -raphael-criu version", a local version stamp on the
`archive/f-save-restore-fixup-allocator` mainline tag). Note: `f7e71a81b2ad` is
*not* the fork point; the first 5 commits (`8fb0d17..f7e71a81`) are
CRIU-enabling prerequisites (see the `P` group in section 6 / Group P in
section 2). **Curation starts from the real dev branch at `f7e71a81`** (branch
`rebase-P-0` = `origin/criu-dev-build-up-rebase` = `origin/6.19-dev-criu`),
keeping the 5 P commits as-is; new functional patches (Group A onward) are built
on top. Underneath: `f7e71a81` sits on `8fb0d17` (a `-raphael-criu` `Makefile`
version stamp) which sits on the real mainline fork point `05f7e89` (`Linux
6.19`). The `umr.c` diagnostic + `mr.c` gate cleanup are parked on a separate
branch and are **out of scope** for this inventory.

## 0. Approach (agreed)

- **One cross-subsystem RFC first**, posted against recent mainline, CC
  `linux-rdma` + `netdev`, for architectural feedback. Then follow-on,
  polished, per-tree series.
- **Curate from the oracle**, do not replay 261 commits: partition the final
  `base..criu-dev-poc` diff (validated, suite-passing) into ordered,
  compilable, reviewable patches. Every hunk taken is the tip (working)
  version — no broken intermediates.
- **Drop `tools/testing/criu_rdma` (70 files)** before upstream; keep it on the
  curation branches as the diff/behaviour oracle.
- **Two development tracks** (not three): T1 = core-uverbs + RXE (interleaved);
  T2 = the whole mlx5 side (mlx5_core vfmig + mlx5_ib verbs, interleaved).
  T2 spans two trees but is one functional unit — proposed together, split
  only as a submission logistic.
- **Development order ≠ submission order.** Sections 2-3 describe the *submission*
  layering; the build-up branches are actually built and tested in a *vertical,
  per-object* order that matches the two-agent (kernel↔criu) workflow. See §8 and
  the criu-side companion (`criu.git:test/rdma/plans/criu_build_up_plan.md`).

## 1. What ships vs. what's dropped

Ships (~50 source files):

| Area | Files | Role |
|------|-------|------|
| `drivers/infiniband/core` | 12 | generic uverbs RESTORE framework + QUERY/NLDEV discovery |
| `drivers/infiniband/sw/rxe` | 16 | RXE reference implementation |
| `drivers/infiniband/hw/mlx5` | 12 | mlx5_ib verbs (restore-mode, RESTORE_*/QUERY_*) |
| `drivers/net/ethernet/mellanox/mlx5/core` | 19 | mlx5_core vfmig datapath |
| `include/uapi` | 7 | ABI (rdma ioctl cmds, rxe ns, mlx5_vfmig, mlx5-abi) |
| `include/rdma` | 3 | ib_verbs / ib_umem / uverbs_types |
| `include/linux/mlx5` | 2 | driver.h / cq.h (shared → mlx5-next) |

Dropped / transformed:
- `tools/testing/criu_rdma` (70) — dev harnesses. A few may later be reworked
  into `tools/testing/selftests/rdma`, but not for v0.
- `scratch/` — internal.
- `design/*.md` — content migrates into **commit messages** and a
  `Documentation/infiniband/` doc (candidates: `uobject_restore.md`,
  `datapath_pause_resume.md`).

## 2. Track 1 — core uverbs + RXE (→ rdma-next)

ABI introduced:
- core: `UVERBS_OBJECT_RESTORE` + `RESTORE_{PD,MR,CQ,QP}`; `QUERY_MR`
  extensions (user_addr, access_flags); `ib_device_ops.ucontext_is_restore_mode`
  + `restore_{pd,mr,cq,qp}`; NLDEV discovery TLVs.
- rxe: `RXE_IB_OBJECT_MIGRATE` + `FREEZE_DATAPATH`, `FREEZE_CONTEXT`,
  `QUERY_QP`, `QUERY_CQ`.

Patch order = **read-only → quiesce → save → restore**, interleaving the core
framework patch with its RXE reference impl, on top of the CRIU-enabling
prerequisites (Group P):

**Group P — CRIU-enabling prerequisites (T1.0, `base..f7e71a81`)**
The 5 commits below `f7e71a81`. **We start curation from the real dev branch at
`f7e71a81` and keep these commits as-is** — this is the `rebase-P-0` branch
(`origin/criu-dev-build-up-rebase`), NOT a re-curation onto pristine mainline.
Group A onward is built on top of `rebase-P-0`. The 5 commits, in branch order:
- **`eb6bb8b` `P-rxe-netns`** — per-netns rxe sockets. Third-party ("original
  patch from Enfabrica"); a carried dependency to keep rxe testable, to be
  dropped once the upstream netns series lands. `rxe_net.c`.
- **`52721d0` / `551a135` / `a753315` `P-fdinfo`** — expose `ib_ucontext`
  restrack id via `show_fdinfo` on uverbs char/mmap/async/comp fds for CRIU
  fd↔context discovery. `uverbs_main.c`. (Squash into one patch at final export.)
- **`f7e71a8` `P-safe-file-access`** — delete the `ib_safe_file_access` check in
  `ib_uverbs_write` (blocks CRIU restore recreating fds). SECURITY-sensitive;
  message self-documents the tradeoff; may need a narrower restore-only
  mechanism before upstream. `uverbs_main.c`.

**Group A — discovery / query (read-only, lowest risk, mergeable first)**
- **T1.1 RDMA/core: extend QUERY_MR (user_addr, access_flags)**
  `uverbs_std_types_mr.c`, `ib_user_ioctl_cmds.h`, `verbs.c`
- **T1.2 RDMA/nldev: expose restore-relevant res handles for CRIU discovery**
  `nldev.c`, `rdma_netlink.h`, `core_priv.h`

**Group B — RXE datapath quiesce**
- **T1.3 RDMA/rxe: per-QP pause/resume + MIGRATE ns / FREEZE_DATAPATH**
  `rxe_migrate.c`(new), `rxe_user_ioctl_cmds.h`(new), `rxe_qp.c`, `rxe_loc.h`,
  `rxe_pool.c/.h`, `Makefile`
- **T1.4 RDMA/rxe: ucontext-scoped FREEZE_CONTEXT** `rxe_migrate.c`, `rxe_pool.c`
- **T1.5 RDMA/rxe: idempotent pause/resume, drop inbound + drain while frozen**
  `rxe_recv.c`, `rxe_comp.c`, `rxe_resp.c`, `rxe_qp.c`

**Group C — RXE dump-side query (save)**
- **T1.6 RDMA/rxe: QUERY_QP state blob + user_handle**
  `rxe_migrate.c`, `rxe_qp.c`, `rxe_verbs.h`, `rdma_user_rxe.h`
- **T1.7 RDMA/rxe: QUERY_CQ ring offset + cqe** `rxe_cq.c`, `rxe_migrate.c`, `rxe_mmap.c`
- **T1.8 RDMA/rxe: B1 in-flight images (SQ/RQ/res, CQE)**
  `rxe_queue.c/.h`, `rxe_resp.c`, `rxe_comp.c`

**Group D — core restore framework + RXE restore impl**
- **T1.9 RDMA/core: restore-mode ucontext predicate + restore_* device ops**
  `ib_verbs.h`, `uverbs_types.h`, `verbs.c`, `device.c`, `core_priv.h`
- **T1.10 RDMA/core: allocate uobject at caller-chosen ufile handle**
  `rdma_core.c/.h`
- **T1.11 RDMA/core: UVERBS_OBJECT_RESTORE + RESTORE_PD dispatcher (+ rxe impl)**
  `uverbs_std_types_restore.c`(new), `uverbs_uapi.c`, `Makefile`,
  `ib_user_ioctl_cmds.h`; `rxe_migrate.c`, `rxe_verbs.c`
- **T1.12 RDMA/{core,rxe}: RESTORE_MR** `uverbs_std_types_restore.c`,
  `umem.c`, `ib_umem.h`; rxe MR restore
- **T1.13 RDMA/{core,rxe}: RESTORE_CQ (+ rxe ring vm_pgoff round-trip)**
  `uverbs_std_types_restore.c`; `rxe_cq.c`, `rxe_mmap.c`, `rxe_queue.c`
- **T1.14 RDMA/{core,rxe}: RESTORE_QP drained (born-frozen + thaw-and-replay)**
  `uverbs_std_types_restore.c`; `rxe_qp.c`, `rxe_verbs.c`, `rxe_comp.c`
- **T1.15 RDMA/rxe: in-flight QP/CQ restore (B1)** — image restore, retry-arm on
  resume, responder continuity, cursor seeding, ship-only-subspan
  `rxe_qp.c`, `rxe_resp.c`, `rxe_comp.c`, `rxe_queue.c`, `rxe_srq.c`

~15 patches. SRQ (`rxe_srq.c`) may be a later micro-stage (design S6c/S7); keep
out of v0 if scoped out.

## 3. Track 2 — mlx5 side (→ net-next/mlx5-next + rdma-next)

**2a — mlx5_core datapath (net-next / mlx5-next)**
- **T2.1 net/mlx5: vfmig chardev framework + UAPI + per-PF state**  (== base
  commit `68e92ad`) `vfmig/vfmig.{c,h}`, `mlx5_vfmig.h`, `Kconfig`, `Makefile`,
  `sriov.c`, `driver.h`, `mlx5_core.h`
- **T2.2 net/mlx5: replayed-page IOVA tracking + DMA ops**
  `vfmig_iova.{c,h}`, `vfmig_dma_ops.{c,h}`, `pagealloc.c`, `alloc.c`, `wq.c`
- **T2.3 net/mlx5: SAVE_VHCA_STATE** `vfmig.c`, `cmd.c`
- **T2.4 net/mlx5: LOAD_VHCA_STATE + mark_restored + restored-VF probe**
  `vfmig.c`, `main.c`, `dev.c`, `eq.c`, `cq.c`, `mlx5_core.h`
- **T2.5 net/mlx5: split SUSPEND/RESUME out of SAVE/LOAD (snapshot ordering)** `vfmig.c`
- **T2.6 net/mlx5: directional SUSPEND/RESUME + tri-state dp_state**
  `vfmig.c`, `driver.h`, `mlx5_vfmig.h`
- **T2.7 net/mlx5: force-resume parked VFs before per-VF SR-IOV teardown**
  `sriov.c`, `vfmig.c`
- **T2.8 net/mlx5: keep fused SUSPEND/RESUME all-or-nothing on failure** `vfmig.c`
- **T2.9 net/mlx5: gate TX on restored VFs** `en_tx.c`

**2b — mlx5_ib restore-mode + verbs (rdma-next; build-deps on 2a exports + T1 core)**
- **T2.10 RDMA/mlx5: restore-mode ucontext** `vfmig_uctx.c`(new), `main.c`,
  `mlx5_ib.h`, `mlx5_user_ioctl_cmds.h`, `mlx5-abi.h`
- **T2.11 RDMA/mlx5: RESTORE_PD impl**
- **T2.12 RDMA/mlx5: RESTORE_MR adoption (FW mkey continuity)** `mr.c`, `mem.c`
- **T2.13 RDMA/mlx5: QUERY_CQ + RESTORE_CQ (FW cqn continuity, ring round-trip)**
  `cq.c`, `doorbell.c`, `cq.h`
- **T2.14 RDMA/mlx5: RESTORE_QP adoption (qpn/DBR/bfreg/ECE)** `qp.c`, `qpc.c`, `qp.h`
- **T2.15 RDMA/mlx5: SRQ restore** `srq.c`  (optional / later)

Cross-tree handling: shared `include/linux/mlx5/{driver.h,cq.h}` land via
**mlx5-next**; 2a to net-next, 2b to rdma-next with a stated dependency. All
posted together in the RFC.

## 4. Curation mechanics

1. `git switch -c upstream-t1 f7e71a81b2ad`
2. per patch: `git checkout criu-dev-poc -- <paths>` then `git add -p` to split
   hunks (drop debug/scratch); write real commit message + `Signed-off-by:
   Raphael Norwitz <rnorwitz@nvidia.com>`.
3. **compile every patch**; run the matching `criu_rdma` harness at each
   capability-introducing patch (harness stays on the oracle, invoked against
   the built modules).
4. `scripts/checkpatch.pl --strict` per patch.
5. Acceptance gate: `git diff upstream-t1 criu-dev-poc -- <T1 paths>` is empty
   (modulo intentional cleanups). Same for `upstream-t2`.
6. RFC branch = T1 + T2 stacked on a recent -rc for posting.

## 5. Open items to resolve before/while curating

- **Restore-mode entry mechanism**: RESOLVED — it is a *per-driver GET_CONTEXT
  udata opt-in*, no core UAPI addition. Core only adds the
  `ib_device_ops.ucontext_is_restore_mode` op (`c57115c`). RXE owns its own
  entry flag: `RXE_ALLOC_UCTX_RESTORE_MODE` in `rdma_user_rxe.h`
  (`struct rxe_alloc_ucontext_req`), latched into the sticky
  `rxe_ucontext.restore_mode` bit by `rxe_alloc_ucontext()` and reported by
  `rxe_ucontext_is_restore_mode()` (`f0623eb`). mlx5 has an analogous
  driver-owned entry via its `mlx5-abi.h` / restore-mode ucontext path. So the
  entry UAPI belongs to each driver, not the core `D-restmode` patch.
- **SRQ scope for v0** (in T1.15 / T2.15, or deferred to a follow-up series).
- **Design-doc destination**: which of `uobject_restore.md` /
  `datapath_pause_resume.md` become `Documentation/infiniband/` vs. cover-letter
  prose.
- **Commit-level classification**: done — see section 6 (all 261 commits mapped
  to goal + disposition + anticipated FUNCTIONAL commit).
- **vfmig↔VFIO convergence**: parked for RFC; revisit if maintainers bite.

## 6. Commit map (all 266, base `8fb0d17`, verified against the final diff)

Legend:
- **Goal** (Level-1, reuse the group spine): `P` CRIU-enabling prerequisites (core uverbs/rxe, pre-`f7e71a81`); `2a` mlx5_core vfmig datapath; `2b` mlx5_ib restore-mode + verbs; `1A` core/rxe discovery+query; `1B` rxe datapath quiesce; `1C` rxe save/query + in-flight; `1D` core restore framework + rxe restore impl. A second goal in parens = rare cross-goal commit.
- **Disp** (disposition): `U` = UPSTREAM (surviving code hunks feed a FUNCTIONAL patch); `S` = SCAFFOLD (harness/design/scratch, kept on the rebuild branch for testing, stripped at export); `Z` = NET-ZERO (reverted/superseded, contributes nothing upstream).
- **Feeds / note**: the anticipated FUNCTIONAL commit the code hunks belong to (labels below), plus provenance notes. `split` = mixed code+tools commit whose tools hunk becomes a SCAFFOLD commit. Empty feeds for `S`/`Z`.

Anticipated FUNCTIONAL commit labels (the curated patches; refine the T-numbers in sections 2-3 against these):
- 2a: `E-chardev`, `E-ioctls`, `E-saveload`, `E-iova`, `E-dma`, `E-dmaops`, `E-restore-probe`, `E-replay`, `E-queryqp`, `E-bind`, `E-susp-split`, `E-directional`, `E-teardown`, `E-fused`, `E-uuid`, `E-move`, `E-dbgioctl` (validation/PROBE_* ioctls: candidate to drop before upstream).
- 2b: `F-restmode`, `F-uar`, `F-gate`, `F-pd`, `F-querypd`, `F-mr`, `F-cq`, `F-qp`, `F-srq`, `F-dealloc`.
- 1A: `A-nldev-ufile`, `A-querymr`, `A-nldev-cqn`, `A-core-acc`.
- 1B: `B-freeze`, `B-idem`, `B-gate`, `B-trace`.
- 1C: `C-queryqp`, `C-querycq`, `C-cq-rt`, `C-inflight`.
- 1D: `D-restmode`, `D-restore-pd`, `D-restore-mr`, `D-restore-cq`, `D-restore-qp`, `D-ufile`.

Rows in branch (chronological) order:

| # | commit | subject | goal | disp | feeds / note |
|---|--------|---------|------|------|--------------|
| P1 | 52721d0 | ib_core: Add show_fdinfo for uverbs_fops | P | U | P-fdinfo (CRIU fd introspection) |
| P2 | 551a135 | uverbs: Add async-fd show_fdinfo callback | P | U | P-fdinfo |
| P3 | a753315 | uverbs: Add comp-event-fd show_fdinfo callback | P | U | P-fdinfo |
| P4 | eb6bb8b | rxe: Fix network namespaces fix | P | U | P-rxe-netns (standalone bugfix; may already be upstreamable alone) |
| P5 | f7e71a8 | ib_uverbs_write: Delete ib_safe_file_access check | P | U | P-safe-file-access; SECURITY-sensitive, needs strong justification or alternative |
| 1 | 68e92ad | mlx5_vfmig: Add chardev framework, UAPI, and per-PF state | 2a | U | E-chardev |
| 2 | 3b2ebe6 | Add ENABLE_MIGRATABLE / GET_VHCA_ID / QUERY_VF / MARK_RESTORED ioctls | 2a | U | E-ioctls |
| 3 | 01bbaef | Add SAVE_VHCA_STATE and LOAD_VHCA_STATE ioctls | 2a | U | E-saveload |
| 4 | 04e15e6 | Add SET_TRACKED ioctl and per-VF vfmig_tracked flag | 2a | U | E-saveload |
| 5 | 93f54fb | Add per-VF IOMMU domain and deterministic IOVA allocator | 2a | U | E-iova |
| 6 | 6f7c6e6 | Route command ring DMA through deterministic IOVA allocator | 2a | U | E-dma |
| 7 | eba30f3 | Route command mailboxes through allocator and add HOST_PAGE wire | 2a | U | E-dma |
| 8 | e55e593 | Route MANAGE_PAGES allocations through the IOVA allocator | 2a | U | E-dma |
| 9 | a8e8b70 | Route mlx5_dma_zalloc_coherent through the IOVA allocator | 2a | U | E-dma |
| 10 | 9532238 | tools: Add userspace harness, scripts, and Makefile | 2a | S | base harness |
| 11 | f426b68 | tools: Add test_m2r_iova.sh harness | 2a | S | |
| 12 | 40a406f | tools: harden test_m2r_iova.sh + multi-host orchestration | 2a | S | |
| 13 | bed9948 | tools: gitignore built test binaries | 2a | S | |
| 14 | 8cc6bd9 | docs: Add layered IOVA restore plan | 2a | S | design |
| 15 | 6fd7ee9 | docs: Add SKILL doc for splitting large kernel WIP commits | - | S | design (meta) |
| 16 | 053fd01 | Add transient IOVA arena for command mailboxes | 2a | U | E-iova |
| 17 | 6d5e28e | Slot-tagged deterministic IOVA allocator | 2a | U | E-iova |
| 18 | 862c388 | STREAM_HEADER + (slot, instance_key) on the wire | 2a | U | E-iova |
| 19 | 1684c26 | At-probe drift detection on the IOVA allocator | 2a | U | E-iova |
| 20 | 30edd49 | Split DMA_COHERENT into per-consumer IOVA slots | 2a | U | E-iova |
| 21 | b6f1563 | Expose per-VF tracked state via QUERY_VF | 2a | U | E-ioctls; split |
| 22 | 381aa15 | Surface SET_TRACKED no-op at info level | 2a | U | E-saveload (trivial) |
| 23 | 76b9777 | Reconstitute priv->page_root_xa on restored VFs | 2a | U | E-restore-probe |
| 24 | 87f9aab | Skip ENABLE_HCA(self) on restored VFs | 2a | U | E-restore-probe |
| 25 | d36084b | Gate mlx5_ib dev-resource init on restored VFs | 2b (2a) | U | F-gate; mc/main.c gate too |
| 26 | 7ec240c | mlx5_ib: Skip FW commands on restore for now | 2b | U | F-gate |
| 27 | 4af41d3 | vfmig: UAR migration experiment log and script | 2a | Z | experiment scaffold in main.c |
| 28 | ec254bf | Add experimental code to check FW UID persistence | 2a | Z | experiment ioctl |
| 29 | c10275f | [SQUASHME] Fix uar persistence script | - | S | tools; SQUASHME |
| 30 | f60c13e | vfmig: Don't create netdev on restored VF | 2a | U | E-restore-probe |
| 31 | f37516b | test_m24_iova.sh: Add pingpong testcase | 2a | S | |
| 32 | ce2ce26 | Kconfig to toggle IOVA allocator size | 2a | U | E-iova |
| 33 | 136c387 | UAR SAVE/RESTORE Design doc | 2b | S | design |
| 34 | e945006 | Add UAR restore hooks | 2b | U | F-uar |
| 35 | 1ba87bd | Tools to support UAR testing | 2b | S | |
| 36 | c390904 | mlx5_vfmig_uctx: Add negative testing | 2b | S | |
| 37 | 969ff39 | mlx5_ib: Add UAR QUERY/RESTORE APIs | 2b | U | F-uar (vfmig_uctx.c) |
| 38 | 2662d73 | mlx5_vfmig_uctx: Add UAR QUERY/RESTORE cases | 2b | S | |
| 39 | 38bbb3e | test_m2r_iova: Add QUERY/RESTORE UAR testcase | 2b | S | |
| 40 | e3aaf64 | vfmig: Add user MR DMA design doc | 2a | S | design |
| 41 | 083dbb9 | vfmig: Put user allocations behind iommu | 2a | U | E-dmaops |
| 42 | b8264c0 | mlx5_ib: Add dynamic UAR SAVE/RESTORE | 2b (1D) | U | F-uar; also core alloc-at-handle -> D-ufile |
| 43 | 21666ec | mlx5_vfmig_uctx: Dynamic UAR testcase | 2b | S | |
| 44 | daa2269 | [MAYBE-SQUASH-ME] vfmig dynamic UAR fixes | 2b | U | F-uar; squash into 42/37 |
| 45 | 5cf7346 | vfmig_iova: Add alloc()/free() | 2a | U | E-dmaops |
| 46 | b4f56ff | vfmig: Flesh out kcoherent area | 2a | U | E-dmaops |
| 47 | 52021cc | ml5_{core/ib/e}: Hacks which enable RC ping pong | 2a (2b) | U | WIP hack; refined into E-restore-probe + en_tx gate |
| 48 | 7d39efd | vfmig_iova: Detach VF domain early in remove_one | 2a | U | E-iova (teardown) |
| 49 | 1196787 | Add ib_uobject criu + kernel design doc | 1D | S | design |
| 50 | e10d0cb | Remove unnecessary step in ib_uobj design doc | 1D | S | design |
| 51 | 63cb898 | Kernel-side design doc improvements | 1D | S | design |
| 52 | 53f33bf | Add test scripts to validate ib_uobject id restore | 1D | S | |
| 53 | 3dbc121 | mlx5_core: vfmig: Add a QP query API | 2a | U | E-queryqp |
| 54 | 73a449b | vfmig: Add QP state test harness | 2a | S | |
| 55 | d5baa76 | Update uobj restore design with experiment results | 1D | S | design |
| 56 | 59a42e1 | uobj_restore design: notes on existing query APIs | 1D | S | design |
| 57 | 7e4aca4 | Add test script for uverbs INFO_HANDLES | 1D | S | |
| 58 | 8aeec40 | ib_uobject design: note about query_handles | 1D | S | design |
| 59 | 9c25989 | Restructure test tree with descriptive names | - | S | tools reorg |
| 60 | dc45885 | Update internal references for new test tree layout | 2a (2b) | U | comment/path churn; distribute or drop |
| 61 | 0601c49 | RDMA/nldev: Expose ufile handle alongside restrack id | 1A | U | A-nldev-ufile; split |
| 62 | c57115c | RDMA/core: Introduce ucontext_is_restore_mode predicate | 1D | U | D-restmode |
| 63 | 32ac3ea | RDMA/mlx5: Wire ucontext_is_restore_mode predicate | 2b | U | F-restmode |
| 64 | 554c306 | design: Document ucontext_is_restore_mode gate | 1D | S | design |
| 65 | f0623eb | RDMA/rxe: Wire ucontext_is_restore_mode predicate | 1D | U | D-restmode (rxe) |
| 66 | e068683 | RDMA/uverbs: Add UVERBS_OBJECT_RESTORE + RESTORE_PD; rxe impl | 1D | U | D-restore-pd |
| 67 | 7ace5b8 | uobj design doc: Mark tasks completed | 1D | S | design |
| 68 | 7cb1187 | design: Document RESTORE_PD as landed + S3a/S3b split | 1D | S | design |
| 69 | 055217e | Add pd_restore_probe_rxe for S3a validation | 1D | S | |
| 70 | 33904d4 | Add MLX5_VFMIG_IOC_PROBE_PD for S3b validation | 2a | U | E-dbgioctl; candidate-drop |
| 71 | 7d6f9e0 | Add pd_adopt empirical test for S3b | 2b | S | |
| 72 | c567858 | RDMA/mlx5: ABI for UVERBS_METHOD_RESTORE_PD UHW payload | 2b | U | F-pd |
| 73 | 4baa782 | RDMA/mlx5: implement restore_pd via Model A pdn adoption | 2b | U | F-pd |
| 74 | fd0cfc4 | design doc: replace S3b plan with landed Model A | 2b | S | design |
| 75 | 5fec54b | Add pd_restore_probe_mlx5_vfmig for S3b e2e | 2b | S | |
| 76 | 8c3764d | design: record v0 dealloc-ordering invariant for S3b | 2b | S | design |
| 77 | d5ec7d7 | RDMA/uverbs: Add RESTORE_MR + rxe impl | 1D | U | D-restore-mr |
| 78 | 25f2167 | Add mr_restore_probe_rxe for S4a validation | 1D | S | |
| 79 | d4acb54 | RDMA/mlx5: fix CRIU PD restore by exposing FW pdn + restrack | 2b | Z | fw_pdn TLV + restrack.c net-zero; superseded by QUERY_PD (213) |
| 80 | afb3b56 | RDMA/mlx5: adopt source devx_uid on restore ucontext | 2b | U | F-restmode |
| 81 | 1fe0d90 | extend test_pd_adopt with DEVX-source matrix | 2b | S | |
| 82 | 73c76f2 | RDMA/mlx5: mark ADOPT_DEVX_UID vestigial; rewrite pr_warn | 2b | U | F-restmode (refines 80) |
| 83 | a10dc00 | design: record DEVX-adoption blind spot in S3b | 2b | S | design |
| 84 | 35fb924 | RDMA/uverbs: surface user_addr + access_flags via QUERY_MR | 1A | U | A-querymr |
| 85 | f422e6b | RDMA/rxe: honour lkey/rkey identity hint in rxe_restore_mr | 1D | U | D-restore-mr (rxe) |
| 86 | 957b470 | design: QUERY_MR + S4a hint-honouring; refresh probe | 1A | S | design |
| 87 | ff4544a | RDMA/uverbs: populate ib_mr.access_flags from DM_MR_REG | 1A | U | A-querymr |
| 88 | e1cb11b | Add MLX5_VFMIG_IOC_PROBE_MKEY for S4b validation | 2a | U | E-dbgioctl; candidate-drop |
| 89 | be66ffa | Add mr_adopt empirical test for S4b | 2b | S | |
| 90 | 87f2981 | RDMA/mlx5: Add struct mlx5_ib_restore_mr_req UHW payload | 2b | U | F-mr |
| 91 | 85c651e | RDMA/mlx5: Add mlx5_ib_restore_mr (Model A mkey adoption) | 2b | U | F-mr |
| 92 | afb2deb | Add mr_restore_probe_mlx5_vfmig + harness (S4b) | 2b | S | |
| 93 | c92f630 | design: finalize S4b -- restore_mr Model A landed | 2b | S | design |
| 94 | afc4149 | ib_uobject allocator design doc edits | 1D | S | design |
| 95 | 5046029 | stage-2 foundation -- (kind, fw_id) identity + rb_tree | 2a | U | E-replay |
| 96 | a0bb5ec | stage-2 observer -- QUERY_AWAITING_BIND | 2a | U | E-dbgioctl / E-replay; split |
| 97 | 685842d | stage-2 harness -- user_object_replay probe + script | 2a | S | |
| 98 | e088700 | stage-2 emit -- HOST_USER_PAGE wire records on SAVE | 2a | U | E-replay |
| 99 | c44a482 | stage-2 replay -- HOST_USER_PAGE consumption on LOAD | 2a | U | E-replay |
| 100 | b2ff659 | stage-2 retag -- MR callsite in create_real_mr | 2a (2b) | U | E-replay; source-side retag in hw/mlx5 |
| 101 | f6416c8 | stage-2 retag -- DBR callsite in db_map_user | 2a (2b) | U | E-replay |
| 102 | fd81527 | stage-2 retag -- CQ callsite in mlx5_ib_create_cq | 2a (2b) | U | E-replay |
| 103 | c018318 | stage-2 retag -- QP callsite in create_user_qp | 2a (2b) | U | E-replay |
| 104 | 4c522ea | stage-2 retag -- SRQ callsite in mlx5_ib_create_srq | 2a (2b) | U | E-replay; F-srq |
| 105 | 3513b4e | stage-2 harness self-calibrate EXPECT_* | 2a | S | |
| 106 | 79c534f | design: finalize stage 2 | 2a | S | design |
| 107 | d21e0f2 | design: reshape stage 3 -- ib_umem_pin + wrapper | 1D | S | design |
| 108 | c87d5c6 | detach iova_dom from driverless VFs before disable_sriov | 2a | U | E-iova (teardown) |
| 109 | beea656 | ib_uverbs: silence !list_empty WARN on restore-mode ufile | 1D | U | D-ufile |
| 110 | 5b6f13a | RDMA/umem: split ib_umem_pin out of ib_umem_get | 1A (1D) | U | A-core-acc; plumbing for D/F restore-mr |
| 111 | 10fd8ab | net/mlx5: add vfmig_iova_bind_user_object (D2) | 2a | U | E-bind |
| 112 | 7c6fb51 | IB/mlx5: Stage-3 D3 umem-restore bind helpers | 2b | U | F-mr; plumbing |
| 113 | 7c49674 | IB/mlx5: Stage-3 D4 -- wire restore_mr to populate umem | 2b | U | F-mr |
| 114 | 1af0dcf | mr_restore probe -- mmap a local addr (D4) | 2b | S | |
| 115 | 117a6d2 | IB/mlx5: correct restore_mr kerneldoc | 2b | U | F-mr (trivial) |
| 116 | c66ad56 | preserve USER_PAGE high-water mark across reset_cursor | 2a | U | E-replay |
| 117 | b1acff2 | design: refresh after Stage-3 D1-D4 | 2a | S | design |
| 118 | a77cc4d | RDMA/uverbs: Add RESTORE_CQ + rxe impl | 1D | U | D-restore-cq |
| 119 | 69cb6ec | design: S5 A1 landed + S5 plan | 1D | S | design |
| 120 | 512cee7 | cq_restore_probe_rxe -- S5a validation | 1D | S | |
| 121 | e878242 | support multi-page user objects in secondary index | 2a | U | E-replay |
| 122 | 5e5b27a | RDMA/uverbs+tools: CQ-restore polish | 1D | U | D-restore-cq; split |
| 123 | 58fd5dd | cq_restore_probe_rxe -- attach UHW_OUT | 1D | S | |
| 124 | e48f4e3 | RDMA/rxe: honor source vm_pgoff in restore_cq | 1C (1D) | U | C-cq-rt (rxe restore_cq core) |
| 125 | 35297c0 | RDMA/rxe: grow rxe_restore_cq_req above inline threshold | 1C | U | C-cq-rt |
| 126 | e591255 | net/mlx5: skip mlx5_cmd_use_events on restored VF | 2a | U | E-restore-probe |
| 127 | e9791b9 | Add doc describing CREATE_MKEY issue and workaround | 2b | S | design |
| 128 | 54e449b | Add MLX5_VFMIG_IOC_PROBE_CQN for S5b validation | 2a | U | E-dbgioctl; candidate-drop |
| 129 | a87b038 | tools: Add probe_cqn verb | 2a | S | |
| 130 | 94dab9a | tests: Add test_cq_adopt.sh (S5b B0) | 2b | S | |
| 131 | 3e9affa | design: Lock in S5b B0 | 2b | S | design |
| 132 | 33600a3 | RDMA/mlx5: Add mlx5_ib_restore_cq_req UAPI (S5b B1) | 2b | U | F-cq |
| 133 | e161e7e | net/mlx5: CQ-restore bind primitives + mlx5_core_adopt_cq | 2b (2a) | U | F-cq; core plumbing |
| 134 | 769b4fe | RDMA/mlx5: Add CQ-restore umem-bind helpers (S5b B3) | 2b | U | F-cq |
| 135 | 9b60b03 | RDMA/mlx5: Implement mlx5_ib_restore_cq verb body (S5b B2) | 2b | U | F-cq |
| 136 | bdd8c9b | design: Lock in S5b B1+B2+B3 | 2b | S | design |
| 137 | f2a4130 | fw_id_continuity_probe emit CQ context (B4 prep) | 2b | S | |
| 138 | 43cb650 | Add cq_restore_probe_mlx5_vfmig (B4) | 2b | S | |
| 139 | 6783d14 | tests: Add test_cq_restore_mlx5_vfmig.sh (B4) | 2b | S | |
| 140 | c292c27 | Wire B4 probe into Makefile + flip design row | 2b | S | |
| 141 | da4dd37 | cq_restore_probe -- MAP_FIXED at src DBR VA | 2b | S | |
| 142 | 45510a8 | design: Lock in S5b B4 | 2b | S | design |
| 143 | 68fb3c0 | RDMA/mlx5: Add MLX5_IB_METHOD_VFMIG_QUERY_CQ UAPI (B5) | 2b | U | F-cq |
| 144 | 2d53a81 | RDMA/mlx5: Add mlx5_ib_db_user_virt() accessor | 2b | U | F-cq |
| 145 | e601020 | RDMA/mlx5: Stamp comp_vector hint onto cq->mcq.vector | 2b | U | F-cq |
| 146 | 3ed73fc | RDMA/mlx5: Implement QUERY_CQ handler (B5) | 2b | U | F-cq |
| 147 | 67ab707 | design: Document QUERY_CQ dump-side rationale | 2b | S | design |
| 148 | d5a084c | Add cq_query_probe_mlx5_vfmig (B5) | 2b | S | |
| 149 | 598b8c0 | tests: Add test_cq_query_mlx5_vfmig.sh (B5) | 2b | S | |
| 150 | 4d48018 | design: Lock in S6 QP-restore v0 contract | 2b | S | design |
| 151 | 27145f0 | RDMA/mlx5: Extend QUERY_QP with wider QPC subset | 2a | U | E-queryqp |
| 152 | 5b19b87 | Print wider QUERY_QP fields in CLI | 2a | S | |
| 153 | 3b584a2 | Drive QP self-loopback INIT/RTR/RTS in probe | 2b | S | |
| 154 | 3be8b36 | tests: Add K6_QP_STATE mode to fw_id_continuity | 2b | S | |
| 155 | 2f10fa4 | design: Lock in S6 RESTORE_QP via K7 RTS byte-equal | 2b | S | design |
| 156 | 85fc2a3 | RDMA/mlx5: Add mlx5_ib_restore_qp_req UAPI (B1) | 2b | U | F-qp |
| 157 | a9ffa1f | design: Lock in S6b B0+B1 | 2b | S | design |
| 158 | db93076 | RDMA/core: Add UVERBS_METHOD_RESTORE_QP framework (B2) | 1D | U | D-restore-qp |
| 159 | 1c6019f | RDMA/mlx5: Implement mlx5_ib_restore_qp handler stub (B2) | 2b | U | F-qp |
| 160 | c035547 | design: Lock in S6b B2 | 2b | S | design |
| 161 | 14ac2c2 | net/mlx5: Add mlx5_vfmig_bind_user_qp primitive (B3) | 2a | U | E-bind |
| 162 | 112b761 | RDMA/mlx5: Bind WQ + DBR umems in restore_qp (B3) | 2b | U | F-qp |
| 163 | 602ebff | design: Lock in S6b B3 | 2b | S | design |
| 164 | 9eede56 | fw_id_continuity_probe emit QP context (B4 prep) | 2b | S | |
| 165 | f5565b1 | add qp_restore_probe_mlx5_vfmig (B4) | 2b | S | |
| 166 | 248aae5 | add test_qp_restore_mlx5_vfmig.sh (B4) | 2b | S | |
| 167 | 2ed9768 | design: Lock in S6b B4 | 2b | S | design |
| 168 | 2727d8a | RDMA/core: Add ib_qp_user_handle accessor (B5 prep) | 1A | U | A-core-acc |
| 169 | 760534e | RDMA/mlx5: Add MLX5_IB_METHOD_VFMIG_QUERY_QP verb (B5) | 2b | U | F-qp |
| 170 | 28927e4 | add qp_query_probe_mlx5_vfmig (B5) | 2b | S | |
| 171 | 1111d71 | design: Lock in S6b B5 | 2b | S | design |
| 172 | 307787a | RDMA/core: Plumb restore_qp through ib_set_device_ops (B2 fix) | 1D | U | D-restore-qp |
| 173 | cf8251f | fix SIGPIPE race in qp_restore harness | 2b | S | |
| 174 | 066338a | line-buffer stdout in qp_restore_probe | 2b | S | |
| 175 | 8470fea | re-anchor qp_restore subtest 1 on RESTORE_PD | 2b | S | |
| 176 | d1c4320 | design: lock S6b B2/B3/B4 | 2b | S | design |
| 177 | a7d44ec | RDMA/restore: park IB_QPT_UC at v0 + WARN multi-port mlx5 | 1D (2b) | U | D-restore-qp; mlx5 WARN |
| 178 | 5d7a3a2 | add test_qp_query_mlx5_vfmig.sh runner (B5) | 2b | S | |
| 179 | e6002ac | design: lock S6b B5 + reconcile v0 type/state | 2b | S | design |
| 180 | 5fe60bc | RDMA/nldev: emit RES_SEND_CQN + RES_RECV_CQN on QP dump | 1A | U | A-nldev-cqn |
| 181 | e4b0b41 | add qp_nlattr_probe for RES_SEND/RECV_CQN | 1A | S | |
| 182 | c659ab6 | RDMA/mlx5: expose source devx_uid + harden destroy_qp | 2b | U | F-qp (diagnostics); split |
| 183 | ee27d8e | RDMA/mlx5: gate DEALLOC_PD on vfmig-restored PDs | 2b | U | F-dealloc; mixed mc |
| 184 | 54a7a13 | RDMA/mlx5: extend cross-uid CQ+MR; relax devx_uid check | 2b | U | F-restmode |
| 185 | a254cde | design: file qp_av_dmac_swap.md | 2b | S | design (investigation) |
| 186 | a7a17b9 | cheap diagnostic for stale-dmac hypothesis | 2b | S | net-zero investigation |
| 187 | 5166e22 | RDMA/mlx5: refresh av.dmac on vfmig-restored RC/UC QPs | 2b | Z | reverted by 0a05d29 |
| 188 | f0e74d4 | design: mark stale-dmac diagnosis locked | 2b | S | design |
| 189 | b70b662 | add userspace-triggered av.dmac refresh ioctl | 2a | Z | reverted by 6055711 |
| 190 | 1bbe576 | CLI verb + harness for av.dmac refresh | 2a | S | net-zero (reverted) |
| 191 | 09d7fd5 | design: record dev-branch backup ioctl rationale | 2a | S | design |
| 192 | d0f51b4 | design: Document vfmig tracking/management rework | 2a | S | design |
| 193 | 66edef3 | RDMA/mlx5: populate full primary_address_path on refresh | 2b | Z | reverted by 0a05d29 |
| 194 | fe188a6 | populate full primary_address_path in refresh ioctl | 2a | Z | reverted by 6055711 |
| 195 | 0a05d29 | RDMA/mlx5: REVERT restore_qp_refresh_av_dmac (infeasible) | 2b | Z | revert of 187/193 |
| 196 | 6055711 | REVERT MLX5_VFMIG_IOC_REFRESH_AV_DMAC ioctl | 2a | Z | revert of 189/194 |
| 197 | 5786cb3 | REVERT refresh_av_dmac CLI verb + harness | 2a | S | revert (tools) |
| 198 | 5dcf56d | design: record FW-rejection post-mortem | 2b | S | design |
| 199 | 9a895b8 | design: demote investigation chain to Appendix A | 2b | S | design |
| 200 | 41fb1af | design: collapse KS7.4 to orchestrator-side identity | 2a | S | design |
| 201 | bc36ac6 | design: clarify EXCLUSIVE/SHAREABLE plugin classification | 2a | S | design |
| 202 | 6f68de8 | add MLX5_VFMIG_IOC_SET_VF_UUID + extend QUERY_VF | 2a | U | E-uuid |
| 203 | e71371e | CLI verb set_vf_uuid + extend query_vf/list | 2a | S | |
| 204 | 5e7c0e6 | vf_uuid kernel-matrix probe binary | 2a | S | |
| 205 | a4f7516 | vf_uuid lifecycle + multi-VF shell harness | 2a | S | |
| 206 | 205c7f4 | design: mark KS7.3 LANDED | 2a | S | design |
| 207 | 9dec87c | use linux/uuid.h helpers for vf_uuid | 2a | U | E-uuid (refine) |
| 208 | a74e715 | CLI query_one() propagate out-fields on -ERANGE | 2a | S | |
| 209 | 8bdf0c9 | design: tighten KS7.3 to (vf_uuid, vf_id) paired match | 2a | S | design |
| 210 | a7023b3 | design: KS7.1 transient-bit + sysfs bind signal | 2a | S | design |
| 211 | 83507b9 | KS7.3 empirical multi-LOAD stage-gate probe | 2a | U | E-dbgioctl; candidate-drop; split |
| 212 | 9e67d87 | KS7.3 multi-LOAD drift_armed gate validated | 2a | U | E-dbgioctl / E-replay; split |
| 213 | 30b5006 | RDMA/mlx5: Add QUERY_PD verb; drop NLDEV fw_pdn TLV | 2b | U | F-querypd; removes 79 fw_pdn (net-zero pairing) |
| 214 | 46a5a3b | pd_query probe reports devx_uid lane | 2b | S | |
| 215 | ed1173a | RDMA/rxe: S6a QP save/restore (RESTORE_QP + verbs; rxe_vfmig.c) | 1C (1D) | U | C-queryqp + D-restore-qp(rxe); foundational rxe migrate file |
| 216 | 66a32b4 | RDMA/rxe: add QUERY_CQ verb (kernel CQ restore inputs) | 1C | U | C-querycq |
| 217 | d95db0c | net/mlx5: move VF-migration sources into core/vfmig/ | 2a | U | E-move (reorg) |
| 218 | af6ad91 | tools: fix vfmig source paths after core/vfmig/ move | 2a | S | |
| 219 | db6e1f3 | tools: revive pd_adopt + dealloc_pd_matrix capture | 2b | S | |
| 220 | 61fa124 | tools: add run_all_harnesses.sh full-suite driver | - | S | test driver |
| 221 | 4dd0384 | RDMA/mlx5: trim VFMIG_QUERY_QP to non-standard state | 2b | U | F-qp |
| 222 | 6fdc71f | RDMA/rxe: emit user_handle from QUERY_QP | 1C | U | C-queryqp |
| 223 | 4e8def1 | tools: update QUERY_QP probes + doc for trimmed verb | 1C | S | |
| 224 | 736b80d | RDMA/rxe: drop VFMIG_ infix from QUERY_QP attr names | 1C | U | C-queryqp (rename) |
| 225 | a51e452 | tools: harden drift_gate against restored-VF unbind wedge | 2a | S | |
| 226 | add51d8 | RDMA/mlx5: split SUSPEND/RESUME out of SAVE/LOAD | 2a | U | E-susp-split; split |
| 227 | ba7c27e | tools: snapshot-ordering harnesses + design docs | 2a | S | |
| 228 | 2d32da1 | tools: ignore newly added compiled probe binaries | - | S | |
| 229 | f60c2fa | tools: fix bind/next-line merge in defer_resume revert | 2a | S | defer_resume reverted |
| 230 | 6f5c8be | RDMA/rxe: drop VFMIG names, add ucontext-scoped freeze | 1B | U | B-freeze (rxe_vfmig->rxe_migrate + FREEZE_CONTEXT) |
| 231 | 4a7966d | tools: track rxe VFMIG->MIGRATE rename + FREEZE_CONTEXT | 1B | S | |
| 232 | dd1f482 | RDMA/rxe: restore in-flight QP datapath (SQ/RQ + resp res) | 1C | U | C-inflight |
| 233 | 2c8a767 | tools: validate rxe in-flight QP restore (B1) | 1C | S | |
| 234 | b282d62 | net/mlx5: force-resume parked VFs before SR-IOV teardown | 2a | U | E-teardown |
| 235 | 3d3df1b | tools: add bound-VF teardown timing repro | 2a | S | |
| 236 | 4eb1792 | RDMA/rxe: born-frozen QP restore + thaw-and-replay | 1C | U | C-inflight |
| 237 | 211b3e6 | tools: validate rxe born-frozen thaw | 1C | S | |
| 238 | d1b6ea1 | tools: drop vfmig token from rxe qp probe helpers | 1C | S | |
| 239 | 566c5dd | RDMA/rxe: arm retry on QP resume so SQ replays full payload | 1C | U | C-inflight; split |
| 240 | c28dc04 | tools: rename mlx5_vfmig test dir to criu_rdma | - | S | tree rename |
| 241 | 5519861 | RDMA/rxe: make CRIU datapath pause/resume idempotent | 1B | U | B-idem; split |
| 242 | 7640e95 | treewide: update stale tools/testing doc paths | multi | U | comment/path churn; distribute across patches or drop |
| 243 | 06b56a4 | RDMA/rxe: seed ring cursors on drained CRIU QP restore | 1C | U | C-inflight (drained) |
| 244 | 09fe649 | RDMA/rxe: drop inbound packets while frozen QP parked | 1B | U | B-gate |
| 245 | b60e047 | RDMA/rxe: drain packet queues when freezing a CRIU QP | 1B | U | B-gate |
| 246 | 46f0788 | RDMA/rxe: trace the CRIU QP freeze/thaw lifecycle | 1B | U | B-trace |
| 247 | 491dab8 | RDMA/rxe: restore responder continuity on drained path | 1C | U | C-inflight |
| 248 | cf70504 | RDMA/rxe: trace CQ state on CRIU restore | 1C | U | C-cq-rt (trace) |
| 249 | 324b366 | net/mlx5: document restored-VF UMR reg_mr hang | 2a | U | comment/doc (en_tx.c); trivial |
| 250 | 2418524 | RDMA/rxe: round-trip the CQ ring on CRIU save/restore | 1C | U | C-cq-rt; split |
| 251 | 53bf38a | tools: validate QUERY_CQ cursors in qp_query_probe_rxe | 1C | S | |
| 252 | eeca4c4 | tools/testing: gitignore top-level scratch/ | - | S | repo hygiene |
| 253 | c8298c7 | tools/testing: rework/rename datapath pause/resume doc | 2a | U | comment churn (en_tx/sriov/vfmig) + design; trivial |
| 254 | 1ff2483 | net/mlx5: add directional SUSPEND/RESUME + tri-state dp_state | 2a | U | E-directional |
| 255 | 69e3bdb | tools: add directional args to mlx5_vfmig tool | 2a | S | |
| 256 | 5cab37b | tools: exercise directional suspend/resume in harnesses | 2a | S | |
| 257 | b622dc6 | tools/testing: document implemented directional API | 2a | S | design |
| 258 | 2aeb0e0 | criu_rdma: fix RUNNING_P2P hold-window probe wedge | 2a | S | |
| 259 | feade3a | net/mlx5: keep fused SUSPEND/RESUME all-or-nothing on failure | 2a | U | E-fused; split |
| 260 | 621d579 | criu_rdma: register hold-window probe + retry bind | 2a | S | |
| 261 | 0da0f09 | RDMA/rxe: ship only the in-flight ring subspan on save/restore | 1C | U | C-inflight |

### Rollup (UPSTREAM source commits per goal)

- **2a** mlx5_core datapath: ~40 U commits -> anticipated FUNC patches `E-chardev..E-move` (16) + `E-dbgioctl` (drop candidate). 4 Z (`refresh_av_dmac` chain: 189/194/196 + experiments 27/28).
- **2b** mlx5_ib verbs: ~34 U commits -> `F-restmode, F-uar, F-gate, F-pd, F-querypd, F-mr, F-cq, F-qp, F-srq, F-dealloc` (10). 3 Z (79 fw_pdn/restrack; 187/193/195 av.dmac).
- **1A** core query: 6 U -> `A-nldev-ufile, A-querymr, A-nldev-cqn, A-core-acc` (4).
- **1B** rxe quiesce: 5 U -> `B-freeze, B-idem, B-gate, B-trace` (4).
- **1C** rxe save/inflight: ~14 U -> `C-queryqp, C-querycq, C-cq-rt, C-inflight` (4).
- **1D** core+rxe restore: ~12 U -> `D-restmode, D-restore-pd, D-restore-mr, D-restore-cq, D-restore-qp, D-ufile` (6).
- **SCAFFOLD**: ~150 (harness + design; consolidate the ~55 `design:`/`Lock in` doc micro-commits into one doc commit per goal).
- **NET-ZERO**: 7 code commits (see Z rows) + their tools partners.

### Acceptance gate / losslessness

- Verified survival anchors: UAR verbs, `QUERY_PD`, restored-VF gates all present at tip; `refresh_av_dmac` and `restrack.c` confirmed net-zero (0 hits / empty stat at tip).
- Remaining check before curation: for each `U` row, confirm its hunks land in exactly one FUNCTIONAL patch such that `git diff <upstream-branch> criu-dev-poc -- drivers include` is empty. Watch the `split`/`multi`/`trivial` rows (60, 100-104, 110, 122, 133, 177, 182, 183, 226, 242, 249, 253, 259) - these need hunk-level splitting.
- Branch-derivation recipe: rebuild branch = these rows in `#` order (U + S interleaved; Z dropped or carried-then-dropped). Upstream series = `git log` with every commit whose diff touches only `tools/ design/ scratch/` filtered out; mixed `split` commits hand-separated so the path-disjoint invariant holds.

### Verifying every branch commit is in the table

Base is `8fb0d17`; the table's commit column is the second `|`-field. This
one-shot prints the symmetric difference between the table and the branch:
lines prefixed by the first file = table hashes not on the branch (typos/stale);
lines prefixed by the second = branch commits missing from the table. Empty
output = the table and the branch agree exactly.

```sh
BASE=8fb0d172394c733b30c8bee8208da197e3e900da
DOC=tools/testing/criu_rdma/plans/upstream_series_inventory.md
comm -3 \
  <(grep -E '^\| *[A-Za-z0-9]+ *\| *[0-9a-f]{7} *\|' "$DOC" \
      | awk -F'|' '{gsub(/ /,"",$3); print $3}' | sort -u) \
  <(git log --format='%H' "$BASE"..criu-dev-poc | cut -c1-7 | sort -u)
```

(Use `comm -13` for only-missing-from-table, or `comm -23` for
only-stale-in-table, if you want the two halves separately.)

## 7. Curation workspace + progress log

Two git worktrees under `/opt/builds` (shared object store, one clone):

| Path | Branch | Tip / tracking | Role |
|------|--------|----------------|------|
| `/opt/builds/linux-stable-poc` | `criu-dev-poc-rebase` | `c729d16` (oracle tip `0da0f097` + inventory/plan commits); tracks `origin/criu-dev-poc-rebase` | **Buildable POC** — the tree we build/boot to run the `criu_rdma` harnesses; holds the full kernel build behind the running `6.19.0-raphael-criu-dev+`. Also the diff/behaviour oracle and where these plan docs live. |
| `/opt/builds/linux` | `rebase-P-0` | `f7e71a81` (dev branch through the 5 P commits); tracks `origin/criu-dev-build-up-rebase` (== `origin/6.19-dev-criu`) | **Curation** — the working tree for building the clean series. Starts at `f7e71a81` (Group P kept as-is); Group A onward is built on top. |

Branch/naming convention:
- `rebase-P-0` = curation starting point = real dev branch through the P
  prerequisites (`f7e71a81`). Successive curation stages follow the same
  `rebase-<group>-<n>` shape (e.g. Group A work on top of `rebase-P-0`).
- The plan docs (`tools/testing/criu_rdma/plans/`) live only on
  `criu-dev-poc-rebase`; they do not exist at `f7e71a81`, so edit/commit them in
  `linux-stable-poc`.

Notes:
- We build/run from the **rebase / POC** (`linux-stable-poc`), not from the
  curation tree: a kernel/modules built off the curation branch won't match the
  running POC kernel's vermagic. Runtime harness validation happens on the POC
  kernel; the curated series is compile-checked per patch and boot-tested
  separately later.
- No `master`/rc worktree — staying on 6.19 for stability; rebase onto a recent
  `-rc` is a later step for the RFC posting only.
- Stale branches from an earlier approach exist and can be deleted:
  `upstream-t1`, `upstream-t1-1` (both `74c9435`, a superseded squash-onto-6.19
  experiment).

### Curation approach (chosen)

Start from the real branch at `f7e71a81` (`rebase-P-0`), keeping the P commits
intact, and build the clean functional series forward on top per section 2/3.
Per FUNCTIONAL patch: assemble the tip (working) hunks from the oracle, write a
real message + `Signed-off-by`, compile, run the matching harness on the POC
kernel, and `checkpatch.pl --strict`. Path-disjoint SCAFFOLD (`tools/design/
scratch`) is dropped from the upstream export.

### Progress

- [x] **Workspace** set up: `linux-stable-poc` (POC/build) + `linux`
  (`rebase-P-0` @ `f7e71a81`, curation), pushed to
  `origin/criu-dev-build-up-rebase`.
- [x] **Group P (T1.0)** adopted as-is via `rebase-P-0` @ `f7e71a81`
  (`eb6bb8b` rxe-netns, `52721d0`/`551a135`/`a753315` fdinfo, `f7e71a8`
  safe-file-access). Squash of the fdinfo trio + message polish deferred to
  final export.
- [ ] **Group A (T1.1–T1.x)** on top of `rebase-P-0` — `A-querymr`
  (`35fb924`,`ff4544a`), `A-nldev-ufile` (`0601c49`, split tools), `A-nldev-cqn`
  (`5fe60bc`), `A-core-acc` (`5b6f13a` umem_pin split + `2727d8a`
  qp_user_handle). NEXT.
- [ ] Group B / C / D — see section 2.

## 8. Development ordering (incremental-test) vs submission ordering — the two-agent workflow

Sections 2-3 order patches for **submission** (horizontal layers: T1 `A→B→C→D`
read-only→quiesce→save→restore; T2 `2a→2b`). That is correct for maintainers but
**test-hostile**: no object round-trips end-to-end until the whole restore layer
(`D` / `F`) lands, so the kernel→criu→on-rig loop has nothing to validate for most
of the series.

**Development follows a different, vertical order** — the POC's own cumulative
per-object spine — because the unit of the kernel→criu handoff and of the on-rig
E2E gate is *one object type's full round-trip*, not one horizontal mechanism. The
POC was in fact built this way (design stages `S3=PD`, `S4=MR`, `S5=CQ`, `S6=QP`;
criu pass matrix `pd → pd_mr/pd_cq → pd_cq_qp → pd_cq_qp_sq`). The build-up
branches keep that spine.

Reconciliation (no rework):
- The **build-up branches are ordered for testing** (vertical, cumulative). This is
  also a fine organization for the **first architectural RFC** (per-feature slices
  read well).
- The **horizontal per-tree re-slice** (§2/§3) is deferred to the *final
  per-maintainer submission* (net-next vs rdma-next). The §6 `feeds` label on every
  commit is the pre-computed dev-order→submission-order mapping; the re-slice is
  mechanical (drop scaffold; coalesce hunks by label).

### Two phases, each built vertically (per-object) for testing

**RXE (T1) is completed end-to-end first; mlx5 (T2) is layered on top.** RXE +
core is independently postable and merges long before vfmig, so finishing it first
delivers a shippable series early, fully exercises the criu restore framework on a
proven base, and — because RXE is soft-RoCE over any netdev — spares the scarce
CX-7 rigs for the T2 phase. This is safe precisely because we curate from a
**proven oracle**: the POC already shows the final core ABI serves both RXE and
mlx5, so there is no "mlx5 forces a late core-ABI change" surprise that would
otherwise argue for per-object interleaving of the two tracks.

Within each phase, dev order is **vertical per-object** (PD→MR→CQ→QP→in-flight);
the horizontal `A→D` / `2a→2b` submission re-slice (§2-3) is derived later via the
§6 `feeds` labels.

Each milestone has two acceptance tiers (see the criu companion for the criu
column in full):
- **Dev testcase** — the synthetic, targeted round-trip (`run_vfmig_cr.sh` pass /
  rxe validator), fast dev loop.
- **Whole-workflow E2E gate** — a *live workload* migrated across hosts (e.g.
  `ib_write_bw` swapped between the two VMs) in the end-to-end migration
  environment. **Passing this is the final gate to advance to the next step.**

**Phase T1 — core uverbs + RXE (complete first; rig-cheap):**

| M | Round-trip | Kernel (feeds §6) | criu | Dev testcase → whole-workflow gate |
|---|-----------|-------------------|------|------------------------------------|
| T1.1 | PD | `D-restmode`, `D-restore-pd`, `A-nldev-ufile` | uobj DAG + claim + cdev-open + PD restore | rxe PD strict round-trip → rxe `ib_write_bw` migrate |
| T1.2 | MR | `A-querymr`, `D-restore-mr`, `A-core-acc` (umem_pin) | RESTORE_MR via pie blob | rxe MR + RDMA-WRITE acid → " |
| T1.3 | CQ | `C-querycq`, `C-cq-rt`, `D-restore-cq` | per-CQ save/restore | rxe CQ ring → " |
| T1.4 | QP (drained) | `B-freeze`, `C-queryqp`, `D-restore-qp` | per-QP dump + master/PIE RESTORE_QP | rxe born-frozen thaw → " |
| T1.5 | QP (in-flight) | `B-idem`/`B-gate`/`B-trace`, `C-inflight` | non-drained-SQ replay, thaw@RESUME_DEVICES_LATE | rxe in-flight (B1) → rxe `ib_write_bw` mid-flight migrate |

**T1 exit gate:** full RXE suite green + a whole-workflow RXE migration passes. T1
is now a postable core+rxe series (→ rdma-next / rxe), independent of vfmig.

**Phase T2 — mlx5 (layered on the complete T1 core framework; rig-bound):**

| M | Round-trip | Kernel (feeds §6) | criu | Dev testcase → whole-workflow gate |
|---|-----------|-------------------|------|------------------------------------|
| T2.0 | VHCA foundation | `E-chardev`/`E-ioctls`/`E-saveload`/`E-iova`/`E-dma`/`E-restore-probe` (2a) | plugin claim/presence, SAVE + GET_CONTEXT, cdev VMAs | VF migrates + RC ping-pong survives (`52021cc`) → — |
| T2.1 | UAR + restore-mode uctx | `F-restmode`, `F-uar` | VFMIG QUERY/RESTORE ucontext (static + dyn UAR) | uctx round-trip → — |
| T2.2 | PD | `F-pd`, `F-querypd` | mlx5 PD via UHW | `pd` → `ib_write_bw` swap |
| T2.3 | MR | `F-mr`, `E-replay`(MR), `E-bind` | mlx5 MR UHW | `pd_mr` + RDMA-WRITE acid → `ib_write_bw` swap |
| T2.4 | CQ | `F-cq` | per-CQ UHW | `pd_cq`, `pd_2cq` → `ib_write_bw` swap |
| T2.5 | QP (+ in-flight) | `F-qp`, `E-queryqp`, `E-susp-split` | per-QP UHW, snapshot-ordering | `pd_cq_qp`, `pd_cq_qp_sq` → `ib_write_bw` swap |
| T2.6 | Cross-host hardening | `E-directional`, `E-teardown`, `E-fused`, `E-uuid`, `E-move` | prerestore binary (KS7.x), rendezvous barrier | cross-host `pd_cq_qp_sq` → `ib_write_bw` swap across hosts (final) |

T2.0's `2a` foundation (chardev / SAVE-LOAD / IOVA / DMA / restored-VF probe) is
itself E2E-checkable as "VF migrates + RC ping-pong survives" *before* any
uverbs-object adoption.

### Two gates
- **Per-patch (rig-free, kernel agent):** compiles + `scripts/checkpatch.pl
  --strict`, on every curated commit.
- **Per-milestone (criu agent):** the dev testcase (setup 1) then the
  whole-workflow migration (setup 2) — the latter needs the **vermagic-matched
  build-up kernel** booted on the rig. Reconcile which worktree is the boot/build
  tree (the §7 table says `linux-stable-poc`, but the live worktree is
  `linux-poc-ref`; confirm it matches the running `6.19.0-raphael-criu-dev+`).

### Hardware scheduling (2-3 two-VM CX-7 setups)
- **Kernel agent is never rig-bound** (breakdown + compile-only) → keep it 1-2
  milestones ahead.
- **criu agent + E2E is the throughput limiter.** Setup roles: **setup 1** = dev /
  incremental testcases; **setup 2** = the whole-workflow migration environment
  (the final gate, e.g. `ib_write_bw` swapped across hosts); **setup 3** (if
  available) = regression baseline of the last-green milestone.
- **Phase T1 barely touches the CX-7 rigs** — RXE runs single-host/loopback for dev
  and over a plain VM-pair netdev for the whole-workflow migration; the CX-7 vfmig
  path is not exercised until T2. Spend the reserved rig time on T2.
- **T2.6 is the most rig-hungry** (true cross-host migration + barrier); schedule it
  when a full setup can be dedicated.

