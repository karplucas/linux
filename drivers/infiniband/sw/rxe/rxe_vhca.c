// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "rxe.h"
#include "rxe_vhca.h"

static_assert(sizeof(struct rxe_vhca_image_header) == 4);
static_assert(sizeof(struct rxe_vhca_record_header) == 16);
static_assert(sizeof(struct rxe_vhca_cq) == 8);
static_assert(sizeof(struct rxe_vhca_resp_resource) == 64);

static int rxe_vhca_encode_resource_type(int type, u32 *image_type)
{
	switch (type) {
	case RXE_READ_MASK:
		*image_type = RXE_VHCA_RESP_RESOURCE_READ;
		break;
	case RXE_ATOMIC_MASK:
		*image_type = RXE_VHCA_RESP_RESOURCE_ATOMIC;
		break;
	case RXE_ATOMIC_WRITE_MASK:
		*image_type = RXE_VHCA_RESP_RESOURCE_ATOMIC_WRITE;
		break;
	case RXE_FLUSH_MASK:
		*image_type = RXE_VHCA_RESP_RESOURCE_FLUSH;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rxe_vhca_decode_resource_type(u32 image_type, int *type)
{
	switch (image_type) {
	case RXE_VHCA_RESP_RESOURCE_READ:
		*type = RXE_READ_MASK;
		break;
	case RXE_VHCA_RESP_RESOURCE_ATOMIC:
		*type = RXE_ATOMIC_MASK;
		break;
	case RXE_VHCA_RESP_RESOURCE_ATOMIC_WRITE:
		*type = RXE_ATOMIC_WRITE_MASK;
		break;
	case RXE_VHCA_RESP_RESOURCE_FLUSH:
		*type = RXE_FLUSH_MASK;
		break;
	default:
		return -EBADMSG;
	}

	return 0;
}

int rxe_vhca_encode_resp_resource(struct rxe_vhca_resp_resource *record,
				  u32 slot, const struct resp_res *resource)
{
	u32 type;
	int err;

	if (!record || !resource)
		return -EINVAL;
	if (!resource->type) {
		memset(record, 0, sizeof(*record));
		record->slot = cpu_to_le32(slot);
		return 0;
	}

	if (resource->state < rdatm_res_state_next ||
	    resource->state > rdatm_res_state_replay)
		return -EINVAL;
	if (resource->replay != 0 && resource->replay != 1)
		return -EINVAL;
	if (resource->first_psn > BTH_PSN_MASK ||
	    resource->last_psn > BTH_PSN_MASK ||
	    resource->cur_psn > BTH_PSN_MASK)
		return -EINVAL;

	err = rxe_vhca_encode_resource_type(resource->type, &type);
	if (err)
		return err;

	memset(record, 0, sizeof(*record));
	record->slot = cpu_to_le32(slot);
	record->type = cpu_to_le32(type);
	record->state = cpu_to_le32(resource->state);
	record->first_psn = cpu_to_le32(resource->first_psn);
	record->last_psn = cpu_to_le32(resource->last_psn);
	record->cur_psn = cpu_to_le32(resource->cur_psn);
	if (resource->replay)
		record->flags = cpu_to_le32(RXE_VHCA_RESP_RESOURCE_F_REPLAY);

	switch (resource->type) {
	case RXE_READ_MASK:
		if (resource->read.resid > resource->read.length)
			return -EINVAL;
		record->data.read.va_org = cpu_to_le64(resource->read.va_org);
		record->data.read.va = cpu_to_le64(resource->read.va);
		record->data.read.rkey = cpu_to_le32(resource->read.rkey);
		record->data.read.length = cpu_to_le32(resource->read.length);
		record->data.read.resid = cpu_to_le32(resource->read.resid);
		break;
	case RXE_ATOMIC_MASK:
		record->data.atomic.orig_val =
			cpu_to_le64(resource->atomic.orig_val);
		break;
	case RXE_FLUSH_MASK:
		record->data.flush.va = cpu_to_le64(resource->flush.va);
		record->data.flush.length = cpu_to_le32(resource->flush.length);
		record->data.flush.type = resource->flush.type;
		record->data.flush.level = resource->flush.level;
		break;
	}

	return 0;
}

int rxe_vhca_decode_resp_resource(const struct rxe_vhca_resp_resource *record,
				  u32 slots, struct resp_res *resource)
{
	u32 flags;
	u32 state;
	u32 type;
	int err;

	if (!record || !resource || le32_to_cpu(record->slot) >= slots)
		return -EINVAL;

	flags = le32_to_cpu(record->flags);
	state = le32_to_cpu(record->state);
	type = le32_to_cpu(record->type);
	if (flags & ~RXE_VHCA_RESP_RESOURCE_F_REPLAY ||
	    state > RXE_VHCA_RESP_RESOURCE_REPLAY ||
	    le32_to_cpu(record->reserved) ||
	    le32_to_cpu(record->first_psn) > BTH_PSN_MASK ||
	    le32_to_cpu(record->last_psn) > BTH_PSN_MASK ||
	    le32_to_cpu(record->cur_psn) > BTH_PSN_MASK)
		return -EBADMSG;

	memset(resource, 0, sizeof(*resource));
	if (type == RXE_VHCA_RESP_RESOURCE_EMPTY) {
		if (flags || state || le32_to_cpu(record->first_psn) ||
		    le32_to_cpu(record->last_psn) ||
		    le32_to_cpu(record->cur_psn) ||
		    memchr_inv(record->data.raw, 0, sizeof(record->data.raw)))
			return -EBADMSG;
		return 0;
	}
	err = rxe_vhca_decode_resource_type(type, &resource->type);
	if (err)
		return err;

	resource->replay = !!(flags & RXE_VHCA_RESP_RESOURCE_F_REPLAY);
	resource->state = state;
	resource->first_psn = le32_to_cpu(record->first_psn);
	resource->last_psn = le32_to_cpu(record->last_psn);
	resource->cur_psn = le32_to_cpu(record->cur_psn);

	switch (resource->type) {
	case RXE_READ_MASK:
		if (le32_to_cpu(record->data.read.reserved))
			return -EBADMSG;
		resource->read.va_org = le64_to_cpu(record->data.read.va_org);
		resource->read.va = le64_to_cpu(record->data.read.va);
		resource->read.rkey = le32_to_cpu(record->data.read.rkey);
		resource->read.length = le32_to_cpu(record->data.read.length);
		resource->read.resid = le32_to_cpu(record->data.read.resid);
		if (resource->read.resid > resource->read.length)
			return -EBADMSG;
		break;
	case RXE_ATOMIC_MASK:
		if (memchr_inv(record->data.atomic.reserved, 0,
			       sizeof(record->data.atomic.reserved)))
			return -EBADMSG;
		resource->atomic.orig_val =
			le64_to_cpu(record->data.atomic.orig_val);
		break;
	case RXE_ATOMIC_WRITE_MASK:
		if (memchr_inv(record->data.raw, 0, sizeof(record->data.raw)))
			return -EBADMSG;
		break;
	case RXE_FLUSH_MASK:
		if (le16_to_cpu(record->data.flush.reserved) ||
		    memchr_inv(record->data.flush.reserved2, 0,
			       sizeof(record->data.flush.reserved2)))
			return -EBADMSG;
		resource->flush.va = le64_to_cpu(record->data.flush.va);
		resource->flush.length = le32_to_cpu(record->data.flush.length);
		resource->flush.type = record->data.flush.type;
		resource->flush.level = record->data.flush.level;
		break;
	}

	return 0;
}

int rxe_vhca_writer_init(struct rxe_vhca_writer *writer, void *data,
			 size_t capacity)
{
	if (!writer || !data)
		return -EINVAL;

	if (capacity < sizeof(struct rxe_vhca_image_header))
		return -ENOSPC;

	put_unaligned_le32(RXE_VHCA_IMAGE_MAGIC, data);
	writer->data = data;
	writer->capacity = capacity;
	writer->length = sizeof(struct rxe_vhca_image_header);

	return 0;
}

int rxe_vhca_write_record(struct rxe_vhca_writer *writer, u32 type,
			  u32 flags, const void *payload, size_t length)
{
	struct rxe_vhca_record_header *header;
	size_t record_length;
	size_t end;

	if (!writer || !writer->data || (!payload && length))
		return -EINVAL;

	if (check_add_overflow(sizeof(*header), length, &record_length) ||
	    check_add_overflow(writer->length, record_length, &end) ||
	    end > writer->capacity)
		return -ENOSPC;

	header = (void *)(writer->data + writer->length);
	put_unaligned_le32(type, &header->type);
	put_unaligned_le32(flags, &header->flags);
	put_unaligned_le64(length, &header->length);
	memcpy((u8 *)header + sizeof(*header), payload, length);
	writer->length = end;

	return 0;
}

int rxe_vhca_reader_init(struct rxe_vhca_reader *reader, const void *data,
			 size_t length)
{
	if (!reader || !data)
		return -EINVAL;

	if (length < sizeof(struct rxe_vhca_image_header))
		return -EBADMSG;

	if (get_unaligned_le32(data) != RXE_VHCA_IMAGE_MAGIC)
		return -EBADMSG;

	reader->data = data;
	reader->length = length;
	reader->offset = sizeof(struct rxe_vhca_image_header);

	return 0;
}

int rxe_vhca_read_record(struct rxe_vhca_reader *reader,
			 struct rxe_vhca_record *record)
{
	const struct rxe_vhca_record_header *header;
	u64 payload_length;
	size_t end;

	if (!reader || !record || !reader->data)
		return -EINVAL;

	if (reader->offset == reader->length)
		return 0;

	if (reader->offset > reader->length ||
	    reader->length - reader->offset < sizeof(*header))
		return -EBADMSG;

	header = (const void *)(reader->data + reader->offset);
	payload_length = get_unaligned_le64(&header->length);
	if (payload_length > SIZE_MAX ||
	    check_add_overflow(reader->offset, sizeof(*header), &end) ||
	    check_add_overflow(end, (size_t)payload_length, &end) ||
	    end > reader->length)
		return -EBADMSG;

	record->type = get_unaligned_le32(&header->type);
	record->flags = get_unaligned_le32(&header->flags);
	record->payload = (const u8 *)header + sizeof(*header);
	record->length = payload_length;
	reader->offset = end;

	return 1;
}

bool rxe_vhca_has_context(const void *data, size_t length, u32 ufile_id)
{
	struct rxe_vhca_context_header context;
	struct rxe_vhca_record record;
	struct rxe_vhca_reader reader;
	int err;

	if (rxe_vhca_reader_init(&reader, data, length))
		return false;

	while ((err = rxe_vhca_read_record(&reader, &record)) > 0) {
		if (record.type != RXE_VHCA_RECORD_CONTEXT ||
		    record.length != sizeof(context))
			continue;
		memcpy(&context, record.payload, sizeof(context));
		if (le32_to_cpu(context.ufile_id) == ufile_id)
			return true;
	}

	return false;
}

int rxe_vhca_find_cq(const void *data, size_t length, u32 ufile_id,
		     u32 uobject_handle, struct rxe_vhca_cq *cq)
{
	struct rxe_vhca_context_header context;
	struct rxe_vhca_record record;
	struct rxe_vhca_reader reader;
	u32 remaining = 0;
	bool match = false;
	int err;

	err = rxe_vhca_reader_init(&reader, data, length);
	if (err)
		return err;

	while ((err = rxe_vhca_read_record(&reader, &record)) > 0) {
		if (record.type == RXE_VHCA_RECORD_CONTEXT) {
			if (record.length != sizeof(context))
				return -EBADMSG;
			memcpy(&context, record.payload, sizeof(context));
			remaining = le32_to_cpu(context.cq_count);
			match = le32_to_cpu(context.ufile_id) == ufile_id;
			continue;
		}
		if (record.type != RXE_VHCA_RECORD_CQ || !remaining ||
		    record.length != sizeof(*cq))
			continue;
		remaining--;
		memcpy(cq, record.payload, sizeof(*cq));
		if (match && le32_to_cpu(cq->uobject_handle) == uobject_handle)
			return 0;
	}

	return err ?: -ENOENT;
}

int rxe_vhca_find_qp(const void *data, size_t length, u32 ufile_id,
		     u32 uobject_handle, struct rxe_restore_qp_req *state,
		     struct resp_res **resources)
{
	struct rxe_vhca_context_header context;
	struct rxe_vhca_record record;
	struct rxe_vhca_reader reader;
	u32 cqs = 0, qps = 0;
	bool match = false;
	int err;

	*resources = NULL;
	err = rxe_vhca_reader_init(&reader, data, length);
	if (err)
		return err;

	while ((err = rxe_vhca_read_record(&reader, &record)) > 0) {
		struct rxe_vhca_qp qp;
		u32 count, i;

		if (record.type == RXE_VHCA_RECORD_CONTEXT) {
			if (record.length != sizeof(context))
				return -EBADMSG;
			memcpy(&context, record.payload, sizeof(context));
			cqs = le32_to_cpu(context.cq_count);
			qps = le32_to_cpu(context.qp_count);
			match = le32_to_cpu(context.ufile_id) == ufile_id;
			continue;
		}
		if (record.type == RXE_VHCA_RECORD_CQ && cqs) {
			cqs--;
			continue;
		}
		if (record.type != RXE_VHCA_RECORD_QP || cqs || !qps ||
		    record.length != sizeof(qp))
			return -EBADMSG;
		qps--;
		memcpy(&qp, record.payload, sizeof(qp));
		count = le32_to_cpu(qp.header.resp_resource_count);
		if (match && le32_to_cpu(qp.header.uobject_handle) ==
		    uobject_handle && count) {
			*resources = kcalloc(count, sizeof(**resources), GFP_KERNEL);
			if (!*resources)
				return -ENOMEM;
		}
		for (i = 0; i < count; i++) {
			struct rxe_vhca_resp_resource image_resource;
			struct resp_res *resource;
			u32 slot;

			err = rxe_vhca_read_record(&reader, &record);
			if (err <= 0 || record.type != RXE_VHCA_RECORD_RESP_RESOURCE ||
			    record.length != sizeof(image_resource)) {
				err = -EBADMSG;
				goto err_free;
			}
			if (!*resources)
				continue;
			memcpy(&image_resource, record.payload,
			       sizeof(image_resource));
			slot = le32_to_cpu(image_resource.slot);
			if (slot >= count) {
				err = -EBADMSG;
				goto err_free;
			}
			resource = *resources + slot;
			err = rxe_vhca_decode_resp_resource(&image_resource, count, resource);
			if (err)
				goto err_free;
		}
		if (match && le32_to_cpu(qp.header.uobject_handle) ==
		    uobject_handle) {
			memcpy(state, &qp.state, sizeof(*state));
			return 0;
		}
	}

	return err ?: -ENOENT;

err_free:
	kfree(*resources);
	*resources = NULL;
	return err;
}

int rxe_vhca_validate_contexts(const void *data, size_t length)
{
	struct rxe_vhca_context_header context;
	struct rxe_vhca_record record;
	struct rxe_vhca_reader reader;
	u32 context_count = 0;
	u32 cq_count = 0;
	u32 qp_count = 0;
	u32 resource_count = 0;
	int err;

	err = rxe_vhca_reader_init(&reader, data, length);
	if (err)
		return err;

	while ((err = rxe_vhca_read_record(&reader, &record)) > 0) {
		u32 ufile_id;

		if (record.type == RXE_VHCA_RECORD_RESP_RESOURCE) {
			if (!resource_count || record.flags ||
			    record.length != sizeof(struct rxe_vhca_resp_resource))
				return -EBADMSG;
			resource_count--;
			continue;
		}
		if (record.type == RXE_VHCA_RECORD_QP) {
			struct rxe_vhca_qp qp;

			if (cq_count || !qp_count || resource_count || record.flags ||
			    record.length != sizeof(qp))
				return -EBADMSG;
			memcpy(&qp, record.payload, sizeof(qp));
			resource_count =
				le32_to_cpu(qp.header.resp_resource_count);
			if (resource_count != qp.state.max_dest_rd_atomic)
				return -EBADMSG;
			qp_count--;
			continue;
		}
		if (record.type == RXE_VHCA_RECORD_CQ) {
			struct rxe_vhca_cq cq;

			if (!cq_count || record.flags ||
			    record.length != sizeof(cq))
				return -EBADMSG;
			memcpy(&cq, record.payload, sizeof(cq));
			if (le32_to_cpu(cq.notify) & ~IB_CQ_SOLICITED_MASK)
				return -EBADMSG;
			cq_count--;
			continue;
		}
		if (record.type != RXE_VHCA_RECORD_CONTEXT || record.flags ||
		    record.length != sizeof(context) || cq_count || qp_count ||
		    resource_count)
			return -EBADMSG;
		memcpy(&context, record.payload, sizeof(context));
		ufile_id = le32_to_cpu(context.ufile_id);
		if (!ufile_id || le32_to_cpu(context.reserved))
			return -EBADMSG;
		cq_count = le32_to_cpu(context.cq_count);
		qp_count = le32_to_cpu(context.qp_count);
		context_count++;
	}
	if (err)
		return err;
	if (cq_count || qp_count || resource_count)
		return -EBADMSG;

	return context_count ? 0 : -ENODATA;
}
