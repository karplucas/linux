# RXE vHCA image design

Status: proposed

## 1. Problem

CRIU currently restores RDMA state one uobject at a time. Generic RESTORE
operations recreate ucontexts, PDs, MRs, CQs, QPs, and their relationships.
RXE-specific state is passed through provider UHW attached to those objects.

mlx5 VFMIG has a cleaner split:

- firmware saves and loads one opaque vHCA image; and
- RESTORE operations recreate the Linux RDMA objects associated with it.

RXE has no firmware, so the driver must build the equivalent image. The goal
is to follow the mlx5 model: RESTORE operations still allocate and initialize
real RDMA objects, but they do not transport RXE-private device state.

Queue storage is related but separate. RXE allocates SQ, RQ, SRQ, and CQ
memory in the kernel and maps it to userspace. Those bytes need one owner:
uobject UHW, CRIU memory images, or the RXE vHCA image.

This refactor must not reduce the functionality supported by the existing RXE
checkpoint/restore implementation.

## 2. Proposed model

### 2.1 An RXE vHCA is a set of context images

An RXE vHCA contains selected ucontexts from one source RXE ib_device and the
RXE-private state belonging to their objects.

    source rxe0
      selected context A  --+
      selected context B  --+--> RXE vHCA image
      unrelated context C        not included

    fresh destination rxe0
      RXE_LOAD_VHCA(image)
      RESTORE operations recreate contexts A and B and their uobjects

All selected contexts must come from the same source ib_device. The image is
loaded only into a fresh destination RXE device. This avoids merging saved
QPNs, keys, mmap offsets, and allocator state into an active device.

The source device may have unrelated contexts. RXE can select contexts because
it implements the image in software; it does not have to copy the whole-VF
capture boundary imposed by mlx5 firmware.

The hardware-like part is the interface: CRIU moves one opaque driver image,
loads it before uobject replay, and does not interpret private records.

### 2.2 Remove EXCLUSIVE versus SHAREABLE

CRIU currently asks each plugin to declare the provider EXCLUSIVE or
SHAREABLE:

- mlx5 VFMIG is EXCLUSIVE because its firmware image covers a whole VF;
- RXE is SHAREABLE because its current implementation selects contexts.

This is the wrong abstraction. Sharing follows from the scope of a save
operation; it is not a permanent provider property.

Remove:

    CR_RDMA_SHARING_EXCLUSIVE
    CR_RDMA_SHARING_SHAREABLE
    CR_PLUGIN_DECLARE_RDMA_SHARING

Each image implementation enforces its real boundary:

- mlx5 ensures the checkpoint covers every context affected by its whole-VF
  firmware save;
- RXE receives an explicit context set and rejects references outside it.

CRIU no longer performs a provider-wide sharing-policy lookup.

### 2.3 State ownership

CRIU and generic RESTORE operations continue to own:

- uverbs files and ucontexts;
- uobject handles and relationships;
- PD, MR, CQ, and QP creation parameters;
- userspace-visible QPN, lkey, and rkey identity;
- queue geometry and mmap offsets; and
- process memory and registered memory contents.

The RXE vHCA image owns:

- requester, completer, and responder execution state;
- live PSNs, MSN/SSN, and retry budgets;
- CQ notification state;
- responder read, atomic, atomic-write, and flush resources;
- logical timer state; and
- queue bytes, if the queue-in-image option is selected.

The image must not contain kernel pointers, locks, skb pointers, work items,
timer internals, or native copies of struct rxe_qp, struct rxe_cq, or
struct resp_res.

Application virtual addresses and MR IOVAs are not kernel pointers. They are
part of the process and MR contract.

### 2.4 Queue indices

An RXE queue header contains producer_index and consumer_index. The kernel
also keeps a cached cursor in struct rxe_queue::index.

- For QUEUE_TYPE_FROM_CLIENT, queue->index is the consumer.
- For QUEUE_TYPE_TO_CLIENT, queue->index is the producer.

Wherever queue bytes are restored, queue->index must be reconstructed before
the datapath is unfrozen.

## 3. RXE image format

The image is an RXE-owned binary stream, not a CRIU protobuf. CRIU saves and
loads it without parsing it.

The stream starts with an image header followed by typed records:

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

Initial record types:

    RXE_VHCA_RECORD_CONTEXT
    RXE_VHCA_RECORD_QP
    RXE_VHCA_RECORD_CQ
    RXE_VHCA_RECORD_RESP_RESOURCE
    RXE_VHCA_RECORD_QUEUE

The queue record is present only if queues are put in the vHCA image.

object_id is local to the image. It associates a private record with a later
RESTORE_QP or RESTORE_CQ call. It is not a pointer or pool index.

A context record identifies its child records. It does not have another UUID.
If a UUID is needed, there is one for the complete RXE vHCA image. Explicit
CRIU source-to-destination mapping may make a UUID unnecessary.

Network namespace IDs, destination ifindices, MAC addresses, MTU, and GIDs are
not authoritative RXE blob fields. CRIU and the orchestrator establish that
topology before creating the destination RXE device.

### 3.1 QP, CQ, and timer records

A QP record contains:

    context and object IDs
    requester/completer/responder PSNs
    MSN and SSN
    remaining retry and RNR-retry counts
    responder opcode, acknowledgment, and replay state
    responder-resource references

Responder resources use tagged records. Pointers are replaced by object IDs,
rkeys, virtual addresses, lengths, offsets, and protocol state.

A CQ record contains its object ID and notification state not represented by
generic CQ attributes or queue memory.

Timer objects and absolute jiffies values are not copied. The prototype must
choose whether to save the remaining relative delay or schedule an immediate
retry after unfreeze while preserving the retry budget.

## 4. Device creation and image loading

Device creation and image loading are separate:

    RDMA_NLDEV_CMD_NEWLINK(name, type="rxe", backing_netdev)
    RXE_LOAD_VHCA(new RXE device, image stream)

NEWLINK creates the RXE device and binds it to the restored netdevice in the
restored network namespace. It already handles namespaces, names, collision
errors, link notification, and deletion.

RXE_LOAD_VHCA loads state after the device exists. Putting the image in
netlink would mix topology management with a large migration stream. Having
RXE_LOAD_VHCA create the device would require a global RXE control endpoint
and duplicate NEWLINK behavior.

Loading into an active RXE device is out of scope. It requires merge rules for
QPNs, keys, handles, mmap offsets, and allocators.

### 4.1 Streaming API

Follow the VFMIG save-fd/load-fd model:

    struct rxe_vhca_save_state {
            __u32 flags;
            __s32 save_fd;
    };

    struct rxe_vhca_load_state {
            __u32 flags;
            __s32 load_fd;
    };

Save flow:

1. Start an RXE save operation for one source device.
2. Add each selected frozen ucontext.
3. Tell RXE that the context list is complete. RXE rejects further additions,
   verifies that every context belongs to that device, captures their state,
   and returns a readable save_fd.
4. Read the completed image from save_fd.

The add-context operation can accept a uverbs fd or be a uverbs method that
adds its context to an image handle. The prototype must choose between them.

On load, CRIU writes the blob to load_fd. RXE retains the context and object
records on the fresh destination device. The endpoint can be an RXE migration
cdev or a driver uverbs method.

### 4.2 Association with uobject replay

RXE_LOAD_VHCA runs before destination uobjects exist, so it stages records
instead of creating objects.

Provider restore input supplies an image object ID:

    RESTORE_QP(..., rxe_vhca_object_id)
    RESTORE_CQ(..., rxe_vhca_object_id)

RXE binds each new object to its staged record. After RDMA objects and memory
are restored, DATAPATH_FREEZE(false) applies remaining state, reconstructs
queue->index, and starts the datapath.

## 5. Queue-memory choices

Queue data must have exactly one owner.

### Option 1: keep queues in object UHW

Continue copying queue headers and entries through QP/CQ query and restore
operations. This is the smallest change but keeps large memory payloads in
per-object control operations.

### Option 2: let CRIU save mapped pages

CRIU currently treats RXE queues as plugin-owned device mappings and saves
their metadata, not their contents.

Queue pages are restored only to the same RXE kernel implementation. Their
mapped layout is not a cross-kernel conversion format; geometry and mapping
identity are validated to reject corrupt or mismatched queues.

The plugin could identify a queue mapping by uverbs file, offset, length, and
owning uobject. CRIU would copy only those verified mappings. This is what
earlier drafts called "explicit opt-in"; arbitrary device mappings cannot be
copied because they may be MMIO or have side effects.

On restore, CRIU must map the new RXE queue and write the saved pages into it.
This requires CRIU memory-restoration changes and may retain PIE coordination.

Creating anonymous memory first is insufficient: replacing it with an RXE
mmap discards its contents. RXE would need an API to adopt user-provided
backing memory.

### Option 3: put queues in the RXE vHCA image

Queue records contain:

    owner object ID
    SQ/RQ/SRQ/CQ role
    queue geometry
    complete header and entry storage
    queue->index

RXE stages the records on load and populates queues after RESTORE_QP/CQ
allocates them.

This keeps CRIU simple and is most likely to remove RXE-specific PIE queue
handling. It produces a larger image and requires temporary kernel storage
until destination objects exist.

### Option 4: make RXE queues user-backed

Userspace could provide queue backing memory, as it does for hardware
providers. CRIU would then restore queues as ordinary process memory.

This is the cleanest ownership model but the largest RXE ABI change. It should
not block the vHCA prototype.

The first prototype should compare options 2 and 3. Option 3 is the likely
starting point if minimizing CRIU and PIE complexity is the priority.

## 6. Call order

Dump:

    stop application tasks
    freeze selected RXE contexts
    build and save the RXE vHCA image
    save generic uverbs files and uobjects
    save process memory
    save queues through their selected owner

Existing source-thaw and --leave-running behavior does not change.

Restore:

    restore network namespace and backing netdevice
    NEWLINK creates a fresh RXE device
    RXE_LOAD_VHCA stages the context-set image
    restore uverbs files and contexts
    RESTORE operations recreate and associate uobjects
    restore process memory and queue state
    DATAPATH_FREEZE(false) applies remaining state and starts the datapath
    release application tasks

The datapath remains frozen until all memory RXE can access is restored.

## 7. CRIU changes

Move common blob handling from the mlx5 VFMIG plugin into CRIU's RDMA layer:

- drain a provider save fd into a blob file;
- stream a blob file into a provider load fd;
- associate one blob with its RDMA device metadata;
- avoid duplicate save/load for contexts in one image; and
- load the device image before uobject replay.

The RXE provider supplies context-selection and save/load callbacks. mlx5
continues to supply firmware save/load callbacks.

Remove the EXCLUSIVE/SHAREABLE plugin API. Each save path validates its real
capture boundary.

The checkpoint contains:

    RDMA device metadata   device and namespace relationships, blob name
    RXE vHCA blob          opaque RXE driver image
    uverbsfd image         fd and ucontext ownership
    rdma_uobj image        generic object graph and identities
    pages images           process memory; queues only under option 2

## 8. Implementation

1. Define RXE image records and encode/decode helpers.
2. Add KUnit image-codec tests.
3. Add the selected-context save operation and readable image stream.
4. Add the load stream for a fresh RXE device.
5. Associate context/object image IDs with provider RESTORE input.
6. Move QP, CQ, and responder state from object UHW to the image.
7. Implement one queue option as a separate series.
8. Apply staged state and reconstruct queue indices during unfreeze.
9. Move common blob streaming into CRIU's RDMA layer.
10. Remove the CRIU plugin sharing-policy API.
11. Update the RXE plugin and remove obsolete queue/PIE code.

Each kernel commit must pass strict checkpatch and compile independently.

## 9. Tests

Extend rdma_test_agent to test:

1. image creation from one and multiple contexts;
2. rejection of contexts from different source RXE devices;
3. exclusion of an unrelated source context;
4. destination RXE creation through NEWLINK;
5. load before any normal destination ucontext exists;
6. context, PD, MR, CQ, and QP restore in dependency order;
7. association of each CQ/QP with its image record;
8. the selected queue restore path;
9. queue->index against the restored queue header; and
10. existing RXE checkpoint/restore workloads after unfreeze.

CRIU tests must cover two selected contexts in one image, an unrelated source
context, different source/destination netdevice names, and rejection of load
into a non-fresh destination.

## 10. Open decisions

1. What API supplies the selected ucontext list to the RXE save operation?
2. Does save/load use an RXE migration cdev or uverbs methods?
3. Are queues restored by CRIU or included in the RXE image?
4. If CRIU owns queues, can the RXE PIE hooks be removed?
5. What limits apply to staged image and queue bytes?
6. What image key associates loaded records with RESTORE operations?
7. Is a device UUID needed, or is explicit CRIU mapping sufficient?
8. Do retry timers retain their delay or retry immediately?
