# RXE checkpoint/restore implementation plan

Status: proposed implementation plan

Last reviewed: 2026-09-04

Scope: CRIU checkpoint/restore for userspace RDMA contexts backed by Soft-RoCE (RXE), with emphasis on the restore path and in-flight RC traffic.

## 1. Sources and repository baseline

This plan is based on the following sources:

- The Google document **CRIU RDMA Checkpoint/Restore**, tab `t.0`, document ID `1OboLlljj7QzudJJ0WpFC63A5-NFi0rSJk6kXsDbvxZw`. The revision read for this plan reported a last modification time of 2026-09-04 13:55 UTC.
- `criu/`, checked out at `e472b0e3a` on `criu-rdma-rebase-v7.2-vfmig-bootstrap`.
- `linux/`, checked out at `4c9a0a6a928c` on `criu-rdma-rebase-v7.2-vfmig-bootstrap`.
- The older RXE proof-of-concept tips, `8de5c8d48` in CRIU and `0da0f097bc1a` in Linux.
- The newer Linux design work at `70dbd70e77db` and `6a543ea2f4db`, including the stable-IOVA and split-PASID proposals. Those proposals are relevant to mlx5/VFMIG but are not prerequisites for RXE, because RXE executes in software and its userspace queue pages do not contain NIC DMA addresses.

The active v7.2 branches are currently led by mlx5 SR-IOV VFMIG work. They also contain a rebased RXE series, including `FREEZE_CONTEXT`, born-frozen restored QPs, responder-resource capture, and SQ/RQ/CQ image transport. The older RXE tip is not an ancestor of the v7.2 branch, so later fixes must be ported by behavior, with tests, rather than merged or cherry-picked blindly.

## 2. Required architectural decisions

### 2.1 Treat RXE queue mappings as memory, not QP/CQ attributes

The target design is:

- CRIU saves and restores the complete RXE SQ, RQ, SRQ, and CQ mappings through an explicitly opted-in device-VMA memory path.
- `RESTORE_QP` and `RESTORE_CQ` create empty object shells with the correct identity, relationships, queue geometry, and mmap offsets. They do **not** copy queue entries or seed queue cursors.
- After all mappings and their bytes have been restored, a separate RXE restore-finalization operation validates the mappings, imports the kernel-owned shadow indices, applies non-memory RXE state, and makes the context eligible to run.
- The context remains born-frozen until every object has finalized successfully. Thaw is a distinct commit step immediately before CRIU releases the tasks.

This follows the ownership distinction already present on mlx5: queue pages are memory, while device-private execution state belongs in a device image. RXE happens to allocate the queue backing pages in the kernel and mmap them to userspace, but that allocation detail should not turn queue contents into `RESTORE_QP` parameters.

### 2.2 Use an RXE state image, not a literal firmware VHCA image

RXE has no firmware VHCA and no single hardware `SAVE_VHCA_STATE` command. Its equivalent should be a versioned logical **RXE device/context state image** assembled through generic RDMA migration hooks. It contains only state that cannot be recovered from:

1. ordinary process memory,
2. the opted-in RXE queue VMAs, or
3. existing RDMA netlink/uverbs queries.

The image must contain stable, explicitly defined fields. It must never be a dump of `struct rxe_dev`, `struct rxe_qp`, `struct resp_res`, pointers, locks, work items, timers, pool internals, or other kernel-layout-dependent data.

### 2.3 Create the RXE device before loading its state

Use the existing RDMA netlink `NEWLINK` model to create or resolve the destination RXE device after the destination network namespace and backing netdevice exist:

```text
RDMA_NLDEV_CMD_NEWLINK
  DEV_NAME=<destination ibdev>
  LINK_TYPE=rxe
  NDEV_NAME=<restored backing netdev>
```

Do not define a command where loading opaque state silently creates an RXE device. Device creation is a network/topology operation with namespace, naming, privilege, collision, and rollback rules; image loading is an RDMA restore transaction. Keeping them separate gives useful error reporting and reuses the standard control plane.

The plugin may issue `NEWLINK` on CRIU's behalf. A small restore-mode extension can be added if the kernel must mark a newly created device as restore-pending before any ucontext is opened. If that extension is unnecessary, restore-pending state can begin when the first restore-mode ucontext is allocated.

### 2.4 Initial support boundary

The first production-quality milestone should support:

- userspace RXE;
- RC QPs only;
- PD, ordinary pinned MR, CQ, and QP objects;
- SQ/RQ/CQ mapped queues;
- one or more ucontexts and RXE devices, including network namespaces;
- empty and in-flight RC SEND, RDMA WRITE, RDMA READ, and atomic operations;
- polling CQs initially, followed by completion channels as a required follow-on milestone.

Dump must reject unsupported live objects before it writes a usable image. In particular, the initial milestone should reject SRQ, AH, MW, multicast membership, UD/UC/raw QPs, completion channels, DMABUF/peer-memory MRs, implicit ODP, and RDMA-CM state until their individual sections below are implemented.

RC is the safe initial transport because the current freeze path can discard packets in the race window and rely on RC replay/duplicate suppression. UC and UD cannot use that recovery rule; they require a proven lossless quiesce or an explicit application-level quiescence contract.

## 3. Non-negotiable restore invariants

1. No restored QP may transmit or consume receive work until its queue pages, private state, dependent MRs, and peer-visible identifiers are complete.
2. No queue data or cursor is applied by `rxe_restore_qp()` or `rxe_restore_cq()`.
3. Every restored queue mapping must retain its source `vm_pgoff`, size, type, and geometry, or restore must fail.
4. A mapped queue header and the kernel's shadow index must describe the same ring before thaw.
5. Source QPNs and MR keys that are visible on the wire must be restored exactly or fail. Local-only pool indices do not need preservation.
6. Destination device indices, uverbs minor numbers, ifindices, and kernel addresses are never treated as persistent identity.
7. Restore is transactional: validate and build while frozen, commit once, and tear down all newly created resources on failure.
8. An image from an unsupported ABI version or with unknown required feature bits must be rejected, not partially interpreted.
9. The source is thawed on dump failure and when `--leave-running` requires it. The destination is never thawed after a partial restore.
10. The first implementation restores logical time: kernel task objects and absolute timer deadlines are reconstructed, not byte-copied.

## 4. State ownership and serialization inventory

The implementation should begin with a field-by-field audit of RXE state. The following is the expected classification; the audit and tests decide the final schema.

| State | Source of truth | Restore treatment |
|---|---|---|
| RXE ibdev name, driver, backing netdev, netns | RDMA netlink plus CRIU network inventory | Save a stable association and create/map the destination device before uverbs restore. Do not save numeric device or netdev indices as identity. |
| Port state, MTU, MAC-derived GID, link state | netlink/sysfs and restored netdevice | Recompute/query after `NEWLINK`; validate required MTU/GID capabilities. Do not serialize `rxe_dev.port` wholesale. |
| Device pools, pool cursors, stats, pending mmap list, monotonic mmap allocator | kernel implementation detail | Recreate. Forced mmap offsets advance the allocator. Do not serialize. |
| ucontext identity and restore mode | CRIU uverbs image plus kernel restore session | Recreate as restore-mode/born-frozen; associate it with a restore UUID and expected object counts. |
| PD handle and parent context | generic RDMA uobject DAG | Restore through `RESTORE_PD`. |
| MR handle, PD, IOVA/VA, length, access, lkey/rkey | generic uobject image plus query data | Restore through `RESTORE_MR`; repin restored process pages. Preserve wire-visible keys exactly. |
| CQ handle, size, vector, event context, completion-channel relation | generic uobject image/query | Create shell through `RESTORE_CQ`. Queue bytes/cursors come from VMA pages. Restore arm state and events separately. |
| `rxe_cq.notify` | not currently queryable | Add to the versioned RXE private CQ record and apply at finalization. |
| SQ/RQ/SRQ/CQ ring header and entries | mapped queue pages | Dump and restore as opted-in VMA memory. Never place in QP/CQ UHW blobs in the target ABI. |
| Queue geometry (`index_mask`, element size, mapped length, type) | create attributes plus kernel result | Record expected geometry as metadata and validate it against the new queue and restored header. It is not permission to trust image-provided allocation sizes. |
| Kernel shadow queue index | RXE private `rxe_queue.index` | Derive only after pages are restored: SQ/RQ/SRQ (`FROM_CLIENT`) import `consumer_index`; CQ (`TO_CLIENT`) imports `producer_index`. |
| QP type, state, caps, CQs/SRQ/PD, signal mode | generic query/uobject DAG | Create the shell and relationships through generic restore methods. |
| QPN and source mmap offsets | wire-visible identity and VMA identity | Restore exactly or fail. Keep in shell-creation metadata. |
| AV, destination QPN, qkey, pkey, port, path MTU, access flags | queryable in part; current RXE `QUERY_QP` fills gaps | Store as stable QP transport fields, not raw `struct rxe_av`; use an explicit UAPI representation. |
| SQ/RQ PSNs and live requester/completer/responder PSNs | not fully available from normal netlink | Serialize in the RXE private QP record and validate 24-bit PSN ranges. |
| requester WQE position and SSN | private RXE execution state | Prefer deriving replay start from restored SQ consumer. Save SSN and any non-derivable progress; define the replay rule explicitly. |
| configured and remaining retry/RNR counts | configured values partly queryable; remaining values private | Save both if observable behavior must remain transparent. Do not silently reset exhausted budgets to their configured maximum. |
| retry/RNR timers and worker/task state | transient kernel mechanics | Save semantic state only: whether retry/RNR was pending and an optional remaining logical delay. Recreate timers/tasks; never copy timer or task structs. Immediate retry on thaw is acceptable only if made an explicit, tested policy. |
| responder `msn`, PSNs, opcode, status, AETH state | private protocol continuity | Serialize stable scalar values required to accept peer replay after restore. |
| responder RDMA-read/atomic/flush resources (`qp->resp.resources`) | private protocol continuity | Serialize each active resource in a versioned tagged record, in logical order. Rebuild the destination array and cursors. Never copy raw `struct resp_res`. |
| responder transient pointers (`wqe`, `mr`, `res`) and current skb | kernel addresses/transient execution | Reach a defined freeze safe point where they are absent, or reconstruct them from restored handles/rkeys and ring positions. Never serialize pointers. |
| packet queues and packets racing freeze | transient network state | Drain/drop under the RC replay contract. No skb serialization in v1. |
| AH, SRQ, MW, multicast, RDMA-CM state | additional objects/private state | Reject in v1; add explicit object records and restore methods in later milestones. |
| async and completion events | fd/object plus unread event stream | Restore fd topology, associations, arm state, and defined pending-event semantics before enabling in the support matrix. |

### 4.1 Freeze-safe-state audit

For each requester, completer, and responder field in `rxe_verbs.h`, document one of:

- recreated default;
- derived from a restored queue or another object;
- serialized stable field;
- required to be empty at the freeze safe point; or
- unsupported state that makes checkpoint fail.

Add assertions or validation for the last two categories. The current proof of concept captures useful continuity fields, but it also restores configured retry counts into remaining counters and copies the native responder-resource array. Both must be corrected before declaring the image ABI stable.

## 5. Image and UAPI design

### 5.1 CRIU image records

Add one RXE device record per source ibdev and one RXE private record per restored ucontext/object as needed. A suggested logical schema is:

```text
RxeDeviceEntry
  schema_version
  required_features / optional_features
  restore_uuid
  source_ibdev_name
  source_netns_id
  backing_netdev_id and source name
  link_type = "rxe"
  expected port/MAC/MTU/GID properties
  expected context/object counts

RxeContextEntry
  restore_uuid
  context image id
  device image id
  feature bits
  expected queue mappings and objects

RxeQueueMappingEntry
  context image id
  owner object type and CRIU object id
  SQ/RQ/SRQ/CQ role
  source vm_pgoff
  mapping size
  index mask and log2 element size
  sharing/canonical-page-set id

RxeQpPrivateEntry
  QP image id / ufile handle / QPN cross-checks
  versioned transport continuity fields
  replay policy and timer/retry state
  zero or more tagged responder-resource records

RxeCqPrivateEntry
  CQ image id / ufile handle cross-checks
  notification arm state
```

Use protobuf for structure and bounded blobs only where it is useful. Every variable array needs a count, byte length, per-record type/version, maximum, and overflow-safe validation. Include required feature bits so an older kernel/plugin fails cleanly.

### 5.2 Kernel UAPI operations

Refactor the RXE migration object into semantic operations such as:

- `CHECKPOINT_CONTEXT` or the existing `FREEZE_CONTEXT(freeze=1)` to reach a safe point;
- `QUERY_CONTEXT_STATE` and/or `QUERY_QP_PRIVATE`/`QUERY_CQ_PRIVATE` for non-queryable logical state;
- `BEGIN_RESTORE` to bind a restore UUID, image version, feature set, and expected counts to a restore-mode ucontext/device;
- normal generic `RESTORE_PD/MR/CQ/QP/...` for object shell construction;
- `FINALIZE_QUEUE` or a batched `FINALIZE_CONTEXT` after queue VMAs and pages exist;
- `COMMIT_RESTORE` to atomically validate completion and thaw, or to make a following `FREEZE_CONTEXT(freeze=0)` legal;
- `ABORT_RESTORE` for deterministic rollback/cleanup.

The exact number of ioctls is secondary to the ordering contract. Prefer one batched finalizer per context to per-object piecemeal calls: it can check that every expected queue and object is present before modifying any live state.

### 5.3 Remove queue payloads from object restore UHW

The target ABI removes:

- SQ/RQ image output attributes from RXE `QUERY_QP`;
- CQE image output from RXE `QUERY_CQ`;
- `sq_image_bytes`, `rq_image_bytes`, `cqe_image_bytes`, and queue image tails from restore requests;
- `queue_inflight_capture()` and `queue_inflight_restore()` from the migration path;
- cursor seeding and queue blits in `rxe_qp_restore_wire_state()`, `rxe_qp_restore_inflight()`, and `rxe_restore_cq()`.

Keep forced SQ/RQ/CQ mmap offsets and queue creation geometry in shell creation. Move responder resources and other private state to the versioned private image/finalizer.

Because this is proof-of-concept UAPI, prefer a clean version bump and explicit rejection of old images over a long-lived dual format. If compatibility with already captured development images is required, implement the old format only behind a documented compatibility feature and remove it before upstreaming.

## 6. CRIU device-VMA memory support

The current CRIU path claims RXE uverbs VMAs with `HANDLE_DEVICE_VMA`, remaps them with `UPDATE_VMA_MAP`, sets `VMA_NO_PROT_WRITE`, and assumes the plugin restores their contents. Generic page restore also rejects pages belonging to non-private VMAs. That behavior must become an explicit policy rather than a global assumption for plugin VMAs.

### 6.1 Add an explicit VMA content-owner policy

Extend the plugin API (or add a narrowly scoped hook) so a device VMA can declare:

```text
DEVICE_VMA_CONTENT_PLUGIN       existing behavior
DEVICE_VMA_CONTENT_CRIU_EAGER  CRIU dumps/restores all mapping pages
DEVICE_VMA_CONTENT_NONE        remap only; content intentionally recreated
```

Only a plugin that has positively identified a supported RXE queue mapping may select `CRIU_EAGER`. Do not make arbitrary device mappings dumpable.

Record the policy and a plugin-provided stable mapping identity in the VMA image. Reject policy/image mismatches on restore.

### 6.2 Dump complete RXE queue mappings

For `CRIU_EAGER` mappings:

- force a full-page dump of the complete mapping, including the queue header and all slots;
- do not depend on soft-dirty bits, parent-image deduplication, lazy pages, or predump in the first implementation;
- copy while the owning RXE context is frozen and task threads are stopped;
- verify that the VMA length and `vm_pgoff` match a queue reported in the RXE object inventory;
- avoid emitting the same backing pages multiple times when VMAs are shared across forked processes or duplicated mappings.

Full mappings are intentionally preferred to an in-flight subspan. They eliminate uverbs attribute length limits, preserve exact slot placement and headers, and let the normal page-image machinery handle large rings.

### 6.3 Restore opted-in shared device pages

For `CRIU_EAGER` mappings:

- create the destination queue shell first so the forced `vm_pgoff` names real RXE backing pages;
- have `UPDATE_VMA_MAP` return a duplicate of the exact restored uverbs `struct file`, as the current RXE plugin already does;
- mmap the queue at the original virtual address, length, sharing flags, protection, and `vm_pgoff`;
- temporarily provide write access to the restorer even for an originally read-only mapping;
- allow page placement into this specifically opted-in shared device VMA;
- restore each canonical backing page set once and preserve aliases/sharing;
- restore original page protections and mapping flags afterward;
- fail before RXE finalization if a page is missing, duplicated inconsistently, short-written, or mapped to an unexpected queue.

The implementation will need targeted changes in the logic around `should_dump_entire_vma()`, `VMA_EXT_PLUGIN`/`VMA_NO_PROT_WRITE`, the non-private-VMA page rejection, and the restorer's assumption that `MAP_SHARED` file mappings are already current. These exceptions must be guarded by the new content policy.

### 6.4 Ordering callback

Add a post-memory device callback, or define an equivalent core barrier, that runs after:

- all restored processes have installed the RXE VMAs;
- CRIU has copied all opted-in queue pages;
- ordinary process memory is present;
- MRs have repinned their destination pages; and
- all RDMA objects in the context DAG exist.

The RXE plugin uses this barrier to invoke the kernel finalizer. `RESUME_DEVICES_LATE` may remain the final thaw hook, but it must become fail-closed: an error must abort restore rather than log a warning and continue with permanently parked QPs.

## 7. Linux RXE changes

### 7.1 Restore session and device state

Add a restore-session object associated with the RXE ucontext, and where necessary an RXE device-level registry keyed by an unguessable restore UUID. It should track:

- state: `NEW`, `BUILDING`, `FINALIZING`, `COMMITTED`, or `ABORTED`;
- schema/features negotiated at `BEGIN_RESTORE`;
- expected and observed object/mapping counts;
- restored object lookup keyed by stable image ID plus ufile handle/QPN cross-checks;
- ownership of a CRIU-created RXE device;
- whether private state and queue indices have been applied;
- one-shot commit/abort status.

Normal ucontexts and non-restore ioctls must not attach to incomplete restore state. Restored QPs remain `dp_frozen`. Finalization and commit are idempotent only where explicitly documented; a second conflicting request returns an error.

### 7.2 Shell-only object creation

Refactor `rxe_restore_cq()` and `rxe_restore_qp()` so they:

- validate generic attributes, caps, relationships, target handles, and exact wire-visible identity;
- allocate zeroed queue backing storage using kernel-validated geometry;
- reserve the requested mmap offsets and return mmap info;
- attach the object to its restore session;
- install QPs frozen;
- do not apply queue bytes, queue headers, queue cursors, replay position, responder-resource content, or runnable timer/task state.

The shell may store private image records in staging memory, but it must not apply them until finalization.

### 7.3 Finalize mapped queues

At `FINALIZE_CONTEXT`, locate every expected queue by the restore session's object records, not by trusting an arbitrary address. For each queue:

1. verify object type, VMA offset, allocation size, queue type, index mask, and element size;
2. validate both restored header indices against the mask and validate the distance/capacity invariant;
3. issue the necessary memory barriers after CRIU's last write;
4. for `QUEUE_TYPE_FROM_CLIENT` SQ/RQ/SRQ, set `q->index = buf->consumer_index`;
5. for `QUEUE_TYPE_TO_CLIENT` CQ, set `q->index = buf->producer_index`;
6. leave the userspace-owned counterpart untouched;
7. mark the queue finalized exactly once.

Do not normalize cursors to zero. Userspace may cache absolute masked cursors, and work/completion entries must remain in their original slots.

### 7.4 Apply private QP/CQ state

After queue validation, apply stable private state. Rebuild responder resources from tagged records and translate all references through restored MRs/objects. Reject invalid opcodes, PSNs, counts, resource spans, rkeys, VAs, and state combinations.

Define a replay algorithm rather than restoring transient execution structs:

- rewind requester processing to the restored SQ consumer when an unacknowledged RC window exists;
- reset each replayed WQE's internal DMA cursor from its stable WQE description;
- retain completer/responder PSN continuity and duplicate-response resources;
- re-arm an immediate or remaining-delay retry according to the chosen timer policy;
- do not start any task until commit.

The freeze path must guarantee that responder transient pointers and packet-local state are at a reconstructible safe point. If a state cannot be reconstructed, checkpoint must wait for a boundary or return a clear unsupported/busy error.

### 7.5 Commit and abort

Commit performs a final whole-context check and then enables QPs in an order that cannot expose a half-restored context. If full atomic enabling is impractical, keep a context-level frozen gate true while individual worker state is prepared, then clear the gate once.

Abort cancels staged timers/work, removes staged objects in reverse dependency order, releases mmap reservations, and deletes the RXE link only when this restore created and owns it. Never delete or reset a pre-existing administrator-owned RXE device.

### 7.6 Freeze correctness

Retain context-scoped freeze as the default snapshot primitive. It must:

- prevent new requester, completer, and responder work;
- drain worker tasks to a documented safe point;
- synchronize the receive/network path;
- account for or deliberately drop race-window packets under the RC replay rule;
- make all queue pages and private-state query results one consistent epoch;
- be reversible on dump failure.

Add a checkpoint generation number so queries and VMA capture can prove they belong to the same freeze epoch. Individual-QP freeze can be added later, but only with dependency tracking for shared CQs/SRQs and peer traffic.

## 8. RXE CRIU plugin changes

### 8.1 Dump side

The plugin should:

1. inventory RXE devices and their backing netdevices using RDMA netlink;
2. claim supported ucontexts and reject unsupported objects/features early;
3. freeze each context once, recording its generation;
4. query only non-memory private state and stable mapping metadata;
5. mark verified queue VMAs as `CRIU_EAGER` and associate them with object IDs;
6. let CRIU dump the full mapped pages;
7. emit versioned device, context, queue metadata, and private-state records;
8. thaw the source on failure and according to the requested final dump mode.

Remove the fixed local SQ/RQ/CQ image buffers and their current u16-attribute-size limitation. Keep local UAPI mirrors only until matching installed headers are available; add compile-time size/version checks while mirrors exist.

### 8.2 Restore side

The plugin should:

1. wait until CRIU has restored the target network namespace and backing netdevice;
2. resolve a user-configurable source-to-destination device/netdev mapping;
3. validate or create the RXE link through RDMA netlink;
4. begin the restore session and open the correct destination uverbs cdev;
5. allocate a restore-mode ucontext and restore the generic uobject DAG;
6. preserve forced queue `vm_pgoff` values and cache the exact context-bearing cdev file for `UPDATE_VMA_MAP`;
7. let CRIU install the VMAs and copy queue pages;
8. invoke context finalization after the post-memory barrier;
9. commit/thaw only after every restored process and context is ready;
10. roll back created links and sessions on any error.

Replace exact source ibdev-name lookup as the only behavior. Default to the source name when it is available, but permit an explicit mapping to a differently named destination RXE device/backing netdev. Names are operational identifiers; QPNs, keys, and queue offsets are restored protocol/mapping identity.

### 8.3 Multi-process coordination

Do not find context fds by repeatedly walking arbitrary restored `/proc/<pid>/fd` entries at the last moment. CRIU core/plugin coordination should retain stable references to restored uverbs file descriptions and contexts. A shared uverbs file must be finalized and thawed once, even when several restored fd tables or VMAs refer to it.

## 9. Detailed restore choreography

The required restore order is:

```text
restore net namespaces and backing netdevices
  -> resolve/create RXE ibdev(s)
  -> BEGIN_RESTORE per device/context
  -> open uverbs cdev and GET_CONTEXT(restore mode)
  -> restore PDs and other parents
  -> create CQ/QP queue shells with forced vm_pgoff
  -> restore MRs and remaining uobject DAG
  -> map SQ/RQ/SRQ/CQ VMAs using the context-bearing fd
  -> copy complete queue pages through CRIU memory restore
  -> restore original VMA protections
  -> FINALIZE_CONTEXT
       validate every mapping/header/geometry
       import kernel shadow indices
       apply CQ arm and QP private protocol state
       rebuild responder resources and retry state
       verify expected object counts and references
  -> global CRIU device-ready barrier
  -> COMMIT_RESTORE / thaw contexts
  -> release application tasks
```

CRIU must not interleave the final two steps such that one endpoint or one process runs while a shared local context is incomplete. Cross-host coordination remains the responsibility of the migration harness, but the plugin should expose clear `READY_TO_COMMIT` and `COMMITTED` points for its barrier protocol.

## 10. Alternative: queue pages inside the RXE state image

If review rejects CRIU support for opted-in shared device VMAs, the fallback is to include queue mappings in the RXE state image. That design must still obey the same phase separation:

- `RESTORE_QP/CQ` allocate shells only;
- queue images are chunked/streamed into restore-session staging storage, not appended to object UHW;
- after all objects and mappings exist, `FINALIZE_CONTEXT` copies the staged full pages into the destination queue allocation, imports kernel indices, and applies private state;
- mapping sharing and canonical ownership are represented explicitly;
- image size, chunk order, checksums, and resource limits are validated.

This fallback is more RXE-specific, duplicates CRIU's memory machinery, and needs its own large-image/chunking ABI. It should be selected only after a small prototype demonstrates that the narrowly opted-in CRIU page path cannot safely preserve these shared mappings.

Decision gate: prototype one CQ plus one QP using `CRIU_EAGER`, including forked aliases and read-only protection restoration. If it passes, proceed with the primary design and delete queue blobs from the RXE UAPI. If it cannot be made safe without broad generic-memory regressions, document the failure and implement the staged-image fallback.

## 11. Additional object and event work

### 11.1 Completion channels

Completion-channel support is P0 after the polling-CQ MVP because normal applications depend on it. Add:

- generic restore for the completion-channel uobject/fd;
- the CQ-to-channel relationship and user event context;
- restoration of `rxe_cq.notify`;
- a defined policy for unread completion events and event acknowledgement counters;
- ordering that creates the channel before its CQs and enables delivery only after commit.

Until complete, the existing dump precheck must be strict, not best-effort.

### 11.2 Async events

CRIU already has an async-event-fd reconstruction path. Validate it end to end for RXE, including shared fd descriptions, QP/CQ user handles, unread events, acknowledgement semantics, and no event delivery while born-frozen. If pending event contents cannot yet be preserved, reject contexts with pending events and document that limitation.

### 11.3 SRQ

Add generic `RESTORE_SRQ`, SRQ handle/limit/error/private state, the mapped SRQ RQ pages under the same VMA policy, forced offset, queue finalization, and QP-to-SRQ relationships. A QP using SRQ must never also expect a private RQ mapping.

### 11.4 AH and UD

Add a stable address-vector representation and `RESTORE_AH` before UD. UD then needs explicit loss semantics because the RC retransmission rule does not apply. Preserve qkey/QPN/AH references and test multicast separately.

### 11.5 MW, multicast, RDMA CM, and DMABUF

- MW needs key/index preservation and bind-state ordering after its MR.
- Multicast needs group membership recreation after port/GID readiness and before UD traffic.
- RDMA CM requires CM IDs, event channels, route/address state, and coordination with socket/network restore; it is a separate milestone.
- DMABUF/GPU Direct/peer-memory MRs require restoring the exporting device object and attachment before MR restore. Hard-reject them in the initial RXE milestone.
- Implicit ODP remains unsupported until its faultable address-space semantics and page population are specified.

## 12. Validation, compatibility, and security

All kernel entry points must treat image data as untrusted:

- use checked additions/multiplications for lengths and counts;
- cap object counts, ring sizes, responder resources, and total staged bytes by device capabilities and restore-session limits;
- validate reserved fields are zero and enum/opcode values are known;
- verify every handle, QPN, key, mmap offset, and object relation belongs to the same restore session/context;
- reject duplicate objects, overlapping mmap offsets, duplicate finalization, and references to normal contexts;
- never accept source pointers, kernel struct sizes, native padding, or host endianness as ABI;
- require the same architecture/ABI where queue WQE/CQE layout is architecture-dependent, unless an explicit conversion layer is added;
- require appropriate netns and RDMA administrative privilege for link creation and restore mode;
- keep restored contexts inaccessible to ordinary traffic until commit.

Record kernel, CRIU, plugin, and image feature versions in test logs. Kernel and CRIU branches must be co-versioned; a capability probe should fail at startup with a specific missing-operation/feature diagnostic.

The outstanding removal of `ib_safe_file_access` must not be treated as a silent prerequisite. Resolve the credential/safe-file-access model, or explicitly gate the proof of concept to a privileged trusted restore, before proposing the generic restore ABI upstream.

## 13. Implementation series

Each item should be a reviewable patch group with tests. Patch subjects are illustrative.

1. **Rebase and semantic reconciliation**
   - Establish matched Linux/CRIU integration branches from the v7.2 VFMIG bootstrap tips.
   - Port later RXE freeze, replay, CQ, responder, VMA-lifetime, and validation fixes by behavior.
   - Add a feature probe proving the paired builds agree before changing the image format.

2. **State audit and versioned schema**
   - Add the field classification described in section 4 as kernel documentation.
   - Define RXE device/context/private-state protobuf and UAPI records with feature negotiation.
   - Add malformed-image and cross-version tests first.

3. **RXE device inventory and creation**
   - Save the ibdev-to-netns/backing-netdev association.
   - Add CRIU RDMA netlink `NEWLINK`/lookup helpers and source-to-destination mapping options.
   - Implement collision policy and owned-link rollback.

4. **Restore sessions and fail-closed freeze**
   - Add begin/finalize/commit/abort state and restore UUIDs.
   - Make late resume errors fatal.
   - Add whole-context generation and completeness checks.

5. **CRIU device-VMA content policy**
   - Add the plugin policy and image fields.
   - Force complete eager dump only for verified RXE queue mappings.
   - Add canonical sharing identity and disable lazy/predump/parent shortcuts initially.

6. **Restore shared device pages**
   - Permit page restore into opted-in shared device VMAs.
   - Manage temporary write protection safely.
   - Preserve aliases and invoke the post-memory barrier.

7. **Shell-only RXE CQ/QP restore**
   - Retain forced mmap offsets and geometry.
   - Remove SQ/RQ/CQ images, cursor seeding, and responder-resource blits from object restore.
   - Version-bump and reject the old proof-of-concept image format.

8. **RXE finalizer and private state**
   - Import shadow queue indices after page restore.
   - Add stable QP/CQ private records and tagged responder resources.
   - Rebuild replay/timer state and commit atomically.

9. **Core correctness tests and RC coverage**
   - Land KUnit, CRIU zdtm-style tests, perftest scenarios, failure injection, and cross-host barrier tests.
   - Promote polling-CQ RC from experimental only after all acceptance criteria pass.

10. **Events and broader objects**
    - Completion channels and async events.
    - SRQ.
    - AH/UD, MW, multicast, RDMA CM, and DMABUF as separately reviewable milestones.

Keep mlx5 VFMIG changes separate where possible. Generic CRIU uobject DAG, plugin arbitration, VMA policy, device hooks, and restore-stage APIs should be shared; RXE private state and mlx5 VHCA/IOVA mechanics should remain driver-specific implementations.

## 14. Test plan

### 14.1 Kernel unit and probe tests

- queue-header import for every queue type, wrap position, empty/full boundary, and invalid cursor;
- geometry mismatch, short mapping, overlapping `vm_pgoff`, wrong object type/context, duplicate finalize, and unknown feature/version;
- responder-resource encode/decode for read, atomic, and flush records without kernel-layout dependence;
- restore-session state transitions, abort at every phase, and no worker execution before commit;
- source freeze/thaw idempotence and receive race-window behavior;
- exact QPN/key allocation collision behavior;
- large rings that exceeded the old u16 query-attribute limit.

### 14.2 CRIU local RXE tests

- context only, then PD, MR, CQ, and QP dependency ladder;
- empty rings and non-empty SQ/RQ/CQ rings;
- unread CQEs visible in the original order after restore;
- ring wraparound and maximum usable occupancy;
- original virtual addresses, `vm_pgoff`, protections, and sharing preserved;
- duplicated/forked VMAs restored once with coherent aliases;
- multiple ucontexts, processes, RXE ibdevs, and network namespaces;
- destination ibdev renamed through an explicit mapping;
- auto-created device, pre-existing compatible device, name collision, incompatible netdev, and rollback;
- intentional failures after link creation, each object type, VMA mmap, page copy, finalization, and commit;
- `--leave-running` and failed-dump source thaw.

### 14.3 In-flight protocol tests

Run with payload integrity checking, not only completion counts:

- RC SEND with and without immediate data, signaled and unsignaled;
- RDMA WRITE and READ across multi-packet messages;
- compare-and-swap and fetch-and-add;
- inline sends and ordinary SGEs;
- outstanding windows from 1 through device limits;
- retry, RNR, duplicate request/response, CQ near-full, and lost race-window packet cases;
- checkpoint at randomized protocol/task state points for repeated iterations;
- high-volume `ib_send_bw`, `ib_write_bw`, `ib_read_bw`, latency variants, and mixed operations.

### 14.4 Platform and integration tests

- x86-64 and ARM64 native restore; explicitly reject unsupported cross-architecture images;
- veth and physical backing netdevices where RXE supports them;
- same-host and cross-host restore with the migration harness's peer barrier;
- one endpoint restored and both endpoints restored;
- kernel/CRIU feature mismatch diagnostics;
- sanitizers, lockdep, KASAN, kmemleak, and repeated create/abort cycles;
- regression runs for mlx5 direct and mlx5 VFMIG plugins to prove the generic VMA changes are opt-in.

Unsupported-object tests must prove dump fails early with actionable messages for completion channels, SRQ, AH, UD/UC, MW, multicast, DMABUF, ODP, and RDMA-CM until each feature is promoted.

## 15. Open decisions to close during the prototype

1. Whether the new post-memory hook is a generic CRIU plugin hook or a stage in the existing device-resume API. It must carry fatal errors and deduplicate shared contexts.
2. Whether restore session scope is per ucontext or also has a device-level parent for coordinated multi-context commit. Use a device parent if contexts share any RXE state that can become externally visible during thaw.
3. The exact logical-timer policy: preserve remaining delay or perform immediate RC retry. Whichever is selected must preserve retry budgets and be documented as image semantics.
4. Whether an existing RXE device may be reused. Recommended default: create a dedicated device or require an explicit mapping to an empty, compatible device; never opportunistically inject state into a busy device.
5. How CRIU represents canonical shared backing pages. Reuse an existing shmem/page-owner abstraction if it can retain device VMA identity; otherwise add a plugin mapping ID.
6. Whether a `NEWLINK` restore UUID/mode attribute is required to prevent normal opens in the interval before `BEGIN_RESTORE`.
7. Whether packet dropping during context freeze is sufficiently bounded and observable for RC, or whether RXE must queue packets behind the frozen gate. Resolve with fault-injection tests before upstreaming.

None of these decisions changes the central ownership rule: queue pages are restored either by CRIU's opted-in memory path or by a separate staged device-image loader, and never by `rxe_restore_qp()`/`rxe_restore_cq()`.

## 16. Acceptance criteria

The RXE restore path is complete for its advertised v1 support when all of the following hold:

- a matched kernel/CRIU pair restores an in-flight RC workload repeatedly with byte-correct results and no application changes;
- complete SQ/RQ/CQ mappings are restored outside QP/CQ object-creation methods;
- queue shadow indices are imported only after memory restore and validated against the mapped headers;
- peer-visible QPNs, keys, PSNs, and responder replay behavior remain correct;
- device creation/mapping works across net namespaces and differently named destination devices, with deterministic rollback;
- no restored traffic runs before the global commit barrier;
- malformed, incompatible, colliding, incomplete, and unsupported images fail before task release;
- source failure paths reliably thaw the original contexts;
- no raw kernel structure is part of the persistent ABI;
- the support matrix, feature probe, image version, operational prerequisites, and known exclusions are documented;
- generic CRIU changes leave mlx5 and unrelated device VMAs unchanged unless their plugins explicitly opt in.
