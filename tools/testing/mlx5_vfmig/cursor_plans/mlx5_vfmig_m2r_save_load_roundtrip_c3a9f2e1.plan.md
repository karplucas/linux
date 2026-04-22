---
name: mlx5 vfmig M2-revised SAVE+LOAD round-trip
overview: Supersedes the original M2 plan. The LOAD-side control plane built in M2 has been validated as plumbing-clean via a synthetic blob (anon-inode fd, FSM, PD/MKEY/DMA registration, LOAD_VHCA_STATE issued, lifecycle stable). What was not validated is *semantic* correctness of a real round-trip; the original M2 acceptance step (VFIO mlx5 SAVE → our LOAD) is blocked on this VM by the absence of any IOMMU. M2-revised adds a SAVE counterpart in the same cdev, mirroring LOAD line-for-line (PD/MKEY/DMA, anon-inode fd, VFIO-mlx5 wire format), so the round-trip can be validated entirely through /dev/mlx5_vfmig with no VFIO/IOMMU dependency. Pause/resume are subsumed as internal sub-steps of save/load -- userspace sees only save_vhca_state, load_vhca_state, mark_restored.
todos:
  - id: investigate_load_resume
    content: Confirm whether LOAD_VHCA_STATE leaves the VHCA in a state where mlx5_core probe (with INIT_HCA skipped) can run QUERY_ADAPTER, or whether a RESUME_VHCA is required between LOAD and bind. Settles where pause/resume sit internally.
    status: pending
  - id: uapi_save
    content: Add struct mlx5_vfmig_save_state and MLX5_VFMIG_IOC_SAVE_VHCA_STATE to include/uapi/linux/mlx5_vfmig.h
    status: pending
  - id: helpers_save
    content: "Add SAVE-side firmware command wrappers to vfmig.c: vfmig_cmd_suspend_vhca, vfmig_cmd_resume_vhca, vfmig_cmd_query_vhca_migration_state, vfmig_cmd_save_vhca_state. Same TODO(vfmig-dedup) markers as M2."
    status: pending
  - id: save_fd
    content: "Implement anon-inode save fd: open allocates ctx + queries vhca_id + suspends VHCA + sizes the buffer + allocates PD/pages/MKEY (DMA_FROM_DEVICE) + issues SAVE_VHCA_STATE; read() drains pages framed as a single FW_DATA record (or chunked records if size > MAX_LOAD_SIZE); release() frees everything and (per investigate_load_resume) optionally resumes the VHCA before drop."
    status: pending
  - id: load_post_resume
    content: If investigate_load_resume concludes RESUME_VHCA is required, add it to vfmig_load_release_resources (LOAD path), keyed by 'image_was_loaded'. Else document explicitly why not.
    status: pending
  - id: ioctl_dispatch_save
    content: Wire MLX5_VFMIG_IOC_SAVE_VHCA_STATE into vfmig_ioctl(); same kref + rwsem model as the LOAD ioctl
    status: pending
  - id: tool_verb_save
    content: Add 'save_vhca_state <vf_id> <blob_path>' verb to tools/testing/mlx5_vfmig/mlx5_vfmig.c (mirror of the load verb, with read+pump into file)
    status: pending
  - id: test_roundtrip
    content: "Write tools/testing/mlx5_vfmig/test_m2r.sh: end-to-end round-trip on this VM. Spin a VF, bind to mlx5_core, save to /tmp/vf.blob, drop sriov_numvfs, recreate with autoprobe=0, load /tmp/vf.blob, mark_restored, bind. No VFIO involvement."
    status: pending
  - id: ack_roundtrip
    content: "Acceptance: dmesg shows SAVE session opened/closed cleanly, LOAD session opened/closed cleanly, post-bind has 'skipping INIT_HCA' AND no 'bad system state', and the rebound VF reaches the same operational state as the original (e.g. /sys/class/net/<X>/operstate == up if pre-save netdev was up)."
    status: pending
isProject: false
---

## Why M2-revised supersedes M2

The original M2 plan finished implementation but left its acceptance step gated on `mlx5_vfio_pci` producing a blob via `STOP_COPY`. On the validation VM:

- `/sys/kernel/iommu_groups/` is empty -- no SMMU coverage at all.
- `mlx5_vfio_pci` probe returns `-EINVAL` because `vfio_pci_core_register_device` requires an `iommu_group` on the device.
- This is a platform/firmware limitation of the VM, not anything fixable in the driver.

Independent of that blocker, an in-driver SAVE counterpart is desirable on its own merits:

- **No VFIO dep for save**: CRIU integration is one cdev, three ioctls. No VFIO state machine, no `mlx5_vfio_pci` rebind dance.
- **Symmetry**: SAVE and LOAD become first-class operations of the host driver. The VFIO variant becomes "live-migration only".
- **Code reuse already established**: M2 LOAD proved out the full helper stack (`vfmig_register_dma_pages`, `vfmig_create_mkey`, `vfmig_alloc_pages`, `vfmig_wire_header`). SAVE reuses them with `DMA_FROM_DEVICE` and `SAVE_VHCA_STATE`.
- **Faster, fewer moving parts**: today's VFIO save needs `mlx5_core unbind → mlx5_vfio_pci bind → STOP_COPY → unbind → ...`. Our SAVE keeps the VF bound to `mlx5_core` throughout (or accepts an unbound VF), then SUSPEND/SAVE/RESUME via the PF mdev.

## Pause/resume are internal, not a separate userspace API

CRIU is fundamentally stop-the-world. Pre-copy / P2P / live tracking are live-migration concerns we don't have. The firmware *does* require the VHCA to be quiesced for SAVE_VHCA_STATE, so internally:

```
SAVE ioctl:
    SUSPEND_VHCA(vhca_id, op_mod=INITIATOR)
    QUERY_VHCA_MIGRATION_STATE -> required_umem_size
    alloc PD/pages/MKEY (DMA_FROM_DEVICE)
    SAVE_VHCA_STATE  (chunked if size > VFMIG_MAX_LOAD_SIZE)
    [VF will be destroyed by sriov_numvfs=0; no RESUME needed]
    teardown PD/MKEY/DMA, drain pages out via read()

LOAD ioctl (existing):
    [VF freshly created, autoprobe=0, in INIT-equivalent state]
    LOAD_VHCA_STATE  (chunked from blob)
    [maybe] RESUME_VHCA(vhca_id, op_mod=RESPONDER)   <-- TBD, see open Q1
    teardown
```

Userspace surface stays minimal:


| ioctl                        | role                                                                   |
| ---------------------------- | ---------------------------------------------------------------------- |
| `SAVE_VHCA_STATE` (new)      | returns read-only fd; userspace `read()`s the blob                     |
| `LOAD_VHCA_STATE` (existing) | returns write-only fd; userspace `write()`s the blob                   |
| `MARK_RESTORED` (existing)   | flips the per-VF restored bit; next `mlx5_core` probe skips `INIT_HCA` |


CRIU dump path: `SAVE → close → file out`. CRIU restore path: `LOAD → close → MARK_RESTORED → bind`. No `pause`, no `resume`, no `query_state`.

## Data flow

```mermaid
sequenceDiagram
    participant U as Userspace (CRIU/test)
    participant V as /dev/mlx5_vfmig/[PF]
    participant S as anon-inode SAVE fd
    participant FW as mlx5 firmware

    Note over U,FW: SAVE phase
    U->>V: ioctl SAVE_VHCA_STATE { vf_id }
    V->>FW: QUERY_HCA_CAP(other_function=1) yields vhca_id
    V->>FW: SUSPEND_VHCA(vhca_id, INITIATOR)
    V->>FW: QUERY_VHCA_MIGRATION_STATE yields size
    V->>V: alloc_pd, alloc_pages, register MKEY (DMA_FROM_DEVICE)
    V->>FW: SAVE_VHCA_STATE(vhca_id, mkey, size)
    V-->>U: save_fd
    loop drain
        U->>S: read(buf, n)
        S-->>U: bytes (FW_DATA framing prepended on first read)
    end
    U->>S: close (or read returns 0 on EOF)
    S->>S: unregister DMA, free pages, dealloc PD

    Note over U,FW: VF teardown / recreate
    U->>U: sriov_numvfs=0 then sriov_numvfs=1 (autoprobe=0)

    Note over U,FW: LOAD phase (existing M2)
    U->>V: ioctl LOAD_VHCA_STATE { vf_id } yields load_fd
    U->>V: write(load_fd, blob), kernel issues LOAD_VHCA_STATE per FW_DATA record
    U->>V: ioctl MARK_RESTORED { vf_id }
    U->>U: echo $VF into mlx5_core/bind
    Note over FW: probe skips INIT_HCA; QUERY_ADAPTER must now succeed
```



## Files touched

### UAPI: `include/uapi/linux/mlx5_vfmig.h`

Add a single ioctl + small struct, mirroring `mlx5_vfmig_load_state`:

```c
struct mlx5_vfmig_save_state {
    __u32 vf_id;     /* in  */
    __u32 flags;     /* in  -- must be 0 for now */
    __s32 save_fd;   /* out -- anon-inode, read-only */
    __u32 reserved;
};
#define MLX5_VFMIG_IOC_SAVE_VHCA_STATE \
    _IOWR(MLX5_VFMIG_IOC_MAGIC, 0x05, struct mlx5_vfmig_save_state)
```

Comment the wire format the same way LOAD does: byte-compatible with the VFIO mlx5 stream so a saved blob is portable in either direction.

### Kernel: `drivers/net/ethernet/mellanox/mlx5/core/vfmig.c`

Logical additions, all marked `TODO(vfmig-dedup)`:

1. **New FW command wrappers** (~80 LoC each):
  - `vfmig_cmd_suspend_vhca(pf_mdev, vhca_id, op_mod)` -- mirror of `mlx5vf_cmd_suspend_vhca` ([cmd.c:38](drivers/vfio/pci/mlx5/cmd.c)) without the `state_mutex` lockdep / `mvdev_detach` checks; we already serialize via `vfmig->lock`.
  - `vfmig_cmd_resume_vhca(pf_mdev, vhca_id, op_mod)` -- mirror of [cmd.c:72](drivers/vfio/pci/mlx5/cmd.c).
  - `vfmig_cmd_query_vhca_migration_state(pf_mdev, vhca_id, *size)` -- minimal slice of [cmd.c:88](drivers/vfio/pci/mlx5/cmd.c) (no PRE_COPY, no INC, no chunk_mode -- we're stop-the-world).
  - `vfmig_cmd_save_vhca_state(pf_mdev, vhca_id, mkey, size)` -- mirror of [cmd.c:738](drivers/vfio/pci/mlx5/cmd.c) sync-only path.
2. **Per-save context** (`struct mlx5_vfmig_save_ctx`): `vfmig` back-ref + kref, `vf_id`, cached `vhca_id`, PD/MKEY/page list (same as LOAD), captured `state_size`, parser state for the *outgoing* stream (we emit one or more FW_DATA records, then EOF), `bytes_emitted`, `read_io_lock` mutex.
3. **Anon-inode save fd**:
  - `vfmig_ioc_save_vhca_state`: validates vf_id, queries vhca_id, allocates PD, suspends the VHCA, queries state size, allocates pages sized for size, registers MKEY (`DMA_FROM_DEVICE`), runs SAVE_VHCA_STATE (one shot if `size <= VFMIG_MAX_LOAD_SIZE`, else chunked). Hands back a read-only anon-inode fd whose pages already hold the snapshot.
  - `vfmig_save_read`: copies bytes from pages out to userspace in PAGE_SIZE-bounded chunks, prepending the `vfmig_wire_header { record_size=size, flags=0, tag=FW_DATA }` on the very first read. Returns 0 on EOF (post-record).
  - `vfmig_save_release`: tear down DMA / MKEY / PD; if open had bumped `vhca_resumed=false`, also issue RESUME_VHCA (only when the save fd is being closed without the VF having been destroyed already -- a `numvfs=0` between would fail SUSPEND-state cmds anyway). For CRIU's "dump-then-destroy" flow, the VF is gone before close, so RESUME silently no-ops.
4. **Updated `mlx5_vfmig_pf`**: add `struct list_head save_ctxs` + reuse `load_ctxs_lock` (renamed `ctxs_lock`) so `pf_cleanup` can iterate and tear down both flavours of session synchronously while pf_mdev is alive.
5. **LOAD post-bring-up** (conditional on the open question): if QUERY_ADAPTER still fails post-LOAD without a RESUME, add `vfmig_cmd_resume_vhca(pf_mdev, vhca_id, RESPONDER)` at the tail of `vfmig_load_release_resources` (only when an image was actually loaded).

### Userspace tool: `tools/testing/mlx5_vfmig/mlx5_vfmig.c`

Add `save_vhca_state <vf_id> <blob_path>`:

- ioctl `SAVE_VHCA_STATE`, get `save_fd`.
- Open `<blob_path>` for write.
- `read(save_fd, buf, sizeof(buf))` until EOF, `write()` into the file.
- Print bytes written.

### Test scaffold: `tools/testing/mlx5_vfmig/test_m2r.sh` (new, supersedes `test_m2.sh` for the round-trip case)

End-to-end on a single host, no VFIO:

1. `sriov_drivers_autoprobe=1`, `sriov_numvfs=1` -- VF auto-binds to `mlx5_core`. Wait for `/sys/class/net/<vfX>` to appear.
2. (Optional) `ip link set <vfX> up`, then verify operstate, to give the SAVE side something non-trivial to capture.
3. `./mlx5_vfmig $PF save_vhca_state 0 /tmp/vf.blob` -- expect a multi-KB blob.
4. `sriov_numvfs=0`, then `sriov_drivers_autoprobe=0`, `sriov_numvfs=1`.
5. `./mlx5_vfmig $PF load_vhca_state 0 /tmp/vf.blob`
6. `./mlx5_vfmig $PF mark_restored 0`
7. `driver_override` + bind to `mlx5_core`.
8. **Acceptance**: dmesg shows the M1' "skipping INIT_HCA" line AND no `bad system state`, AND `mlx5_init_one` returns 0 (no `probe_one ... failed` line), AND a netdev appears.

The synth-only smoke test (`test_m2_synth.sh`) stays around as a regression for the LOAD plumbing in isolation.

## Open questions / risks

1. **(Q1) Does mlx5_core probe succeed after LOAD_VHCA_STATE without an explicit RESUME_VHCA?** The M1' "bad system state" we still see in synth could be because (a) the bytes are garbage, OR (b) the VHCA needs RESUME after LOAD before any other command works. The VFIO variant always traverses `RESUMING → STOP → RUNNING` via its state machine, which goes through RESUME_VHCA implicitly. We don't have that machine. **Mitigation**: implement SAVE first; if a real LOAD still hits `bad system state`, add RESUME_VHCA(RESPONDER) to the LOAD release path and re-test. If both paths still fail, this becomes a blocking sub-investigation -- could mean we also need MODIFY_VHCA_STATE or a teardown-style transition not visible in cmd.c.
2. **(Q2) Can SAVE_VHCA_STATE deliver state larger than VFMIG_MAX_LOAD_SIZE in a single call?** `query_vhca_migration_state.required_umem_size` is the FW's answer for the buffer size; the LOAD command itself is bounded by `load_vhca_state_in.size`. The VFIO variant chunks save output via incremental SAVE + framing, but only when `chunk_mode` is set. For our stop-the-world path with `chunk_mode=0` and sane VF sizes (typical mlx5 VHCA state is tens of KB to low MB), one shot should suffice. **Mitigation**: implement single-shot first; if QUERY returns a size > MAX_LOAD_SIZE, fail with a clear `-EOPNOTSUPP` and a note in dmesg. Add chunking later if real VFs hit the cap.
3. **(Q3) SUSPEND op_mod choice**: VFIO uses INITIATOR for the local side and RESPONDER on the peer. For a non-P2P stop-the-world snapshot, INITIATOR is correct; but firmware may require a specific transition order. **Mitigation**: try INITIATOR, fall back to RESPONDER if SUSPEND_VHCA returns `bad parameter`.
4. **(Q4) VF being bound to mlx5_core during SAVE**: SUSPEND_VHCA on a vhca whose driver is actively issuing commands could race. The cleanest model is to require userspace to first quiesce the VF (e.g. `ip link set down`, or unbind `mlx5_core`) before SAVE. **Mitigation for v1**: document the requirement, optionally hard-enforce by checking that the VF has no driver bound (return `-EBUSY` otherwise). The test script will go the unbind route to keep noise low.
5. **(Q5) `vhca_id` after VF recreate**: when `numvfs=0; numvfs=1` cycles the VF, the new VF likely gets a fresh `vhca_id`. The blob's contents should be agnostic (firmware embeds whatever it embeds; LOAD assigns it to the destination vhca_id we pass in). **Mitigation**: empirically verify on first run; if blobs are not agnostic, escalate (would imply we need to scrub or remap inside the FW_DATA payload, which would be a much larger problem).
6. **PF unbind during a long SAVE**: same model as LOAD. `pf_cleanup` synchronously calls a save-side `release_resources` for every open save fd, leaving the fd valid but turning subsequent reads into `-ENODEV`.

## Acceptance criteria for M2-revised

1. `MLX5_VFMIG_IOC_SAVE_VHCA_STATE` returns a readable anon-inode fd; reading it streams a non-empty blob whose first 16 bytes parse as `vfmig_wire_header { record_size > 0, flags=0, tag=FW_DATA }`.
2. `MLX5_VFMIG_IOC_LOAD_VHCA_STATE` accepts the blob written by SAVE without `-EINVAL` / `-EOPNOTSUPP`.
3. After SAVE → recreate VF → LOAD → MARK_RESTORED → bind, dmesg has the `skipping INIT_HCA` line and **no** `bad system state` lines; `mlx5_init_one` returns 0.
4. The post-restore VF reaches operational parity with the pre-save VF (e.g. netdev visible in `/sys/class/net`, link can be brought up).
5. Closing the SAVE fd without reading anything is a no-op for plumbing; closing the LOAD fd without writing is the same (existing).
6. The synth regression (`test_m2_synth.sh`) still passes -- LOAD plumbing is not regressed.

