---
name: commit inventory map
overview: "Restructure the inventory around functional goals (T1 A-D, T2 2a/2b) plus, per goal, an ordered anticipated-commits list interleaving FUNCTIONAL and SCAFFOLD commits so the rebuild branch is testable at every milestone. Add a lossless map of all 261 source commits to (goal, anticipated commit(s) fed, disposition), verified against the final base..tip diff."
todos:
  - id: goals-spine
    content: Write the Level-1 functional-goal spine (reuse T1 A-D, T2 2a/2b) with target-functionality statements
    status: pending
  - id: anticipated
    content: Under each goal, list the ordered anticipated commits (FUNCTIONAL + SCAFFOLD interleaved) we intend to write
    status: pending
  - id: source-map
    content: Map all 261 source commits to (goal, anticipated commit(s) fed, disposition UPSTREAM/SCAFFOLD/NET-ZERO)
    status: pending
  - id: verify-survival
    content: Verify each code commit's hunks against final base..tip diff; flag net-zero reverts + mixed commits to split
    status: pending
  - id: revert-clusters
    content: Confirm the known net-zero/WIP clusters (refresh_av_dmac, defer_resume, SQUASHME hacks)
    status: pending
  - id: acceptance
    content: Add per-goal FUNCTIONAL-commit rollup + losslessness acceptance check + branch-derivation recipe
    status: pending
  - id: append-doc
    content: Append everything as section 6 of upstream_series_inventory.md
    status: pending
isProject: false
---

# Commit map + testable rebuild branch (refine the inventory)

Append a new section `## 6. Commit map` to [tools/testing/criu_rdma/plans/upstream_series_inventory.md](tools/testing/criu_rdma/plans/upstream_series_inventory.md). Two levels, so we keep both the big picture (functional goals) and every small change (all 261 source commits).

## Model

Three dispositions (not "drop"):
- **UPSTREAM** — code hunks that survive into a real functional patch.
- **SCAFFOLD** — harness/design/scratch commits kept on the rebuild branch so we can test at each milestone; stripped only at final upstream export. Noisy incremental design-doc micro-commits are *consolidated* (one doc commit per goal); harness commits stay as-is so each remains runnable.
- **NET-ZERO** — reverted/superseded; contributes nothing upstream.

Two levels:
- **Level 1 - functional goals**: reuse the existing groups as the spine (T1 Group A/B/C/D; T2 sub-track 2a/2b). Each goal gets a *target functionality* statement + an ordered **anticipated-commits** list interleaving FUNCTIONAL patches and the SCAFFOLD commits that test/document them, in branch order.
- **Level 2 - source-commit map**: all 261 commits, each row = `(short hash, subject, goal, anticipated commit(s) fed, disposition, note)`. "Fed" is empty for NET-ZERO.

## Why the rebuild branch is testable by construction
- SCAFFOLD commits are **path-disjoint** from code (touch only `tools/`, `design/`, `scratch/`).
- Branch order per goal: FUNCTIONAL milestone(s) then its SCAFFOLD harness commit -> the harness is runnable the moment the capability lands.
- Upstream series is derived mechanically: drop every commit whose diff touches only `tools/design/scratch`. Clean by construction.
- **Mixed commits** (e.g. `RDMA/uverbs+tools: CQ-restore polish`, `5e5b27a8`) are **split** at curation: code hunk -> FUNCTIONAL commit, tools hunk -> SCAFFOLD commit, preserving the path-disjoint invariant.

## Classification method (rigor = verified)
- **125 drop-only commits** (only `tools/design/scratch`): tag SCAFFOLD (consolidating doc micro-commits) or NET-ZERO. Confirmed via `git log ... -- drivers include` set-difference.
- **136 code commits** (`drivers/`/`include/`): inspect `git show <c> -- drivers include`, assign to a goal + anticipated FUNCTIONAL commit by file+hunk, then verify survival:
  - Survives if present in `git diff f7e71a81b2ad..criu-dev-poc -- <file>` -> UPSTREAM.
  - Reverted/superseded -> NET-ZERO (pair flagged in note).
  - Touches tools too -> mark as mixed/split.

## Known net-zero / WIP clusters to verify and flag
- S6b `refresh_av_dmac` add-then-revert: `b70b6624`, `1bbe576b`, `fe188a60`, `66edef3f`, `5166e228` vs reverts `0a05d29e`/`6055711a`/`5786cb30` - expect mostly NET-ZERO.
- `defer_resume` revert (`f60c2fa5`), `ADOPT_DEVX_UID` marked vestigial (`73c76f29`).
- Explicit WIP markers: `52021ccf` "Hacks which enable RC ping pong", `daa2269d` "[MAYBE-SQUASH-ME]", `c10275fb` "[SQUASHME]".

## Acceptance gate (losslessness)
- Every hunk in `git diff f7e71a81b2ad..criu-dev-poc -- drivers include` attributed to exactly one FUNCTIONAL anticipated commit. Unattributed hunk = missed commit.
- Per-goal rollup: count of FUNCTIONAL anticipated commits (sanity vs. the ~15+15 estimate).
- Record the one-line branch-derivation recipe (path-filter to produce the upstream series from the rebuild branch).

## Not in scope
- No code changes; read-only git analysis only. The `umr.c`/`mr.c` side-branch stays excluded.
