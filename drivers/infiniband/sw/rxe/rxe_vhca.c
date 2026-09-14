// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/xarray.h>

#include "rxe.h"
#include "rxe_vhca.h"

static_assert(sizeof(struct rxe_vhca_image_header) == 4);
static_assert(sizeof(struct rxe_vhca_record_header) == 16);
static_assert(sizeof(struct rxe_vhca_cq) == 8);
static_assert(sizeof(struct rxe_vhca_resp_resource) == 64);

struct rxe_vhca_staged_context {
	bool bound;
};

static int rxe_vhca_encode_ip(struct rxe_vhca_ip_address *image,
			      const void *address)
{
	const struct sockaddr *sa = address;

	memset(image, 0, sizeof(*image));
	image->family = cpu_to_le16(sa->sa_family);
	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *sin = (const void *)address;

		image->port = cpu_to_le16(ntohs(sin->sin_port));
		memcpy(image->address, &sin->sin_addr, sizeof(sin->sin_addr));
	} else if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *sin6 = (const void *)address;

		image->port = cpu_to_le16(ntohs(sin6->sin6_port));
		image->flowinfo = cpu_to_le32(ntohl(sin6->sin6_flowinfo));
		memcpy(image->address, &sin6->sin6_addr, sizeof(sin6->sin6_addr));
		image->scope_id = cpu_to_le32(sin6->sin6_scope_id);
	} else if (sa->sa_family) {
		return -EAFNOSUPPORT;
	}

	return 0;
}

static int rxe_vhca_decode_ip(const struct rxe_vhca_ip_address *image,
			      void *address)
{
	u16 family = le16_to_cpu(image->family);

	memset(address, 0, sizeof(struct sockaddr_in6));
	if (family == AF_INET) {
		struct sockaddr_in *sin = (void *)address;

		if (le32_to_cpu(image->flowinfo) || le32_to_cpu(image->scope_id) ||
		    memchr_inv(image->address + sizeof(sin->sin_addr), 0,
			       sizeof(image->address) - sizeof(sin->sin_addr)))
			return -EBADMSG;
		sin->sin_family = AF_INET;
		sin->sin_port = htons(le16_to_cpu(image->port));
		memcpy(&sin->sin_addr, image->address, sizeof(sin->sin_addr));
	} else if (family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (void *)address;

		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = htons(le16_to_cpu(image->port));
		sin6->sin6_flowinfo = htonl(le32_to_cpu(image->flowinfo));
		memcpy(&sin6->sin6_addr, image->address, sizeof(sin6->sin6_addr));
		sin6->sin6_scope_id = le32_to_cpu(image->scope_id);
	} else if (family) {
		return -EBADMSG;
	}

	return 0;
}

int rxe_vhca_encode_qp_state(struct rxe_vhca_qp_state *image,
			     const struct rxe_restore_qp_req *state)
{
	int err;

	memset(image, 0, sizeof(*image));
	image->av.port_num = state->av.port_num;
	image->av.network_type = state->av.network_type;
	memcpy(image->av.dmac, state->av.dmac, sizeof(image->av.dmac));
	memcpy(image->av.dgid, state->av.grh.dgid.raw, sizeof(image->av.dgid));
	image->av.flow_label = cpu_to_le32(state->av.grh.flow_label);
	image->av.sgid_index = state->av.grh.sgid_index;
	image->av.hop_limit = state->av.grh.hop_limit;
	image->av.traffic_class = state->av.grh.traffic_class;
	err = rxe_vhca_encode_ip(&image->av.sgid_addr, &state->av.sgid_addr);
	if (err)
		return err;
	err = rxe_vhca_encode_ip(&image->av.dgid_addr, &state->av.dgid_addr);
	if (err)
		return err;
	image->sq_vm_pgoff = cpu_to_le64(state->sq_vm_pgoff);
	image->rq_vm_pgoff = cpu_to_le64(state->rq_vm_pgoff);
	image->qpn = cpu_to_le32(state->qpn);
	image->dest_qp_num = cpu_to_le32(state->dest_qp_num);
	image->qkey = cpu_to_le32(state->qkey);
	image->sq_psn = cpu_to_le32(state->sq_psn);
	image->rq_psn = cpu_to_le32(state->rq_psn);
	image->qp_access_flags = cpu_to_le32(state->qp_access_flags);
	image->max_rd_atomic = cpu_to_le32(state->max_rd_atomic);
	image->max_dest_rd_atomic = cpu_to_le32(state->max_dest_rd_atomic);
	image->req_psn = cpu_to_le32(state->req_psn);
	image->comp_psn = cpu_to_le32(state->comp_psn);
	image->resp_psn = cpu_to_le32(state->resp_psn);
	image->resp_msn = cpu_to_le32(state->resp_msn);
	image->req_wqe_index = cpu_to_le32(state->req_wqe_index);
	image->ssn = cpu_to_le32(state->ssn);
	image->resp_ack_psn = cpu_to_le32(state->resp_ack_psn);
	image->resp_opcode = cpu_to_le32(state->resp_opcode);
	image->resp_status = cpu_to_le32(state->resp_status);
	image->res_head = cpu_to_le32(state->res_head);
	image->res_tail = cpu_to_le32(state->res_tail);
	image->pkey_index = cpu_to_le16(state->pkey_index);
	image->path_mtu = state->path_mtu;
	image->retry_cnt = state->retry_cnt;
	image->rnr_retry = state->rnr_retry;
	image->retry_cnt_left = state->retry_cnt_left;
	image->rnr_retry_left = state->rnr_retry_left;
	image->min_rnr_timer = state->min_rnr_timer;
	image->timeout = state->timeout;
	image->port_num = state->port_num;
	image->sq_sig_all = state->sq_sig_all;
	image->resp_aeth_syndrome = state->resp_aeth_syndrome;
	return 0;
}

int rxe_vhca_decode_qp_state(const struct rxe_vhca_qp_state *image,
			     struct rxe_restore_qp_req *state)
{
	int err;

	if (image->av.reserved || memchr_inv(image->reserved, 0,
					     sizeof(image->reserved)))
		return -EBADMSG;
	memset(state, 0, sizeof(*state));
	state->av.port_num = image->av.port_num;
	state->av.network_type = image->av.network_type;
	memcpy(state->av.dmac, image->av.dmac, sizeof(state->av.dmac));
	memcpy(state->av.grh.dgid.raw, image->av.dgid,
	       sizeof(state->av.grh.dgid.raw));
	state->av.grh.flow_label = le32_to_cpu(image->av.flow_label);
	state->av.grh.sgid_index = image->av.sgid_index;
	state->av.grh.hop_limit = image->av.hop_limit;
	state->av.grh.traffic_class = image->av.traffic_class;
	err = rxe_vhca_decode_ip(&image->av.sgid_addr, &state->av.sgid_addr);
	if (err)
		return err;
	err = rxe_vhca_decode_ip(&image->av.dgid_addr, &state->av.dgid_addr);
	if (err)
		return err;
	state->sq_vm_pgoff = le64_to_cpu(image->sq_vm_pgoff);
	state->rq_vm_pgoff = le64_to_cpu(image->rq_vm_pgoff);
	state->qpn = le32_to_cpu(image->qpn);
	state->dest_qp_num = le32_to_cpu(image->dest_qp_num);
	state->qkey = le32_to_cpu(image->qkey);
	state->sq_psn = le32_to_cpu(image->sq_psn);
	state->rq_psn = le32_to_cpu(image->rq_psn);
	state->qp_access_flags = le32_to_cpu(image->qp_access_flags);
	state->max_rd_atomic = le32_to_cpu(image->max_rd_atomic);
	state->max_dest_rd_atomic = le32_to_cpu(image->max_dest_rd_atomic);
	state->req_psn = le32_to_cpu(image->req_psn);
	state->comp_psn = le32_to_cpu(image->comp_psn);
	state->resp_psn = le32_to_cpu(image->resp_psn);
	state->resp_msn = le32_to_cpu(image->resp_msn);
	state->req_wqe_index = le32_to_cpu(image->req_wqe_index);
	state->ssn = le32_to_cpu(image->ssn);
	state->resp_ack_psn = le32_to_cpu(image->resp_ack_psn);
	state->resp_opcode = le32_to_cpu(image->resp_opcode);
	state->resp_status = le32_to_cpu(image->resp_status);
	state->res_head = le32_to_cpu(image->res_head);
	state->res_tail = le32_to_cpu(image->res_tail);
	state->pkey_index = le16_to_cpu(image->pkey_index);
	state->path_mtu = image->path_mtu;
	state->retry_cnt = image->retry_cnt;
	state->rnr_retry = image->rnr_retry;
	state->retry_cnt_left = image->retry_cnt_left;
	state->rnr_retry_left = image->rnr_retry_left;
	state->min_rnr_timer = image->min_rnr_timer;
	state->timeout = image->timeout;
	state->port_num = image->port_num;
	state->sq_sig_all = image->sq_sig_all;
	state->resp_aeth_syndrome = image->resp_aeth_syndrome;
	state->res_image_bytes = state->max_dest_rd_atomic *
				 sizeof(struct resp_res);
	return 0;
}

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

void rxe_vhca_clear_contexts(struct rxe_dev *rxe)
{
	struct rxe_vhca_staged_context *context;
	unsigned long index;

	xa_for_each(&rxe->vhca_contexts, index, context)
		kfree(context);
	xa_destroy(&rxe->vhca_contexts);
	xa_init(&rxe->vhca_contexts);
}

int rxe_vhca_index_contexts(struct rxe_dev *rxe)
{
	struct rxe_vhca_context_header header;
	struct rxe_vhca_record record;
	struct rxe_vhca_reader reader;
	int err;

	err = rxe_vhca_reader_init(&reader, rxe->vhca_image,
				   rxe->vhca_image_length);
	if (err)
		return err;

	while ((err = rxe_vhca_read_record(&reader, &record)) > 0) {
		struct rxe_vhca_staged_context *context;
		u32 ufile_id;

		if (record.type != RXE_VHCA_RECORD_CONTEXT)
			continue;
		memcpy(&header, record.payload, sizeof(header));
		ufile_id = le32_to_cpu(header.ufile_id);
		context = kzalloc_obj(*context);
		if (!context) {
			err = -ENOMEM;
			break;
		}
		err = xa_insert(&rxe->vhca_contexts, ufile_id, context,
				GFP_KERNEL);
		if (err) {
			kfree(context);
			break;
		}
	}
	if (err < 0)
		rxe_vhca_clear_contexts(rxe);

	return err < 0 ? err : 0;
}

int rxe_vhca_bind_context(struct rxe_dev *rxe, u32 ufile_id)
{
	struct rxe_vhca_staged_context *context;

	context = xa_load(&rxe->vhca_contexts, ufile_id);
	if (!context)
		return -ENOENT;
	if (context->bound)
		return -EALREADY;

	context->bound = true;
	return 0;
}

void rxe_vhca_unbind_context(struct rxe_dev *rxe, u32 ufile_id)
{
	struct rxe_vhca_staged_context *context;

	context = xa_load(&rxe->vhca_contexts, ufile_id);
	if (context)
		context->bound = false;
}

int rxe_vhca_find_cq(void *data, size_t length, u32 ufile_id,
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
		if (match && le32_to_cpu(cq->uobject_handle) == uobject_handle) {
			struct rxe_vhca_record_header *header;

			header = (void *)(record.payload - sizeof(*header));
			if (le32_to_cpu(header->flags) & RXE_VHCA_RECORD_F_CONSUMED)
				return -EALREADY;
			header->flags = cpu_to_le32(RXE_VHCA_RECORD_F_CONSUMED);
			return 0;
		}
	}

	return err ?: -ENOENT;
}

int rxe_vhca_find_qp(void *data, size_t length, u32 ufile_id,
			     u32 uobject_handle, struct rxe_restore_qp_req *state,
			     struct resp_res **resources,
			     struct rxe_vhca_qp_timers *timers)
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
		struct rxe_vhca_record_header *qp_record_header;
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
		qp_record_header = (void *)(record.payload -
					   sizeof(*qp_record_header));
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
			err = rxe_vhca_decode_qp_state(&qp.state, state);
			if (err)
				goto err_free;
			timers->retrans_pending = qp.state.retrans_pending;
			timers->rnr_pending = qp.state.rnr_pending;
			timers->retrans_remaining_ns =
				le64_to_cpu(qp.state.retrans_remaining_ns);
			timers->rnr_remaining_ns =
				le64_to_cpu(qp.state.rnr_remaining_ns);
			if (le32_to_cpu(qp_record_header->flags) &
			    RXE_VHCA_RECORD_F_CONSUMED) {
				err = -EALREADY;
				goto err_free;
			}
			qp_record_header->flags =
				cpu_to_le32(RXE_VHCA_RECORD_F_CONSUMED);
			return 0;
		}
	}

	return err ?: -ENOENT;

err_free:
	kfree(*resources);
	*resources = NULL;
	return err;
}

bool rxe_vhca_all_objects_consumed(const void *data, size_t length)
{
	struct rxe_vhca_record record;
	struct rxe_vhca_reader reader;
	int err;

	if (rxe_vhca_reader_init(&reader, data, length))
		return false;
	while ((err = rxe_vhca_read_record(&reader, &record)) > 0) {
		if ((record.type == RXE_VHCA_RECORD_CQ ||
		     record.type == RXE_VHCA_RECORD_QP) &&
		    !(record.flags & RXE_VHCA_RECORD_F_CONSUMED))
			return false;
	}

	return err == 0;
}

int rxe_vhca_validate_contexts(const void *data, size_t length)
{
	struct rxe_vhca_context_header context;
	struct rxe_vhca_record record;
	struct rxe_vhca_reader reader;
	DEFINE_XARRAY(context_ids);
	DEFINE_XARRAY(cq_ids);
	DEFINE_XARRAY(qp_ids);
	unsigned long *resource_slots = NULL;
	u32 resource_slot_count = 0;
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
			struct rxe_vhca_resp_resource image_resource;
			struct resp_res resource;
			u32 slots;
			u32 slot;

			if (!resource_count || record.flags ||
			    record.length != sizeof(image_resource)) {
				err = -EBADMSG;
				goto out;
			}
			memcpy(&image_resource, record.payload,
			       sizeof(image_resource));
			slot = le32_to_cpu(image_resource.slot);
			if (slot >= resource_slot_count ||
			    test_and_set_bit(slot, resource_slots)) {
				err = -EBADMSG;
				goto out;
			}
			slots = resource_slot_count;
			err = rxe_vhca_decode_resp_resource(&image_resource, slots, &resource);
			if (err)
				goto out;
			resource_count--;
			continue;
		}
		if (record.type == RXE_VHCA_RECORD_QP) {
			struct rxe_vhca_qp qp;

			if (cq_count || !qp_count || resource_count || record.flags ||
			    record.length != sizeof(qp))
				goto bad_image;
			memcpy(&qp, record.payload, sizeof(qp));
			err = xa_insert(&qp_ids,
					le32_to_cpu(qp.header.uobject_handle),
					XA_ZERO_ENTRY, GFP_KERNEL);
			if (err) {
				err = err == -EBUSY ? -EBADMSG : err;
				goto out;
			}
			if (qp.state.retrans_pending > 1 || qp.state.rnr_pending > 1 ||
			    (!qp.state.retrans_pending &&
			     le64_to_cpu(qp.state.retrans_remaining_ns)) ||
			    (!qp.state.rnr_pending &&
			     le64_to_cpu(qp.state.rnr_remaining_ns)))
				goto bad_image;
			resource_count =
				le32_to_cpu(qp.header.resp_resource_count);
			resource_slot_count = resource_count;
			bitmap_free(resource_slots);
			resource_slots = bitmap_zalloc(resource_slot_count, GFP_KERNEL);
			if (resource_slot_count && !resource_slots) {
				err = -ENOMEM;
				goto out;
			}
			if (resource_count !=
			    le32_to_cpu(qp.state.max_dest_rd_atomic))
				goto bad_image;
			qp_count--;
			continue;
		}
		if (record.type == RXE_VHCA_RECORD_CQ) {
			struct rxe_vhca_cq cq;

			if (!cq_count || record.flags ||
			    record.length != sizeof(cq))
				goto bad_image;
			memcpy(&cq, record.payload, sizeof(cq));
			err = xa_insert(&cq_ids, le32_to_cpu(cq.uobject_handle),
					XA_ZERO_ENTRY, GFP_KERNEL);
			if (err) {
				err = err == -EBUSY ? -EBADMSG : err;
				goto out;
			}
			if (le32_to_cpu(cq.notify) & ~IB_CQ_SOLICITED_MASK)
				goto bad_image;
			cq_count--;
			continue;
		}
		if (record.type != RXE_VHCA_RECORD_CONTEXT || record.flags ||
		    record.length != sizeof(context) || cq_count || qp_count ||
		    resource_count)
			goto bad_image;
		memcpy(&context, record.payload, sizeof(context));
		ufile_id = le32_to_cpu(context.ufile_id);
		if (!ufile_id || le32_to_cpu(context.reserved))
			goto bad_image;
		err = xa_insert(&context_ids, ufile_id, XA_ZERO_ENTRY,
				GFP_KERNEL);
		if (err) {
			err = err == -EBUSY ? -EBADMSG : err;
			goto out;
		}
		xa_destroy(&cq_ids);
		xa_destroy(&qp_ids);
		cq_count = le32_to_cpu(context.cq_count);
		qp_count = le32_to_cpu(context.qp_count);
		context_count++;
	}
	if (err)
		goto out;
	if (cq_count || qp_count || resource_count)
		goto bad_image;
	err = context_count ? 0 : -ENODATA;
	goto out;

bad_image:
	err = -EBADMSG;
out:
	bitmap_free(resource_slots);
	xa_destroy(&qp_ids);
	xa_destroy(&cq_ids);
	xa_destroy(&context_ids);
	return err;
}
