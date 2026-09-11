// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2026 rxe CRIU migration. All rights reserved.
 *
 * Driver-private RXE_IB_OBJECT_MIGRATE uverbs object: the RXE arm of the
 * CRIU checkpoint and restore choreography. Generic object creation lives
 * in UVERBS_OBJECT_RESTORE; this object carries RXE-private queries,
 * datapath gating, and late context finalization.
 *
 * The object carries FREEZE_DATAPATH and FREEZE_CONTEXT plus the
 * dump-side query verbs QUERY_QP and QUERY_CQ, the counterparts to
 * UVERBS_METHOD_RESTORE_QP / RESTORE_CQ. FREEZE_DATAPATH parks one QP;
 * FREEZE_CONTEXT parks every user QP owned by the calling uverbs fd.
 */

#include <rdma/uverbs_ioctl.h>
#include <rdma/uverbs_std_types.h>
#include <rdma/uverbs_types.h>
#include <rdma/rxe_user_ioctl_cmds.h>

#include "rxe.h"
#include "rxe_queue.h"
#include "rxe_vhca.h"

#define UVERBS_MODULE_NAME rdma_rxe
#include <rdma/uverbs_named_ioctl.h>

#define RXE_VHCA_MAX_IMAGE_LENGTH	SZ_1G

enum rxe_vhca_stream_mode {
	RXE_VHCA_STREAM_SAVE,
	RXE_VHCA_STREAM_LOAD,
};

struct rxe_vhca_stream {
	struct ib_uobject uobject;
	struct mutex lock; /* protects stream state and contents */
	struct rxe_dev *rxe;
	void *data;
	size_t length;
	size_t written;
	enum rxe_vhca_stream_mode mode;
	bool committed;
};

static void rxe_snapshot_qp(struct rxe_qp *qp,
			    struct rxe_restore_qp_req *state)
{
	memset(state, 0, sizeof(*state));
	state->qpn = qp->ibqp.qp_num;
	if (qp->sq.queue->ip)
		state->sq_vm_pgoff = qp->sq.queue->ip->info.offset;
	if (qp->rq.queue && qp->rq.queue->ip)
		state->rq_vm_pgoff = qp->rq.queue->ip->info.offset;
	state->sq_queue_size = qp->sq.queue->buf_size;
	if (qp->rq.queue)
		state->rq_queue_size = qp->rq.queue->buf_size;
	memcpy(&state->av, &qp->pri_av, sizeof(state->av));
	state->dest_qp_num = qp->attr.dest_qp_num;
	state->qkey = qp->attr.qkey;
	state->sq_psn = qp->attr.sq_psn;
	state->rq_psn = qp->attr.rq_psn;
	state->qp_access_flags = qp->attr.qp_access_flags;
	state->max_rd_atomic = qp->attr.max_rd_atomic;
	state->max_dest_rd_atomic = qp->attr.max_dest_rd_atomic;
	state->pkey_index = qp->attr.pkey_index;
	state->path_mtu = qp->attr.path_mtu;
	state->retry_cnt = qp->attr.retry_cnt;
	state->rnr_retry = qp->attr.rnr_retry;
	state->retry_cnt_left = qp->comp.retry_cnt;
	state->rnr_retry_left = qp->comp.rnr_retry;
	state->min_rnr_timer = qp->attr.min_rnr_timer;
	state->timeout = qp->attr.timeout;
	state->port_num = qp->attr.port_num;
	state->sq_sig_all = qp->sq_sig_type == IB_SIGNAL_ALL_WR;
	state->req_psn = qp->req.psn;
	state->comp_psn = qp->comp.psn;
	state->resp_psn = qp->resp.psn;
	state->resp_msn = qp->resp.msn;
	state->req_wqe_index = qp->req.wqe_index;
	state->ssn = atomic_read(&qp->ssn);
	state->resp_ack_psn = qp->resp.ack_psn;
	state->resp_opcode = qp->resp.opcode;
	state->resp_status = qp->resp.status;
	state->resp_aeth_syndrome = qp->resp.aeth_syndrome;
	state->res_head = qp->resp.res_head;
	state->res_tail = qp->resp.res_tail;
	if (qp->resp.resources && qp->attr.max_dest_rd_atomic)
		state->res_image_bytes = qp->attr.max_dest_rd_atomic *
					 sizeof(struct resp_res);
}

static int rxe_vhca_build_context_image(struct rxe_dev *rxe, void **data, size_t *length)
{
	struct rxe_vhca_context_header context = {};
	struct rxe_vhca_writer writer;
	struct rxe_pool_elem *elem;
	unsigned long index = 0;
	size_t image_length;
	size_t cq_image_length;
	size_t qp_image_length = 0;
	u32 context_count = 0;
	u32 cq_count = 0;
	void *image;
	int err;

	mutex_lock(&rxe->vhca_lock);
	rcu_read_lock();
	for (elem = xa_find(&rxe->uc_pool.xa, &index, ULONG_MAX, XA_PRESENT);
	     elem;
	     elem = xa_find_after(&rxe->uc_pool.xa, &index, ULONG_MAX,
				  XA_PRESENT)) {
		struct rxe_ucontext *uc = elem->obj;

		if (!uc->migration_registered) {
			err = -EINVAL;
			goto out_rcu;
		}
		{
			struct rxe_pool_elem *cq_elem;
			unsigned long cq_index = 0;

			for (cq_elem = xa_find(&rxe->cq_pool.xa, &cq_index,
					       ULONG_MAX, XA_PRESENT);
			     cq_elem;
			     cq_elem = xa_find_after(&rxe->cq_pool.xa, &cq_index,
						     ULONG_MAX, XA_PRESENT)) {
				struct rxe_cq *cq = cq_elem->obj;

				if (ib_cq_ucontext(&cq->ibcq) != &uc->ibuc)
					continue;
				if (!cq->migration_captured) {
					err = -EINVAL;
					goto out_rcu;
				}
				cq_count++;
			}
		}
		{
			struct rxe_pool_elem *qp_elem;
			unsigned long qp_index = 0;

			for (qp_elem = xa_find(&rxe->qp_pool.xa, &qp_index,
					       ULONG_MAX, XA_PRESENT);
			     qp_elem;
			     qp_elem = xa_find_after(&rxe->qp_pool.xa, &qp_index,
						     ULONG_MAX, XA_PRESENT)) {
				struct rxe_qp *qp = qp_elem->obj;
				size_t resource_length;
				size_t qp_length;

				if (ib_qp_ucontext(&qp->ibqp) != &uc->ibuc)
					continue;
				if (!qp->migration_captured) {
					err = -EINVAL;
					goto out_rcu;
				}
				resource_length = sizeof(struct rxe_vhca_record_header) +
						  sizeof(struct rxe_vhca_resp_resource);
				if (check_mul_overflow((size_t)qp->attr.max_dest_rd_atomic,
						       resource_length, &qp_length) ||
				    check_add_overflow(qp_length,
						       sizeof(struct rxe_vhca_record_header) +
						       sizeof(struct rxe_vhca_qp),
						       &qp_length) ||
				    check_add_overflow(qp_image_length, qp_length,
						       &qp_image_length)) {
					err = -EOVERFLOW;
					goto out_rcu;
				}
			}
		}
		context_count++;
	}
	rcu_read_unlock();

	if (!context_count) {
		err = -ENODATA;
		goto out_unlock;
	}
	if (check_mul_overflow((size_t)cq_count,
			       sizeof(struct rxe_vhca_record_header) +
			       sizeof(struct rxe_vhca_cq),
			       &cq_image_length) ||
	    check_mul_overflow((size_t)context_count,
			       sizeof(struct rxe_vhca_record_header) +
			       sizeof(context), &image_length) ||
	    check_add_overflow(image_length,
			       sizeof(struct rxe_vhca_image_header),
			       &image_length) ||
	    check_add_overflow(image_length, cq_image_length, &image_length) ||
	    check_add_overflow(image_length, qp_image_length, &image_length)) {
		err = -EOVERFLOW;
		goto out_unlock;
	}

	image = kvmalloc(image_length, GFP_KERNEL);
	if (!image) {
		err = -ENOMEM;
		goto out_unlock;
	}
	err = rxe_vhca_writer_init(&writer, image, image_length);
	if (err)
		goto out_free;

	index = 0;
	rcu_read_lock();
	for (elem = xa_find(&rxe->uc_pool.xa, &index, ULONG_MAX, XA_PRESENT);
	     elem;
	     elem = xa_find_after(&rxe->uc_pool.xa, &index, ULONG_MAX,
				  XA_PRESENT)) {
		struct rxe_ucontext *uc = elem->obj;
		struct rxe_pool_elem *cq_elem;
		unsigned long cq_index = 0;
		u32 uc_cq_count = 0;
		struct rxe_pool_elem *qp_elem;
		unsigned long qp_index = 0;
		u32 uc_qp_count = 0;

		for (cq_elem = xa_find(&rxe->cq_pool.xa, &cq_index, ULONG_MAX,
				       XA_PRESENT);
		     cq_elem;
		     cq_elem = xa_find_after(&rxe->cq_pool.xa, &cq_index,
					     ULONG_MAX, XA_PRESENT)) {
			struct rxe_cq *cq = cq_elem->obj;

			if (ib_cq_ucontext(&cq->ibcq) == &uc->ibuc &&
			    cq->migration_captured)
				uc_cq_count++;
		}
		for (qp_elem = xa_find(&rxe->qp_pool.xa, &qp_index, ULONG_MAX,
				       XA_PRESENT);
		     qp_elem;
		     qp_elem = xa_find_after(&rxe->qp_pool.xa, &qp_index,
					     ULONG_MAX, XA_PRESENT)) {
			struct rxe_qp *qp = qp_elem->obj;

			if (ib_qp_ucontext(&qp->ibqp) == &uc->ibuc &&
			    qp->migration_captured)
				uc_qp_count++;
		}

		context.ufile_id = cpu_to_le32(uc->migration_ufile_id);
		context.cq_count = cpu_to_le32(uc_cq_count);
		context.qp_count = cpu_to_le32(uc_qp_count);
		err = rxe_vhca_write_record(&writer, RXE_VHCA_RECORD_CONTEXT, 0,
					    &context, sizeof(context));
		if (err)
			goto out_free_rcu;

		cq_index = 0;
		for (cq_elem = xa_find(&rxe->cq_pool.xa, &cq_index, ULONG_MAX,
				       XA_PRESENT);
		     cq_elem;
		     cq_elem = xa_find_after(&rxe->cq_pool.xa, &cq_index,
					     ULONG_MAX, XA_PRESENT)) {
			struct rxe_vhca_cq image_cq = {};
			struct rxe_cq *cq = cq_elem->obj;

			if (ib_cq_ucontext(&cq->ibcq) != &uc->ibuc ||
			    !cq->migration_captured)
				continue;
			image_cq.uobject_handle =
				cpu_to_le32(cq->migration_uobject_handle);
			spin_lock_irq(&cq->cq_lock);
			image_cq.notify = cpu_to_le32(cq->notify);
			spin_unlock_irq(&cq->cq_lock);
			err = rxe_vhca_write_record(&writer, RXE_VHCA_RECORD_CQ,
						    0, &image_cq,
						    sizeof(image_cq));
			if (err)
				goto out_free_rcu;
		}

		qp_index = 0;
		for (qp_elem = xa_find(&rxe->qp_pool.xa, &qp_index, ULONG_MAX,
				       XA_PRESENT);
		     qp_elem;
		     qp_elem = xa_find_after(&rxe->qp_pool.xa, &qp_index,
					     ULONG_MAX, XA_PRESENT)) {
			struct rxe_vhca_qp image_qp = {};
			struct rxe_qp *qp = qp_elem->obj;
			u32 i;

			if (ib_qp_ucontext(&qp->ibqp) != &uc->ibuc ||
			    !qp->migration_captured)
				continue;
			image_qp.header.uobject_handle =
				cpu_to_le32(qp->migration_uobject_handle);
			image_qp.header.resp_resource_count =
				cpu_to_le32(qp->attr.max_dest_rd_atomic);
			rxe_snapshot_qp(qp, &qp->migration_state);
			err = rxe_vhca_encode_qp_state(&image_qp.state,
						       &qp->migration_state);
			if (err)
				goto out_free_rcu;
			if (timer_pending(&qp->retrans_timer)) {
				unsigned long remaining = 0;

				if (time_after(qp->retrans_timer.expires, jiffies))
					remaining = qp->retrans_timer.expires - jiffies;
				image_qp.state.retrans_pending = 1;
				image_qp.state.retrans_remaining_ns =
					cpu_to_le64(jiffies_to_nsecs(remaining));
			}
			if (timer_pending(&qp->rnr_nak_timer)) {
				unsigned long remaining = 0;

				if (time_after(qp->rnr_nak_timer.expires, jiffies))
					remaining = qp->rnr_nak_timer.expires - jiffies;
				image_qp.state.rnr_pending = 1;
				image_qp.state.rnr_remaining_ns =
					cpu_to_le64(jiffies_to_nsecs(remaining));
			}
			err = rxe_vhca_write_record(&writer, RXE_VHCA_RECORD_QP, 0,
						    &image_qp,
						    sizeof(image_qp));
			if (err)
				goto out_free_rcu;
			for (i = 0; i < qp->attr.max_dest_rd_atomic; i++) {
				struct rxe_vhca_resp_resource resource;
				struct resp_res *resp_resource;
				u32 type = RXE_VHCA_RECORD_RESP_RESOURCE;

				resp_resource = &qp->resp.resources[i];
				err = rxe_vhca_encode_resp_resource(&resource, i, resp_resource);
				if (err)
					goto out_free_rcu;
				err = rxe_vhca_write_record(&writer, type, 0, &resource,
							    sizeof(resource));
				if (err)
					goto out_free_rcu;
			}
		}
	}
	rcu_read_unlock();

	*data = image;
	*length = writer.length;
	mutex_unlock(&rxe->vhca_lock);
	return 0;

out_free_rcu:
	rcu_read_unlock();
out_free:
	kvfree(image);
out_unlock:
	mutex_unlock(&rxe->vhca_lock);
	return err;
out_rcu:
	rcu_read_unlock();
	goto out_unlock;
}

static ssize_t rxe_vhca_stream_read(struct file *file, char __user *buffer,
				    size_t length, loff_t *offset)
{
	struct ib_uobject *uobject = file->private_data;
	struct rxe_vhca_stream *stream;

	if (!uobject)
		return -EBADF;

	stream = container_of(uobject, struct rxe_vhca_stream, uobject);
	if (stream->mode != RXE_VHCA_STREAM_SAVE)
		return -EBADF;

	return simple_read_from_buffer(buffer, length, offset, stream->data,
				       stream->length);
}

static ssize_t rxe_vhca_stream_write(struct file *file,
				     const char __user *buffer, size_t length,
				     loff_t *offset)
{
	struct ib_uobject *uobject = file->private_data;
	struct rxe_vhca_stream *stream;
	ssize_t ret;

	if (!uobject)
		return -EBADF;

	stream = container_of(uobject, struct rxe_vhca_stream, uobject);
	if (stream->mode != RXE_VHCA_STREAM_LOAD)
		return -EBADF;

	mutex_lock(&stream->lock);
	if (stream->committed) {
		ret = -EBUSY;
		goto out;
	}
	if (*offset != stream->written) {
		ret = -ESPIPE;
		goto out;
	}
	if (length > stream->length - stream->written) {
		ret = -EFBIG;
		goto out;
	}
	if (copy_from_user((u8 *)stream->data + stream->written, buffer,
			   length)) {
		ret = -EFAULT;
		goto out;
	}

	stream->written += length;
	*offset += length;
	ret = length;
out:
	mutex_unlock(&stream->lock);
	return ret;
}

static const struct file_operations rxe_vhca_stream_fops = {
	.owner = THIS_MODULE,
	.read = rxe_vhca_stream_read,
	.write = rxe_vhca_stream_write,
	.llseek = noop_llseek,
	.release = uverbs_uobject_fd_release,
};

static void rxe_vhca_stream_destroy(struct ib_uobject *uobject,
				    enum rdma_remove_reason why)
{
	struct rxe_vhca_stream *stream =
		container_of(uobject, struct rxe_vhca_stream, uobject);

	kvfree(stream->data);
	mutex_destroy(&stream->lock);
}

static int
UVERBS_HANDLER(RXE_IB_METHOD_CREATE_SAVE_FD)(struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobject;
	struct rxe_vhca_stream *stream =
		NULL;
	struct ib_device *ibdev;
	const u16 handle_attr = RXE_IB_ATTR_CREATE_SAVE_FD_HANDLE;
	int err;

	ibdev = uverbs_attr_get_ibdev(attrs);
	if (!ibdev)
		return -ENODEV;

	uobject = uverbs_attr_get_uobject(attrs, handle_attr);
	stream = container_of(uobject, struct rxe_vhca_stream, uobject);
	err = rxe_vhca_build_context_image(to_rdev(ibdev), &stream->data,
					   &stream->length);
	if (err)
		return err;

	mutex_init(&stream->lock);
	stream->rxe = to_rdev(ibdev);
	stream->mode = RXE_VHCA_STREAM_SAVE;
	uverbs_finalize_uobj_create(attrs, RXE_IB_ATTR_CREATE_SAVE_FD_HANDLE);

	return 0;
}

static int
UVERBS_HANDLER(RXE_IB_METHOD_REGISTER_CONTEXT)(struct uverbs_attr_bundle *attrs)
{
	struct ib_ucontext *ucontext = ib_uverbs_get_ucontext(attrs);
	struct rxe_ucontext *uc;
	struct rxe_pool_elem *elem;
	unsigned long index = 0;
	struct rxe_dev *rxe;
	u32 ufile_id;
	int err;

	if (IS_ERR(ucontext))
		return PTR_ERR(ucontext);
	err = uverbs_copy_from(&ufile_id, attrs,
			       RXE_IB_ATTR_REGISTER_CONTEXT_UFILE_ID);
	if (err)
		return err;
	if (!ufile_id)
		return -EINVAL;

	uc = to_ruc(ucontext);
	rxe = to_rdev(ucontext->device);
	mutex_lock(&rxe->vhca_lock);
	rcu_read_lock();
	for (elem = xa_find(&rxe->uc_pool.xa, &index, ULONG_MAX, XA_PRESENT);
	     elem;
	     elem = xa_find_after(&rxe->uc_pool.xa, &index, ULONG_MAX,
				  XA_PRESENT)) {
		struct rxe_ucontext *other = elem->obj;

		if (other != uc && other->migration_registered &&
		    other->migration_ufile_id == ufile_id) {
			err = -EEXIST;
			goto out_rcu;
		}
	}
	rcu_read_unlock();
	if (uc->migration_registered && uc->migration_ufile_id != ufile_id) {
		err = -EALREADY;
	} else {
		uc->migration_ufile_id = ufile_id;
		uc->migration_registered = true;
		err = 0;
	}
	mutex_unlock(&rxe->vhca_lock);

	return err;
out_rcu:
	rcu_read_unlock();
	mutex_unlock(&rxe->vhca_lock);
	return err;
}

static int
UVERBS_HANDLER(RXE_IB_METHOD_CREATE_LOAD_FD)(struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobject;
	struct rxe_vhca_stream *stream =
		NULL;
	struct ib_device *ibdev;
	const u16 handle_attr = RXE_IB_ATTR_CREATE_LOAD_FD_HANDLE;
	u64 length;
	int err;

	err = uverbs_copy_from(&length, attrs,
			       RXE_IB_ATTR_CREATE_LOAD_FD_LENGTH);
	if (err)
		return err;
	if (length < sizeof(struct rxe_vhca_image_header) ||
	    length > RXE_VHCA_MAX_IMAGE_LENGTH || length > SIZE_MAX)
		return -EINVAL;

	ibdev = uverbs_attr_get_ibdev(attrs);
	if (!ibdev)
		return -ENODEV;

	uobject = uverbs_attr_get_uobject(attrs, handle_attr);
	stream = container_of(uobject, struct rxe_vhca_stream, uobject);
	stream->data = kvzalloc(length, GFP_KERNEL);
	if (!stream->data)
		return -ENOMEM;

	mutex_init(&stream->lock);
	stream->rxe = to_rdev(ibdev);
	stream->length = length;
	stream->mode = RXE_VHCA_STREAM_LOAD;
	uverbs_finalize_uobj_create(attrs, RXE_IB_ATTR_CREATE_LOAD_FD_HANDLE);

	return 0;
}

static int
UVERBS_HANDLER(RXE_IB_METHOD_LOAD_VHCA)(struct uverbs_attr_bundle *attrs)
{
	const u16 handle_attr = RXE_IB_ATTR_LOAD_VHCA_HANDLE;
	struct rxe_vhca_stream *stream;
	struct ib_uobject *uobject;
	struct ib_device *ibdev;
	struct rxe_dev *rxe;
	int err;

	ibdev = uverbs_attr_get_ibdev(attrs);
	if (!ibdev)
		return -ENODEV;
	rxe = to_rdev(ibdev);
	uobject = uverbs_attr_get_uobject(attrs, handle_attr);
	stream = container_of(uobject, struct rxe_vhca_stream, uobject);

	mutex_lock(&stream->lock);
	if (stream->mode != RXE_VHCA_STREAM_LOAD || stream->rxe != rxe) {
		err = -EXDEV;
		goto out_stream;
	}
	if (stream->committed) {
		err = -EALREADY;
		goto out_stream;
	}
	if (stream->written != stream->length) {
		err = -ENODATA;
		goto out_stream;
	}

	err = rxe_vhca_validate_contexts(stream->data, stream->length);
	if (err)
		goto out_stream;

	mutex_lock(&rxe->vhca_lock);
	if (rxe->vhca_image || !xa_empty(&rxe->uc_pool.xa)) {
		err = -EBUSY;
		goto out_vhca;
	}

	rxe->vhca_image = stream->data;
	rxe->vhca_image_length = stream->length;
	err = rxe_vhca_index_contexts(rxe);
	if (err) {
		rxe->vhca_image = NULL;
		rxe->vhca_image_length = 0;
		goto out_vhca;
	}
	stream->data = NULL;
	stream->committed = true;
	err = 0;
out_vhca:
	mutex_unlock(&rxe->vhca_lock);
out_stream:
	mutex_unlock(&stream->lock);
	return err;
}

/*
 * RXE freeze drops packets that race with the context gate. Only RC has
 * retransmission and duplicate suppression to recover them safely.
 */
static int rxe_migrate_chk_qp_type(const struct ib_qp *ibqp)
{
	if (ibqp->qp_type == IB_QPT_RC)
		return 0;

	return -EOPNOTSUPP;
}

/*
 * FREEZE_DATAPATH: non-destructively park (freeze=1) or unpark
 * (freeze=0) a user QP's datapath so the dumper can take a coherent
 * PSN/cursor snapshot. rxe_qp_pause/resume are idempotent, so a repeated
 * freeze or thaw is harmless.
 */
static int UVERBS_HANDLER(RXE_IB_METHOD_FREEZE_DATAPATH)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_ucontext *ucontext;
	struct ib_qp *ibqp;
	struct rxe_qp *qp;
	u8 freeze;
	int err;

	ibqp = uverbs_attr_get_obj(attrs,
				   RXE_IB_ATTR_FREEZE_DATAPATH_QP_HANDLE);
	if (IS_ERR(ibqp))
		return PTR_ERR(ibqp);

	err = rxe_migrate_chk_qp_type(ibqp);
	if (err)
		return err;

	err = uverbs_copy_from(&freeze, attrs,
			       RXE_IB_ATTR_FREEZE_DATAPATH_FREEZE);
	if (err)
		return err;
	ucontext = ib_qp_ucontext(ibqp);
	if (!freeze && ucontext && to_ruc(ucontext)->restore_mode &&
	    !READ_ONCE(to_ruc(ucontext)->restore_finalized))
		return -EAGAIN;

	qp = to_rqp(ibqp);

	/* Only user QPs carry the datapath CRIU freezes. */
	if (!qp->is_user)
		return -ENXIO;

	if (freeze)
		rxe_qp_pause(qp);
	else
		rxe_qp_resume(qp);

	return 0;
}

/*
 * Ucontext-scoped freeze-all: pause (or resume) every user QP owned by
 * the calling uverbs fd in a single call, for CRIU's early
 * CHECKPOINT_DEVICES hook (one ioctl, before per-QP fds are dumped).
 *
 * A driver module cannot reach the core-internal ufile object walk, so
 * the QP set is enumerated from rxe's own QP pool and filtered by owning
 * ucontext. rxe_qp_pause() drains the worker tasks and can sleep, so we
 * take a pool reference on each element under RCU and run the
 * pause/resume outside the read-side critical section.
 */
static int UVERBS_HANDLER(RXE_IB_METHOD_FREEZE_CONTEXT)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_ucontext *ucontext = ib_uverbs_get_ucontext(attrs);
	struct rxe_pool_elem *elem;
	unsigned long index = 0;
	struct rxe_pool *pool;
	struct rxe_dev *rxe;
	int ret = 0;
	u8 freeze;
	int err;

	if (IS_ERR(ucontext))
		return PTR_ERR(ucontext);

	err = uverbs_copy_from(&freeze, attrs,
			       RXE_IB_ATTR_FREEZE_CONTEXT_FREEZE);
	if (err)
		return err;
	if (!freeze && to_ruc(ucontext)->restore_mode &&
	    !READ_ONCE(to_ruc(ucontext)->restore_finalized))
		return -EAGAIN;

	rxe = to_rdev(ucontext->device);
	pool = &rxe->qp_pool;

	rcu_read_lock();
	for (elem = xa_find(&pool->xa, &index, ULONG_MAX, XA_PRESENT);
	     elem;
	     elem = xa_find_after(&pool->xa, &index, ULONG_MAX, XA_PRESENT)) {
		struct rxe_qp *qp = elem->obj;

		/* Pin across the (sleeping) pause; skip elems being freed. */
		if (!kref_get_unless_zero(&elem->ref_cnt))
			continue;
		rcu_read_unlock();

		/*
		 * Only this ucontext's user QPs. Skip kernel QPs (no user
		 * datapath) and QPs owned by other processes sharing the
		 * device. Freezing is non-destructive (no IBTA transition),
		 * so unlike the per-QP QUERY path there is no type gate.
		 */
		if (qp->is_user && ib_qp_ucontext(&qp->ibqp) == ucontext) {
			if (freeze)
				rxe_qp_pause(qp);
			else if (to_ruc(ucontext)->restore_mode &&
				 !qp->restore_finalized)
				ret = -EAGAIN;
			else
				rxe_qp_resume(qp);
		}

		rxe_put(qp);
		rcu_read_lock();
	}
	rcu_read_unlock();

	return ret;
}

static int rxe_resume_vhca_cqs(struct rxe_dev *rxe)
{
	struct rxe_pool *pool = &rxe->cq_pool;
	struct rxe_pool_elem *elem;
	unsigned long index = 0;
	int err = 0;

	rcu_read_lock();
	for (elem = xa_find(&pool->xa, &index, ULONG_MAX, XA_PRESENT);
	     elem;
	     elem = xa_find_after(&pool->xa, &index, ULONG_MAX, XA_PRESENT)) {
		struct rxe_cq *cq = elem->obj;
		unsigned long flags;

		if (!kref_get_unless_zero(&elem->ref_cnt))
			continue;
		rcu_read_unlock();

		if (cq->is_user && ib_cq_ucontext(&cq->ibcq) &&
		    to_ruc(ib_cq_ucontext(&cq->ibcq))->restore_mode) {
			if (!cq->queue) {
				err = -EINVAL;
			} else {
				spin_lock_irqsave(&cq->cq_lock, flags);
				if (cq->restore_finalized) {
					err = 0;
				} else if (!cq->restore_pending) {
					err = -EINVAL;
				} else {
					err = rxe_queue_sync_for_restore(cq->queue);
					if (!err) {
						cq->notify = cq->restore_notify;
						cq->restore_pending = false;
						cq->restore_finalized = true;
					}
				}
				spin_unlock_irqrestore(&cq->cq_lock, flags);
			}
		}

		rxe_put(cq);
		if (err)
			return err;
		rcu_read_lock();
	}
	rcu_read_unlock();

	return 0;
}

static int rxe_resume_vhca_qps(struct rxe_dev *rxe)
{
	struct rxe_pool *pool = &rxe->qp_pool;
	struct rxe_pool_elem *elem;
	unsigned long index = 0;
	int err = 0;

	rcu_read_lock();
	for (elem = xa_find(&pool->xa, &index, ULONG_MAX, XA_PRESENT);
	     elem;
	     elem = xa_find_after(&pool->xa, &index, ULONG_MAX, XA_PRESENT)) {
		struct rxe_qp *qp = elem->obj;
		unsigned long flags;

		if (!kref_get_unless_zero(&elem->ref_cnt))
			continue;
		rcu_read_unlock();

		if (!qp->is_user || !ib_qp_ucontext(&qp->ibqp) ||
		    !to_ruc(ib_qp_ucontext(&qp->ibqp))->restore_mode)
			goto next;

		spin_lock_irqsave(&qp->state_lock, flags);
		if (!qp->dp_frozen)
			err = -EINVAL;
		spin_unlock_irqrestore(&qp->state_lock, flags);
		if (err)
			goto next;

		if (qp->sq.queue)
			err = rxe_queue_sync_for_restore(qp->sq.queue);
		if (!err && qp->rq.queue && !qp->srq)
			err = rxe_queue_sync_for_restore(qp->rq.queue);
		if (!err)
			err = rxe_qp_finalize_restore(qp);

next:
		rxe_put(qp);
		if (err)
			return err;
		rcu_read_lock();
	}
	rcu_read_unlock();

	return 0;
}

static void rxe_resume_vhca_contexts(struct rxe_dev *rxe)
{
	struct rxe_pool *pool = &rxe->uc_pool;
	struct rxe_pool_elem *elem;
	unsigned long index = 0;

	rcu_read_lock();
	for (elem = xa_find(&pool->xa, &index, ULONG_MAX, XA_PRESENT);
	     elem;
	     elem = xa_find_after(&pool->xa, &index, ULONG_MAX, XA_PRESENT)) {
		struct rxe_ucontext *uc = elem->obj;

		if (uc->restore_mode)
			WRITE_ONCE(uc->restore_finalized, true);
	}
	rcu_read_unlock();
}

static void rxe_resume_vhca_datapath(struct rxe_dev *rxe)
{
	struct rxe_pool *pool = &rxe->qp_pool;
	struct rxe_pool_elem *elem;
	unsigned long index = 0;

	rcu_read_lock();
	for (elem = xa_find(&pool->xa, &index, ULONG_MAX, XA_PRESENT);
	     elem;
	     elem = xa_find_after(&pool->xa, &index, ULONG_MAX, XA_PRESENT)) {
		struct rxe_qp *qp = elem->obj;

		if (!kref_get_unless_zero(&elem->ref_cnt))
			continue;
		rcu_read_unlock();

		if (qp->is_user && ib_qp_ucontext(&qp->ibqp) &&
		    to_ruc(ib_qp_ucontext(&qp->ibqp))->restore_mode) {
			rxe_qp_restore_timers(qp);
			rxe_qp_resume(qp);
		}

		rxe_put(qp);
		rcu_read_lock();
	}
	rcu_read_unlock();
}

static int UVERBS_HANDLER(RXE_IB_METHOD_RESUME_VHCA)(struct uverbs_attr_bundle *attrs)
{
	struct ib_ucontext *ucontext = ib_uverbs_get_ucontext(attrs);
	struct rxe_dev *rxe;
	int err;

	if (IS_ERR(ucontext))
		return PTR_ERR(ucontext);
	if (!to_ruc(ucontext)->restore_mode)
		return -EACCES;

	rxe = to_rdev(ucontext->device);
	mutex_lock(&rxe->vhca_lock);
	if (!rxe_vhca_all_objects_consumed(rxe->vhca_image,
					   rxe->vhca_image_length)) {
		mutex_unlock(&rxe->vhca_lock);
		return -ENODATA;
	}
	mutex_unlock(&rxe->vhca_lock);
	err = rxe_resume_vhca_cqs(rxe);
	if (err)
		return err;

	err = rxe_resume_vhca_qps(rxe);
	if (err)
		return err;

	/* Do not make any restored context runnable until all are ready. */
	rxe_resume_vhca_contexts(rxe);
	rxe_resume_vhca_datapath(rxe);

	mutex_lock(&rxe->vhca_lock);
	rxe_vhca_clear_contexts(rxe);
	kvfree(rxe->vhca_image);
	rxe->vhca_image = NULL;
	rxe->vhca_image_length = 0;
	mutex_unlock(&rxe->vhca_lock);
	return 0;
}

static int UVERBS_HANDLER(RXE_IB_METHOD_QUERY_QP)(
	struct uverbs_attr_bundle *attrs)
{
	struct rxe_restore_qp_req blob = {};
	struct ib_uobject *uobject;
	struct ib_qp *ibqp;
	u64 user_handle;
	struct rxe_qp *qp;
	int err;

	ibqp = uverbs_attr_get_obj(attrs, RXE_IB_ATTR_QUERY_QP_HANDLE);
	if (IS_ERR(ibqp))
		return PTR_ERR(ibqp);

	err = rxe_migrate_chk_qp_type(ibqp);
	if (err)
		return err;

	qp = to_rqp(ibqp);
	uobject = uverbs_attr_get_uobject(attrs, RXE_IB_ATTR_QUERY_QP_HANDLE);

	/* No user-side wire state to emit for kernel QPs. */
	if (!qp->is_user || !qp->sq.queue)
		return -ENXIO;

	rxe_snapshot_qp(qp, &blob);

	err = uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_QP_RESP_BLOB,
			     &blob, sizeof(blob));
	if (err)
		return err;

	/*
	 * The async-event cookie the source's ibv_create_qp recorded on
	 * the QP uobject. Not standard-queryable, so CRIU must preserve it
	 * through the kernel-sourced dump.
	 */
	user_handle = ib_qp_user_handle(ibqp);
	err = uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE,
			     &user_handle, sizeof(user_handle));
	if (err)
		return err;

	qp->migration_state = blob;
	qp->migration_uobject_handle = uobject->id;
	qp->migration_captured = true;
	return 0;
}

static int UVERBS_HANDLER(RXE_IB_METHOD_QUERY_CQ)(
	struct uverbs_attr_bundle *attrs)
{
	struct rxe_query_cq_resp blob = {};
	struct ib_uobject *uobject;
	struct rxe_cq *cq;
	struct ib_cq *ibcq;
	int err;

	ibcq = uverbs_attr_get_obj(attrs, RXE_IB_ATTR_QUERY_CQ_HANDLE);
	if (IS_ERR(ibcq))
		return PTR_ERR(ibcq);

	cq = to_rcq(ibcq);
	uobject = uverbs_attr_get_uobject(attrs, RXE_IB_ATTR_QUERY_CQ_HANDLE);

	/* Kernel-mode CQs have no user mmap ring to identify. */
	if (!cq->is_user || !cq->queue || !cq->queue->ip)
		return -ENXIO;

	blob.vm_pgoff = cq->queue->ip->info.offset;
	blob.cqe      = ibcq->cqe;
	blob.queue_size = cq->queue->buf_size;
	spin_lock_irq(&cq->cq_lock);
	blob.notify = cq->notify;
	spin_unlock_irq(&cq->cq_lock);
	err = uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_CQ_RESP_BLOB,
			     &blob, sizeof(blob));
	if (err)
		return err;

	cq->migration_uobject_handle = uobject->id;
	cq->migration_captured = true;
	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_FREEZE_DATAPATH,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_FREEZE_DATAPATH_QP_HANDLE,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(RXE_IB_ATTR_FREEZE_DATAPATH_FREEZE,
			   UVERBS_ATTR_TYPE(u8),
			   UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_FREEZE_CONTEXT,
	UVERBS_ATTR_PTR_IN(RXE_IB_ATTR_FREEZE_CONTEXT_FREEZE,
			   UVERBS_ATTR_TYPE(u8),
			   UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(RXE_IB_METHOD_RESUME_VHCA);

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_REGISTER_CONTEXT,
	UVERBS_ATTR_PTR_IN(RXE_IB_ATTR_REGISTER_CONTEXT_UFILE_ID,
			   UVERBS_ATTR_TYPE(u32), UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(RXE_IB_METHOD_CREATE_SAVE_FD,
			    UVERBS_ATTR_FD(RXE_IB_ATTR_CREATE_SAVE_FD_HANDLE,
				   RXE_IB_OBJECT_VHCA_STREAM,
				   UVERBS_ACCESS_NEW, UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(RXE_IB_METHOD_CREATE_LOAD_FD,
			    UVERBS_ATTR_FD(RXE_IB_ATTR_CREATE_LOAD_FD_HANDLE,
				   RXE_IB_OBJECT_VHCA_STREAM,
				   UVERBS_ACCESS_NEW, UA_MANDATORY),
			    UVERBS_ATTR_PTR_IN(
				    RXE_IB_ATTR_CREATE_LOAD_FD_LENGTH,
				    UVERBS_ATTR_TYPE(u64), UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(RXE_IB_METHOD_LOAD_VHCA,
			    UVERBS_ATTR_FD(RXE_IB_ATTR_LOAD_VHCA_HANDLE,
				   RXE_IB_OBJECT_VHCA_STREAM,
				   UVERBS_ACCESS_READ, UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_QUERY_QP,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_QUERY_QP_HANDLE,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_BLOB,
			    UVERBS_ATTR_TYPE(struct rxe_restore_qp_req),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE,
			    UVERBS_ATTR_TYPE(u64),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_SQ_IMAGE,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_RQ_IMAGE,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_RES,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL));

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_QUERY_CQ,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_QUERY_CQ_HANDLE,
			UVERBS_OBJECT_CQ,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_CQ_RESP_BLOB,
			    UVERBS_ATTR_TYPE(struct rxe_query_cq_resp),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_CQ_RESP_CQE_IMAGE,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL));

DECLARE_UVERBS_GLOBAL_METHODS(
	RXE_IB_OBJECT_MIGRATE,
	&UVERBS_METHOD(RXE_IB_METHOD_FREEZE_DATAPATH),
	&UVERBS_METHOD(RXE_IB_METHOD_QUERY_QP),
	&UVERBS_METHOD(RXE_IB_METHOD_QUERY_CQ),
	&UVERBS_METHOD(RXE_IB_METHOD_FREEZE_CONTEXT),
	&UVERBS_METHOD(RXE_IB_METHOD_RESUME_VHCA),
	&UVERBS_METHOD(RXE_IB_METHOD_REGISTER_CONTEXT));

DECLARE_UVERBS_NAMED_OBJECT(RXE_IB_OBJECT_VHCA_STREAM,
			   UVERBS_TYPE_ALLOC_FD(
				   sizeof(struct rxe_vhca_stream),
				   rxe_vhca_stream_destroy,
				   &rxe_vhca_stream_fops, "[rxe-vhca]",
			   O_RDWR),
			   &UVERBS_METHOD(RXE_IB_METHOD_CREATE_SAVE_FD),
			   &UVERBS_METHOD(RXE_IB_METHOD_CREATE_LOAD_FD),
			   &UVERBS_METHOD(RXE_IB_METHOD_LOAD_VHCA));

const struct uapi_definition rxe_migrate_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(RXE_IB_OBJECT_MIGRATE),
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(RXE_IB_OBJECT_VHCA_STREAM),
	{},
};
