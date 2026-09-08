# 1. Problem

CRIU currently restores RDMA state one context graph at a time. Generic
RESTORE operations recreate ucontexts, PDs, MRs, CQs, QPs, and their
relationships. RXE-specific state is passed through provider UHW attached to
those objects.

mlx5 VFMIG has a cleaner split:

- firmware saves and loads one opaque vHCA image; and
- RESTORE operations recreate the Linux RDMA objects associated with it; And
  RXE should deliberately mock this hardware model. Since RXE does not have
  FW, it needs to handle creating and serializing the vHCA image, while
  RESTORE operations seed the RDMA stack without requiring RXE private state.

RXE has no firmware, so the driver must build and consume the equivalent
image. Restore operations still allocate and initialize real RDMA objects.

Queue storage is related but separate. RXE allocates SQ, RQ, SRQ, and CQ
memory in the kernel and maps it to userspace. Those bytes need one owner:
uobject UHW, CRIU memory images, or the RXE vHCA image.

This refactor must not reduce the functionality supported by the existing RXE
checkpoint/restore implementation.

# 2. Proposed model

## 2.1 RXE vHCA

An RXE vHCA contains all ucontexts from one source RXE ib_device and the
RXE-private state belonging to their objects.

```
source rxe0
  context A  --+
  context B  --+
  context C --+--> RXE vHCA image

fresh destination rxe0
  RXE_LOAD_VHCA(image)
  RESTORE operations recreate contexts A/B/C and their uobjects
```

The image is loaded only into a fresh destination RXE device. This avoids
merging saved QPNs, keys, mmap offsets, and allocator state into an active
device.

The hardware-like part is the interface: CRIU moves one opaque driver image,
loads it before uobject replay, and does not interpret private records.

## 2.2 Remove EXCLUSIVE versus SHAREABLE

CRIU currently asks each plugin to declare the provider EXCLUSIVE or
SHAREABLE:

- mlx5 VFMIG is EXCLUSIVE because its firmware image covers a whole VF;
- RXE is SHAREABLE because its current implementation selects contexts.

Each image implementation enforces its real boundary:

- mlx5 ensures the checkpoint covers every context affected by its whole-VF
  firmware save;
- RXE receives an explicit context set and rejects references outside it.

CRIU no longer performs a provider-wide sharing-policy lookup.

Remove:

```
CR_RDMA_SHARING_EXCLUSIVE
CR_RDMA_SHARING_SHAREABLE
CR_PLUGIN_DECLARE_RDMA_SHARING
```

## 2.3 State ownership

CRIU and generic RESTORE operations continue to own:

- uverbs files and ucontexts;
- uobject handles and relationships;
- PD, MR, CQ, and QP creation parameters;
- userspace-visible QPN, lkey, and rkey identity;
- queue geometry and mmap offsets; and
- process memory

The RXE vHCA image owns:

- requester, completer, and responder execution state;
- live PSNs, MSN/SSN, and retry budgets;
- CQ notification state;
- responder read, atomic, atomic-write, and flush resources;
- logical timer state; and

Three options were considered for the queue bytes:

- vHCA image
- Rdma uobject image
- CRIU pages

The image must not serialize destination local runtime state such as kernel
pointers, locks, skbs, work items, or timer objects. Similarly, do not copy
rxe_qp or rxe_cq objects. However, resp_res must be restored as it can contain
valid data related to a previous transmission.

Application virtual addresses and MR IOVAs are not kernel pointers. They are
part of the process and MR contract.

## 2.4 Queue indices

An RXE queue header contains producer_index and consumer_index. The kernel
also keeps a cached cursor in struct rxe_queue::index.

For QUEUE_TYPE_FROM_CLIENT, queue->index is the consumer.
For QUEUE_TYPE_TO_CLIENT, queue->index is the producer.
Wherever queue bytes are restored, queue->index must be reconstructed before
the datapath is unfrozen.

# 3. RXE image format

The image is an RXE-owned binary stream, not a CRIU protobuf. CRIU saves and
loads it without parsing it.

The stream starts with an image header followed by typed records:

```c
struct rxe_vhca_image_header {
	__le32 magic;
};

struct rxe_vhca_record_header {
	__le32 type;
	__le32 flags;
	__le64 length;
};
```

Length is the record payload, minus the record header. Flags is by default
zero unless record type defines otherwise.

Context and QP sections begin with these payload headers:

```c
struct rxe_vhca_context_header {
	__le32 ufile_id;
	__le32 cq_count;
	__le32 qp_count;
	__le32 reserved;
};

struct rxe_vhca_qp_header {
	__le32 uobject_handle;
	__le32 resp_resource_count;
};
```

The total image length is supplied to RXE_LOAD_VHCA. RXE validates each
record's length against that boundary.

Initial record types:

```
RXE_VHCA_RECORD_CONTEXT
RXE_VHCA_RECORD_QP
RXE_VHCA_RECORD_CQ
RXE_VHCA_RECORD_RESP_RESOURCE
```

A context record establishes the scope for its following object records.
Object-specific payloads contain the identifiers needed to associate them
with uobject replay. For example:

- a QP record contains its source context key and QP handle;
- a CQ record contains its source context key and CQ handle;
- a responder-resource record identifies its owning QP and resource slot;
- a queue record identifies its owning QP, CQ, or SRQ and the queue type.

## 3.1 QP, CQ, and timer records

A QP record contains:

- context and object IDs
- requester/completer/responder PSNs
- MSN and SSN
- remaining retry and RNR-retry counts
- responder opcode, acknowledgment, and replay state
- responder-resource references

Responder resources use tagged records. Pointers are replaced by object IDs,
rkeys, virtual addresses, lengths, offsets, and protocol state.

# 4. Device creation and image loading

Device creation and image loading are separate:

Use the existing api to create the rxe device
RDMA_NLDEV_CMD_NEWLINK(name, type="rxe", backing_netdev)

Load state via RXE_LOAD_VHCA(new RXE device, image stream)

NEWLINK creates the RXE device and binds it to the restored netdevice in the
restored network namespace. It already handles namespaces, names, collision
errors, link notification, and deletion.

RXE_LOAD_VHCA loads state after the device exists. Putting the image in
netlink would mix topology management with a large migration stream. Having
RXE_LOAD_VHCA create the device would require a global RXE control endpoint
and duplicate NEWLINK behavior.

## 4.1 Streaming API

Follow the VFMIG save-fd/load-fd model:

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

Save flow:

1. Start an RXE save operation for one source device.
2. Add each selected frozen ucontext.
3. Tell RXE that the context list is complete. RXE rejects further additions,
   verifies that every context belongs to that device, captures their state,
   and returns a readable save_fd. No more contexts can be added to this
   image. Source rxe then follows existing criu flow and resumes.
4. Read the completed image from save_fd.

The add-context operation can accept a uverbs fd or be a uverbs method that
adds its context to an image handle. The prototype must choose between them.

On load, CRIU writes the blob to load_fd. RXE retains the context and object
records on the fresh destination device. The endpoint can be an RXE migration
cdev or a driver uverbs method.

## 4.2 Association with uobject replay

RXE_LOAD_VHCA runs before destination ucontexts and uobjects exist, so it
stages the saved RXE-private records.

When CRIU creates a destination ucontext, the RXE context-restore path
supplies its saved ufile_id. RXE associates the new rxe_ucontext with the
matching staged context record.

The generic RESTORE_QP and RESTORE_CQ operations already supply the saved
uobject handle. RXE therefore finds private state using:

```
saved ufile_id + object type + saved uobject handle
```

Each RESTORE_* operation applies the staged private state for the object it
creates. CRIU subsequently maps the new RXE queues and restores their bytes
through its memory path.

After the queue mappings and other process memory have been restored,
DATAPATH_FREEZE(false) reconstructs queue->index from the shared queue headers
and resumes the datapath.

# 5. Queue bytes

The design decision is where the checkpoint stores the bytes in the RXE SQ,
RQ, SRQ, and CQ mappings.

Today, QUERY_QP and QUERY_CQ return queue contents in provider UHW. CRIU puts
that UHW in the object's `plugin_blob` in `rdma-uobj.img`, and RESTORE_QP or
RESTORE_CQ copies the bytes into the new RXE queue. Queue data is therefore
currently stored with individual uobjects.

The proposed format moves those bytes to CRIU's page images:

```
                        Current             Proposed
rdma-uobj.img           queue bytes         queue metadata only
RXE vHCA blob           no queue bytes      no queue bytes
CRIU page images        no queue bytes      complete queue mappings
```

This matches a hardware provider such as mlx5, where queue memory is restored
with the process. The RXE vHCA blob is then limited to private device state,
and per-object restore operations do not carry bulk queue data.

## 5.1 Dump

Dump happens after the RXE datapath is frozen:

1. While dumping `rdma-uobj.img`, CRIU queries each RXE QP, CQ, and SRQ for
   its queue geometry and mmap offsets. The query returns metadata only; it
   does not return queue entries.
2. CRIU builds an inventory keyed by uverbs file, mmap offset, and length.
   Each entry identifies the owning `ufile_id`, uobject handle, and queue role
   (SQ, RQ, SRQ, or CQ).

Steps [3] - [5] diverge from existing implementations.

3. When CRIU encounters a VMA backed by an RXE uverbs file, it matches the VMA
   against that inventory. An exact match marks the VMA as safe to copy. An
   unmatched or partial mapping is rejected rather than treated as memory.
4. CRIU dumps every page in the matched VMA into its normal page images. This
   captures the queue header, all WQEs or CQEs, and the shared producer and
   consumer indices.
5. CRIU records the VMA address, mmap offset, length, and sharing information
   needed to map the same queue during restore.

Part of the work is to find a way to store queue memory pages along with
process memory for [4] and [5].

## 5.2 Restore

Restore happens in this order:

1. RESTORE_QP, RESTORE_CQ, and RESTORE_SRQ create the objects and allocate
   empty RXE queue buffers with the saved geometry and mmap offsets.
2. CRIU maps those buffers at the saved virtual addresses.

Step [3] is new and step [4] needs to have DATAPATH_FREEZE removed and the
work move to RESUME_VHCA.

3. CRIU copies the saved queue bytes from its page images into the mappings.
   If a queue had multiple mappings, CRIU recreates that sharing.
4. CRIU calls DATAPATH_FREEZE(false) after every queue has been populated.

Follow vfmig design so that VHCA loading comes after graph reconstruction on
RESTORE. RESTORE allocations destination queue buffers and CRIU maps those
buffers and populates them from the page images.

queue->index is private kernel state, so it is not present in the page image.
During DATAPATH_FREEZE(false) RXE rebuilds it from the restored queue header:

- QUEUE_TYPE_FROM_CLIENT: use consumer_index;
- QUEUE_TYPE_TO_CLIENT: use producer_index.

RXE then resumes the datapath. No separate finalization operation is needed.

Once this page-image path works, QUERY_QP, QUERY_CQ, RESTORE_QP, and
RESTORE_CQ must stop carrying queue headers and entries in UHW. The RXE vHCA
format must not add another copy of them.

# 6. Call order

Dump:

1. stop application tasks
2. freeze selected RXE contexts
3. save generic uverbs files and uobjects
4. save process memory
5. save queues through their selected owner
6. build and save the RXE vHCA image

Existing source-thaw and --leave-running behavior does not change.

Restore:

1. restore network namespace and backing netdevice
2. NEWLINK creates a fresh RXE device
3. RXE_LOAD_VHCA stages the context-set image
4. restore uverbs files and contexts
5. RESTORE operations recreate and associate uobjects
6. restore process memory and queue state
7. DATAPATH_FREEZE(false) applies remaining state and starts the datapath
8. release application tasks

The datapath remains frozen until all memory RXE can access is restored.

# 7. CRIU changes

Move common blob handling from the mlx5 VFMIG plugin into CRIU's RDMA layer:

- drain a provider save fd into a blob file;
- stream a blob file into a provider load fd;
- associate one blob with its RDMA device metadata;
- avoid duplicate save/load for contexts in one image; and
- load the device image before uobject replay.

The RXE provider supplies context-selection and save/load callbacks. mlx5
continues to supply firmware save/load callbacks.

Remove the EXCLUSIVE/SHAREABLE plugin API.

The checkpoint contains:

```
RDMA vHCA image rxe_mig.img devicedevice  metadata  source device identity, topology mapping, and vHCA blob reference

ufile_id         uverbs-file ID from criu/images/uverbsfd.proto
rdma_uobj image   generic uobject graph, attributes, and identities
page images       process memory, including RXE queue mappings
```

# 8. Implementation

1. Define the RXE image records and cover their encoding and validation with
   KUnit tests.
2. As a first step, initially pull the responder resources for the QPs.
3. Match staged records by image context ID and saved uobject handle. Apply
   QP, CQ, and responder state during the corresponding RESTORE operation.
4. Move provider blob streaming into CRIU's RDMA layer and add support for
   saving and restoring verified RXE queue VMAs.
5. Reconstruct queue indices and resume the datapath in
   DATAPATH_FREEZE(false).
6. Remove queue contents from object UHW, remove the EXCLUSIVE/SHAREABLE API,
   and delete RXE plugin code replaced by the common paths.

# 10. Open decisions

Define the device-level RXE save/load API. The save API must accept each
selected uverbs context together with its existing CRIU ufile_id; the load API
must operate on a fresh RXE device before destination ucontexts are created.

How do RXE queue mappings use the page images and which plugin hooks remain
necessary to remap them on the correct uverbs file.
