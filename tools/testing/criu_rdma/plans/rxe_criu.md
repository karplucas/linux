# RXE vHCA image and uobject restore design

Status: proposed, pre-implementation alignment document

Last reviewed: 2026-09-08

## 1. Goal and scope

This document defines where RXE checkpoint state should live and how CRIU
should move it. The central design question is the split between:

- an RXE-owned device image, analogous to the firmware vHCA image used by
  mlx5 VFMIG; and
- CRIU's generic replay of individual RDMA uobjects such as ucontexts, PDs,
  MRs, CQs, and QPs.

RXE has no firmware image to save. If RXE exposes a vHCA-style image, the
kernel driver must serialize its own private software state into that image.
The image is opaque to CRIU but has a structured, RXE-internal binary format.

Queue placement is a related but separable decision. RXE allocates SQ, RQ,
SRQ, and CQ storage in the kernel and maps it to userspace. The design must
decide whether those bytes remain in per-object uverbs payloads, are handled
as process memory by CRIU, or are included in the RXE device image.

This document does not redefine which transports or object combinations are
checkpointable. That requires a separate freeze-safe-state and feature audit.
The refactor described here must not introduce functional regressions in the
set of workloads supported by the existing implementation.

## 2. Background and terminology

### 2.1 Current split

The existing CRIU RDMA work has two restoration layers:

1. Generic uobject replay reconstructs the kernel RDMA object graph. It
   creates ucontexts, PDs, MRs, CQs, QPs, and their relationships using
   `RESTORE_*` operations.
2. A provider-specific plugin supplies state that generic RDMA queries do not
   expose. For the RXE proof of concept, this includes transport state and,
   historically, queue entries copied through QP/CQ uverbs payloads.

For mlx5 VFMIG there is a third source of state: firmware supplies a vHCA
blob. CRIU streams that blob without understanding its contents, while
`RESTORE_*` rebuilds the Linux RDMA stack objects that refer to the restored
firmware objects.

The proposed RXE model mirrors that division even though both sides happen to
be software:

- `RXE_SAVE_VHCA` and `RXE_LOAD_VHCA` move RXE-private execution state;
- `RESTORE_*` performs meaningful RDMA-stack work: allocation, identity,
  relationships, queue geometry, mmap identity, and generic object state;
- `RESTORE_*` does not transport or seed the state owned by the RXE vHCA
  image.

### 2.2 Remove the `EXCLUSIVE` versus `SHAREABLE` plugin policy

CRIU currently asks each RDMA plugin to declare the entire provider
`EXCLUSIVE` or `SHAREABLE`. The cross-tree precheck uses that declaration when
an in-checkpoint process and an outside process both hold contexts on the same
ibdev:

- mlx5 VFMIG declares `EXCLUSIVE` because saving and loading a firmware vHCA
  affects the complete VF; and
- RXE declares `SHAREABLE` because its proof of concept freezes and replays
  selected ucontexts and uobjects rather than replacing device-wide state.

That enum is a proxy for snapshot granularity, not an inherent provider
property. Moving RXE state from per-uobject payloads into a vHCA image
invalidates the reason RXE is marked `SHAREABLE`, but merely changing RXE to
`EXCLUSIVE` would preserve the wrong abstraction. The answer depends on what
one save operation captures, not on the driver name.

The vHCA refactor should therefore remove `CR_RDMA_SHARING_EXCLUSIVE`,
`CR_RDMA_SHARING_SHAREABLE`, and the plugin declaration hook. Coverage should
instead be derived from the actual save operation:

- mlx5 `SAVE_VHCA_STATE` captures a complete VF vHCA, so its save path must
  ensure that the checkpoint covers every context affected by that operation;
- RXE `SAVE_VHCA` builds an image from an explicit set of selected contexts,
  so an unrelated context on the source RXE device remains outside the image;
  and
- a save path rejects dependencies that cross its selected image boundary.

There is no plugin policy lookup. The RXE image builder defines and validates
its capture boundary. CRIU groups one selected RXE context set into one vHCA
blob and verifies that all selected contexts use the same source RXE device.

This removal is a fundamental part of the design. It replaces provider-wide
policy with image-scope semantics and prevents the generic CRIU RDMA layer
from encoding assumptions specific to either mlx5 firmware images or the old
RXE per-object implementation.

### 2.3 RXE vHCA scope: a context set loaded into a fresh device

One RXE RDMA device (`struct rxe_dev`) corresponds to one RXE link backed by
one netdevice in one network namespace. RXE contexts remain associated with
that underlying `ib_device`, but the source device may also have contexts that
are not part of the checkpoint.

For this design, an RXE vHCA is the set of selected context images from one
source RXE `ib_device`, plus the RXE-private state belonging to their objects.
It is not a byte copy of `struct rxe_dev`, and it does not include unrelated
source contexts.

The image is loaded only into a freshly created destination RXE `ib_device`:

```text
source rxe0
  selected context A  --+
  selected context B  --+--> one RXE vHCA image
  unrelated context C        not included

fresh destination rxe0
  NEWLINK
  RXE_LOAD_VHCA(contexts A and B)
  RESTORE_* binds new uobjects to context A and B image records
```

The fresh destination avoids merging restored QPN, key, mmap, and allocator
identity into a live RXE device. It gives the loaded context set a
hardware-like device boundary on the destination without requiring exclusive
ownership of the source device.

The goal of mocking hardware is the save/load contract: CRIU handles one
opaque driver image before replaying Linux uobjects. RXE does not need to copy
mlx5's physical VF capture granularity when the software driver can define the
vHCA as an explicit context set.

### 2.4 Queue indices and `struct rxe_queue::index`

An RXE queue has indices in two locations:

- the mapped queue header contains `producer_index` and `consumer_index`;
- `struct rxe_queue::index` is a kernel-private cached index.

For a `QUEUE_TYPE_FROM_CLIENT` queue, userspace owns the producer index and
the kernel's `queue->index` tracks the consumer. For a
`QUEUE_TYPE_TO_CLIENT` queue, userspace owns the consumer index and
`queue->index` tracks the producer.

Earlier drafts called `queue->index` a "shadow index" without defining it.
This document uses the field name directly. Wherever queue bytes are restored,
the design must also restore or reconstruct `queue->index` before the queue is
allowed to run.

## 3. Architectural alternatives

### 3.1 Per-uobject replay only

Keep all RXE-private state in `QUERY_QP`, `QUERY_CQ`, `RESTORE_QP`, and
`RESTORE_CQ` payloads. Queue entries can continue to travel through those
operations.

Advantages:

- smallest change from the proof of concept;
- object state is naturally adjacent to the object that consumes it; and
- no new device-image control path is required.

Disadvantages:

- RXE looks unlike the hardware migration model;
- per-object UHW becomes a transport for potentially large queue data;
- image size is constrained by uverbs attribute mechanics;
- device-private state is fragmented across CRIU object records; and
- CRIU must understand more RXE-specific sequencing.

This remains a valid implementation, but it does not accomplish the vHCA
image refactor.

### 3.2 RXE vHCA image plus uobject replay

Add a driver-generated RXE image and retain generic `RESTORE_*` for the Linux
RDMA object graph.

Advantages:

- matches the mlx5 separation between device state and Linux uobjects;
- gives the RXE driver ownership of its internal serialization format;
- lets CRIU stream one opaque blob instead of interpreting private QP/CQ
  fields; and
- provides one place for state that is neither queryable through netlink nor
  appropriate for a generic restore verb.

Disadvantages:

- RXE must implement serialization that firmware provides for mlx5;
- records in the blob must be associated with later `RESTORE_*` calls; and
- state cannot be installed directly at `LOAD` time because most destination
  uobjects do not exist yet.

This is the target architecture for the prototype.

### 3.3 Device image creates all uobjects

`RXE_LOAD_VHCA` could create the destination ucontexts, PDs, MRs, CQs, and
QPs itself, eliminating individual replay.

This most closely resembles firmware object restoration, but it conflicts
with CRIU's need to reproduce uverbs file ownership, userspace handles,
cross-process sharing, event files, and restored process address spaces. The
kernel image does not have enough CRIU context to reconstruct those
relationships cleanly. This alternative is not recommended.

## 4. Proposed ownership split

The inventory below distinguishes existing ownership from the proposed
vHCA-based ownership. "Generic image" means CRIU's normal RDMA images;
"RXE image" means the opaque byte stream produced and consumed by RXE.

- **RXE link name, backing netdevice, and namespace placement:** currently
  discovered by the plugin or orchestrator. Keep them in a CRIU/orchestrator
  topology descriptor and create the link with `NEWLINK`; they are destination
  topology policy, not private execution state.
- **Uverbs file and ucontext ownership:** stays in generic CRIU RDMA images
  because CRIU owns process and file relationships.
- **PD, MR, CQ, and QP handles and dependency graph:** stays in the generic
  uobject image, which recreates the userspace-visible object graph.
- **QPN, lkey, and rkey identity:** remains input to per-object restore, with
  an association to the corresponding RXE-image record. The generic operation
  requests the visible identity; RXE associates it with private state.
- **Queue capacity, SGE limits, CQ size, and mmap offset:** remains
  `RESTORE_*` input because it determines allocation and VMA replay.
- **Requester, completer, and responder execution state:** moves from RXE QP
  UHW into an RXE-image QP record. This is analogous to a hardware QP context.
- **CQ notification state:** moves from RXE CQ UHW into an RXE-image CQ record.
- **Responder read/atomic resources:** moves from a raw RXE resource payload
  to explicit RXE-image resource records, avoiding native structures in the
  uverbs interface.
- **SQ/RQ/SRQ/CQ bytes and ring-header indices:** uses one of the alternatives
  in section 6. The original proof of concept put these in per-object UHW;
  remove them only when another owner exists.
- **`struct rxe_queue::index`:** currently seeded from per-object cursor
  fields. Reconstruct it from the selected queue representation before thaw.
- **Kernel pointers, locks, task/work objects, sk_buffs, and timer internals:**
  continue to be reconstructed and are never persistent identity.

"Kernel addresses" in this design specifically means pointer-valued fields
such as MR pointers, current WQE pointers, resource pointers, skb pointers,
work-item links, and timer internals. Virtual addresses and IOVAs registered by
the application are different: they are part of the application's restored
address-space and MR contract and may need to be preserved.

## 5. RXE vHCA image format

### 5.1 Ownership and transport

The byte stream is an RXE kernel-driver image, not a CRIU protobuf schema.
CRIU treats it the same way the VFMIG plugin treats a firmware blob:

1. request a save stream from the driver;
2. drain it to a blob file in the CRIU image directory;
3. record only the blob's association with the CRIU RDMA device entry; and
4. feed the exact stream to the destination driver during load.

Because RXE has no firmware payload, the driver constructs the stream from a
top-level header followed by typed sections. The structures below define the
shape to prototype; exact fields should be finalized only after the state
inventory is checked against the RXE implementation.

```c
struct rxe_vhca_image_header {
	__u32 magic;
	__u32 image_length;
	__u32 record_count;
	__u32 flags;
	__u8  device_uuid[16];
};

struct rxe_vhca_record_header {
	__u16 type;
	__u16 flags;
	__u32 length;
	__u64 object_id;
};
```

Every record is `length` bytes and begins with `rxe_vhca_record_header`.
Candidate record types are:

```text
RXE_VHCA_RECORD_DEVICE
RXE_VHCA_RECORD_CONTEXT
RXE_VHCA_RECORD_QP
RXE_VHCA_RECORD_CQ
RXE_VHCA_RECORD_RESP_RESOURCE
RXE_VHCA_RECORD_QUEUE       if queue alternative 3 is selected
```

`object_id` is an image-local association key. It is not a destination pool
index or kernel pointer. A context record contains a header describing its
number of child records. QP and CQ records carry the context/object key needed
to associate them with generic `RESTORE_QP/CQ` calls.

The driver image must use explicit fixed-width scalar fields. It must not copy
`struct rxe_dev`, `struct rxe_qp`, `struct rxe_cq`, `struct resp_res`, or any
other native kernel structure wholesale. CRIU does not parse the records; the
RXE save and load implementations do.

This is a greenfield format. Compatibility machinery and multiple schema
versions are intentionally deferred while the layout is being designed.

### 5.2 Device and context identity

There is one device UUID, not one UUID per context. Its purpose is to identify
the vHCA image to the orchestrator in the same way that the VFMIG UUID
identifies a VF-backed vHCA. Contexts use image-local numeric IDs beneath that
device.

The UUID, if used, identifies the aggregate RXE vHCA image and its selected
context set. It can be assigned by the orchestrator and copied into the driver
image. If explicit source-to-destination mapping is sufficient, the prototype
may instead use a CRIU image ID and omit a persistent UUID. A UUID is not an
authorization token and is not needed merely to make kernel cleanup work.

The RXE image should not contain source netns IDs, destination ifindices, or
expected MAC/MTU/GID values as authoritative restore input:

- network namespace IDs are local to a CRIU image and have no cross-host
  meaning by themselves;
- destination namespace and backing-netdevice placement are established by
  CRIU and the orchestrator before RXE creation; and
- MAC, MTU, port, and GID policy belongs to destination topology validation.

A small CRIU-side device descriptor may record the source RXE name, source
netdevice relationship, CRIU network-namespace image ID, and vHCA blob name.
On restore, CRIU translates the namespace image ID to the restored namespace,
and an operator/orchestrator mapping selects the destination netdevice. Those
fields do not need to be duplicated inside the opaque RXE image.

Object counts belong in record headers when required to parse the byte stream.
They should not be promises supplied independently by the orchestrator.

### 5.3 QP, CQ, and responder records

An RXE QP record should contain the non-queryable RXE execution state needed
to resume the same logical QP, including the requester, completer, and
responder PSNs, MSN/SSN state, remaining retry budgets, responder opcode and
acknowledgment state, and references to responder-resource records.

Responder resources need tagged representations for read, atomic,
atomic-write, and flush operations. Pointer fields are replaced with stable
object references, rkeys, addresses, lengths, offsets, and protocol state.

An RXE CQ record contains notification/arming state and any other CQ state not
represented by the queue bytes or generic CQ attributes.

### 5.4 Timers and deferred work

RXE cannot copy timer or workqueue objects. The image should carry only the
logical state necessary to restart them. The prototype must choose and test
one retry-timer policy:

- save the remaining relative delay and restart the timer with that delay; or
- preserve the remaining retry budget but schedule an immediate retry after
  thaw.

Absolute jiffies values, hrtimer internals, task objects, and workqueue links
are never stored. The choice is part of RXE image semantics and should be made
after examining the requester/completer timeout paths; it is not hidden under
a generic "logical time" invariant.

## 6. Queue-memory alternatives

Queue handling is independent enough to prototype separately from the vHCA
control path. Removing queue bytes from `QUERY/RESTORE_QP/CQ` is correct only
after one of the other paths owns them.

### 6.1 Alternative 1: keep queues in per-object UHW

Continue copying queue headers and entries through the RXE query and restore
uverbs.

This recognizes that RXE kernel-backed queues differ from hardware providers.
It is the least disruptive option, but large memory payloads remain attached
to per-object control operations and the result is least like the hardware
model.

### 6.2 Alternative 2: treat queues as process memory in CRIU

The RXE uverbs mapping contains ordinary kernel RAM, but CRIU classifies it as
a plugin-owned device VMA and normally saves only its mapping metadata. To
make CRIU own the bytes, CRIU must positively identify an SQ/RQ/SRQ/CQ mapping
and save every page while the context is frozen.

"Explicit opt-in" means exactly this: the RXE plugin identifies one VMA by
its uverbs file, offset, size, and owning RDMA object, and tells CRIU that this
specific mapping is safe for memory copying. It must not make all device VMAs
copyable because other mappings may be MMIO or have read/write side effects.

There are two ways to implement the restore half:

- Extend CRIU's device-VMA handling so it maps the restored RXE queue first
  and then writes the saved pages into that mapping. This requires CRIU memory
  and restorer changes and probably retains PIE coordination.
- Save the bytes into anonymous or memfd-backed memory first, then give RXE a
  kernel API that replaces/adopts that backing when recreating the queue.
  A normal `mmap(MAP_FIXED)` of the RXE queue would discard the temporary
  mapping, so preserving its contents requires an explicit RXE/user-memory
  queue interface rather than only `HANDLE_DEVICE_VMA` and
  `UPDATE_VMA_MAP`.

This alternative aligns queue bytes with the mlx5 model, where queues are
userspace memory, but it expands CRIU's special memory-mapping machinery. It
does not obviously eliminate the PIE hooks unless RXE queues are redesigned
to use user-provided backing.

### 6.3 Alternative 3: include queues in the RXE vHCA image

Add a queue record containing role, owner object ID, geometry, header, and
complete queue allocation. `RXE_LOAD_VHCA` stages these records. After
`RESTORE_QP/CQ` allocates the destination queues, RXE copies the saved bytes
into the allocations and restores `queue->index` before thaw.

Advantages:

- CRIU streams one device blob and does not need to understand RXE VMAs;
- RXE owns both its private execution state and its kernel-owned queue memory;
- the RXE plugin can potentially drop queue-specific PIE hooks; and
- queue size is not constrained by a uverbs attribute.

Disadvantages:

- memory visible in the process is now transported as kernel device state;
- the kernel must stage a potentially large image until objects exist; and
- shared/aliased mappings still need correct VMA reconstruction even though
  their bytes come from the vHCA image.

This is likely the best first prototype if minimizing CRIU and PIE complexity
is the priority.

### 6.4 Alternative 4: make RXE queues user-backed

Change RXE queue creation so userspace supplies the backing memory, similar to
hardware providers. CRIU would then save it through the ordinary process
memory path without device-VMA exceptions.

This produces the cleanest long-term memory ownership model, but it is a much
larger RXE ABI and implementation change. It should be considered separately
from the vHCA-image prototype.

### 6.5 Prototype decision

Prototype alternatives 2 and 3 far enough to answer two concrete questions:

1. Can CRIU restore an RXE queue mapping without adding more PIE hooks or a
   broad device-memory exception?
2. Can an RXE image containing realistic queue sizes be streamed and staged
   without an unacceptable kernel-memory or implementation cost?

Until that comparison is complete, the vHCA format should allow a queue
record type but the implementation should not assume it is mandatory.

## 7. Kernel control API

### 7.1 Device creation is separate from image load

The proposed happy path uses two interfaces:

```text
RDMA_NLDEV_CMD_NEWLINK(name, type="rxe", backing_netdev)
RXE_LOAD_VHCA(destination RXE device, image stream)
```

`NEWLINK` is used only to create the RXE link and bind it to a netdevice. It
does not carry the checkpoint image. This reuses existing namespace, naming,
link notification, collision, and `DELLINK` behavior.

`RXE_LOAD_VHCA` is a separate driver operation on the newly created RXE
device. It accepts a streamed blob and stages the RXE records needed by later
uobject restore. This is the lower-level model used by the rest of this
document.

### 7.2 Alternatives for creating and selecting the device

**Existing `NEWLINK`, followed by `RXE_LOAD_VHCA`** is preferred because
topology and state loading remain separate. CRIU can enter the restored
network namespace, resolve the restored backing netdevice, create `rxe0`, and
then open the resulting device for load.

**A new netlink command that also loads the image** would provide one control
plane, but netlink is a poor transport for a large byte stream and would need
chunk sequencing and abandoned-load cleanup. It also mixes link topology with
process checkpoint state. There is no clear advantage for the prototype.

**An `RXE_LOAD_VHCA` ioctl that implicitly creates the RXE link** would need a
global RXE control device because there is no per-device fd before creation.
It would duplicate netlink handling for namespace selection, netdevice lookup,
naming, notifications, and deletion.

**Loading into an already active RXE device** avoids link creation, but
requires merge and collision rules for QPNs, keys, handles, mmap offsets, and
allocators. The target design instead loads the selected source context set
into a fresh destination RXE device.

### 7.3 Save and load stream shape

The control API should follow the VFMIG streaming pattern rather than placing
the blob in an ioctl argument. On save, CRIU creates an RXE image builder for
one source device and associates each selected ucontext with that builder.
Sealing the builder produces one readable stream containing the context set.
The exact add-context call is part of the control-endpoint prototype: it may
accept a uverbs fd, or a uverbs method may append its current context to a
device-image handle.

```c
struct rxe_vhca_save_state {
	__u32 flags;
	__s32 save_fd;
};

struct rxe_vhca_load_state {
	__u32 flags;
	__s32 load_fd;
};
```

`RXE_SAVE_VHCA` returns a readable anonymous fd. CRIU drains it until EOF.
`RXE_LOAD_VHCA` returns a writable anonymous fd. CRIU writes the saved stream
and closes the fd to make the complete image available to the RXE device.

The exact control endpoint remains a prototype decision:

- a driver uverbs method on an RXE ucontext naturally identifies the RDMA
  device but makes a device-wide operation depend on a process context; or
- a small RXE migration control cdev gives device-level save/load semantics
  similar to mlx5 VFMIG but introduces a new device node.

The prototype should implement the smallest endpoint that can demonstrate
streaming, device association, and later uobject binding before fixing the
long-term UAPI.

### 7.4 Association with `RESTORE_*`

`RXE_LOAD_VHCA` cannot fully apply QP/CQ records immediately because their
destination objects do not yet exist. The loaded image is held in an RXE
restore object associated with the fresh destination device. Its context
records define which restore-mode ucontexts may subsequently be created.

Each relevant generic restore operation supplies an image-local object ID:

```text
RESTORE_QP(..., rxe_vhca_object_id)
RESTORE_CQ(..., rxe_vhca_object_id)
```

The RXE callback uses that ID to bind the newly initialized RDMA object to its
staged private record. The ID may be carried in provider UHW initially; CRIU
does not interpret the referenced record.

No context UUID is required. Context IDs in the image plus the loaded device
image/session are sufficient.

## 8. Restore and dump flow

### 8.1 Dump

```text
CRIU stops application tasks
  -> freeze the RXE device/datapath using the existing freeze mechanism
  -> RXE_SAVE_VHCA produces the RXE-private byte stream
  -> CRIU stores the stream as an opaque blob
  -> CRIU records generic uverbs files and the uobject dependency graph
  -> CRIU saves normal process memory
  -> save queue bytes according to the selected queue alternative
  -> preserve the existing success/failure/--leave-running thaw behavior
```

This refactor should not change CRIU's existing source-thaw semantics. It only
changes where RXE state is serialized.

### 8.2 Restore

```text
restore the destination network namespace and backing netdevice
  -> NEWLINK creates the named RXE device
  -> open the RXE migration control endpoint
  -> RXE_LOAD_VHCA streams and stages the RXE device image
  -> restore uverbs files and ucontexts
  -> RESTORE_PD/MR/CQ/QP recreates and associates the uobject graph
  -> restore process memory and queue bytes according to the selected option
  -> DATAPATH_FREEZE(false) validates completeness, applies remaining staged
     RXE state, reconstructs queue->index, and starts the datapath
  -> release application tasks
```

`DATAPATH_FREEZE(false)` performs any RXE state installation that must wait
until after queue, object, and memory restoration. If validation fails, the
datapath remains frozen and ordinary CRIU cleanup closes the new uverbs files
and deletes a link created for the failed restore.

This is staged failure containment, not a claim that existing uverbs restore
is an atomic transaction. Individual `RESTORE_*` calls publish kernel objects,
and the initial design does not require rollback of every prior operation.

The thaw must occur after CRIU has restored any memory that RXE can access,
including registered MR pages and, for queue alternative 2, the queue VMAs.

## 9. CRIU integration

The mlx5 VFMIG plugin already contains generic mechanics that should move into
CRIU's RDMA layer rather than be copied into a new RXE plugin:

- drain a driver-provided save fd into an opaque blob file;
- stream a blob file into a driver-provided load fd;
- associate one device blob with one CRIU RDMA device entry;
- ensure a shared device is saved and loaded once even when several uverbs
  files refer to it; and
- order device load before per-uobject restore.

The same refactor removes the static plugin sharing declaration. The RXE image
builder receives the selected source contexts explicitly; the mlx5 save path
continues to enforce the larger capture boundary of its firmware operation.

Provider hooks would supply the operations needed to open save/load streams
and the small destination-device descriptor. CRIU core would own file I/O,
image-directory naming, deduplication, and restore ordering.

The resulting checkpoint would conceptually contain:

```text
rdma device metadata       source device ID, namespace-image relationship,
                           backing-netdevice relationship, blob name/length
RXE vHCA blob              opaque RXE kernel byte stream
uverbsfd image             process fd and ucontext ownership
rdma_uobj image            generic PD/MR/CQ/QP graph and restore identities
pages/pagemap images       normal process memory; queue pages only for option 2
```

If queue alternative 3 is selected, RXE queue VMAs still need to be recreated
at their saved addresses and offsets, but the plugin need not save or write
their contents. This is the path most likely to reduce or remove RXE-specific
PIE hooks.

## 10. Initial support boundary

The first prototype should be bounded by complications introduced by the
vHCA/uobject split itself:

- require all selected source contexts to belong to one RXE `ib_device`;
- require a fresh destination RXE device with no pre-existing ucontexts;
- require an explicitly mapped destination network namespace and backing
  netdevice before `NEWLINK`;
- preserve source-visible QPNs, keys, uverbs handles, relationships, and mmap
  offsets through generic restore;
- associate every restored QP/CQ with exactly one record in the loaded RXE
  image;
- keep the datapath frozen until device state, uobjects, mappings, and process
  memory are ready;
- choose and implement one queue-memory alternative end to end; and
- introduce no functional regressions relative to the workloads already
  supported by the RXE CRIU implementation.

Transport and object eligibility, packet-race analysis, completion-event
semantics, and a full freeze-safe-state audit should be documented and tested
as a separate effort unless this refactor changes them directly.

## 11. Concrete implementation series

Each step should be a focused, buildable commit and should include its
corresponding probe or unit test where practical.

1. **RXE image structures and codec.** Add internal headers, record walking,
   sizing, and encode/decode helpers. Add KUnit coverage using synthetic
   device/context/QP/CQ/resource records. No UAPI yet.
2. **Save stream.** Add an RXE image builder, attach selected frozen contexts
   from one source RXE device, and expose the sealed image through a readable
   anonymous fd.
3. **Load stream.** Add `RXE_LOAD_VHCA`, accept the byte stream through a
   writable anonymous fd, and retain parsed records on the destination RXE
   device while frozen.
4. **Device identity and creation probe.** Extend `rdma_test_agent` to create
   an RXE link with `NEWLINK`, open the control endpoint, save a minimal image,
   delete/recreate the link, and load it.
5. **Uobject association.** Add the image-local object ID to RXE's
   `RESTORE_QP/CQ` provider input and bind restored objects to loaded QP/CQ
   records. Keep existing private state transport until association tests pass.
6. **Move private state into the image.** Move QP/CQ execution state and
   responder resources from per-object query/UHW into explicit image records.
   Remove each old field only after its image replacement is exercised.
7. **Queue prototype.** Implement either CRIU-owned queue memory or RXE queue
   records as a separate series. Do not couple the initial save/load stream to
   an unproven queue choice.
8. **Thaw integration.** Make `DATAPATH_FREEZE(false)` verify that all loaded
   records are associated, restore/reconstruct `queue->index`, apply remaining
   private state, recreate logical timer/work state, and resume.
9. **Common CRIU vHCA streaming and scope.** Extract save-fd/load-fd blob
   management and per-device deduplication from the mlx5 VFMIG plugin into
   CRIU's RDMA code. Remove the `EXCLUSIVE`/`SHAREABLE` plugin contract and
   let each image builder enforce its actual capture boundary.
10. **RXE CRIU integration.** Add provider callbacks for save/load and device
    selection, then remove obsolete RXE queue-image and PIE code according to
    the selected queue design.

## 12. Test plan

### 12.1 Kernel codec tests

KUnit should build representative records, serialize them, parse them back,
and compare all logical fields. Include multiple contexts, multiple QPs/CQs,
responder-resource types, and the selected queue representation.

### 12.2 `rdma_test_agent` kernel-interface ladder

Extend `rdma_test_agent` with explicit commands so failures can be localized
without running a full CRIU restore:

1. create/delete a named RXE link through `NEWLINK`;
2. freeze and `SAVE_VHCA`, then inspect record headers;
3. create a fresh RXE link and `LOAD_VHCA`;
4. restore one context only;
5. add a PD, then an MR;
6. add a CQ and associate its RXE image record;
7. add an RC QP and associate its RXE image record;
8. restore queue bytes through the selected alternative;
9. thaw and verify `queue->index` against the mapped header; and
10. exercise one in-flight send, receive, read, and retry case already
    supported by the existing implementation.

Each step should have a standalone command and machine-readable success/fail
result so it can be used by kernel CI and the CRIU integration scripts.

### 12.3 CRIU integration tests

- one process and one RXE device;
- two selected uverbs contexts on the same source RXE device, proving one vHCA
  blob and no context UUIDs;
- one outside-checkpoint context on that source device, proving it is excluded
  without relying on a provider-wide `SHAREABLE` declaration;
- load into a fresh destination device and reject load after a normal
  destination ucontext has already been created;
- CQ and QP dependency restoration through `rdma_uobj.img`;
- queue mapping at the original virtual address and mmap offset;
- source/destination netdevice names that differ through explicit mapping;
- failure before thaw leaves the destination datapath frozen; and
- regression execution of the existing RXE CRIU test set.

## 13. Open decisions for the prototype

1. What kernel API adds each selected source ucontext to an RXE vHCA image
   builder while proving that all selected contexts belong to one device?
2. Should `RXE_SAVE/LOAD_VHCA` live on a small migration cdev or as driver
   uverbs methods?
3. Which queue path wins the prototype comparison: CRIU-owned device-VMA
   pages or queue records inside the RXE image?
4. If CRIU owns queue bytes, can the implementation remove PIE hooks, or does
   it require more restorer-specific mapping work?
5. If the image owns queue bytes, what maximum staged size and streaming
   strategy are acceptable?
6. What image-local object key should generic `RESTORE_QP/CQ` pass to RXE?
7. Should a device UUID be mandatory, or is an explicit CRIU/orchestrator
   source-to-destination mapping sufficient for RXE?
8. Should retry timers restart with a saved relative delay or immediately
   with the saved remaining retry budget?

## 14. Acceptance criteria

The design is ready for implementation when the prototype demonstrates that:

- RXE produces and consumes an opaque, driver-owned vHCA byte stream without
  firmware involvement;
- the device image contains RXE-private execution state while generic
  `RESTORE_*` remains responsible for meaningful Linux RDMA uobject creation;
- `NEWLINK` is used only for RXE link creation/binding and
  `RXE_LOAD_VHCA` loads the checkpoint separately;
- one device identity is sufficient and no per-context UUID is introduced;
- CRIU no longer exposes a provider-wide `EXCLUSIVE`/`SHAREABLE` declaration;
- an RXE image is an explicit set of contexts from one source device and is
  loaded only into a fresh destination RXE device;
- every loaded QP/CQ record is associated with the correct restored uobject;
- one queue-memory alternative is implemented without queue state being
  restored twice;
- `struct rxe_queue::index` and mapped queue indices agree before thaw;
- thaw is the single transition that makes the restored RXE datapath runnable;
- CRIU's common RDMA layer, rather than an RXE-specific copy, owns opaque
  save/load stream transport; and
- the refactor passes the existing RXE CRIU tests without functional
  regressions.

## 15. Reference points

- `example.txt`: design style and the mlx5 split between a device/FW image and
  per-ucontext uverbs state.
- `criu/plugins/rdma/mlx5_sriov_vfmig/vf_image.c`: current save-fd-to-blob
  streaming and per-device image management.
- `criu/plugins/rdma/mlx5_sriov_vfmig/vfmig_restore.c`: current load stream and
  restore ordering.
- `criu/images/rdma_uobj.proto`: generic uobject graph and provider
  `plugin_blob` boundary.
- `linux/drivers/infiniband/sw/rxe/rxe_queue.h`: mapped queue representation
  and `struct rxe_queue::index`.
- `linux/drivers/infiniband/sw/rxe/rxe_migrate.c`: existing RXE private query
  and freeze prototype.
