// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2026 rxe CRIU migration. All rights reserved.
 *
 * Driver-private RXE_IB_OBJECT_MIGRATE uverbs object: the rxe arm of the
 * CRIU dump-side choreography. The generic restore path lives in the
 * UVERBS_OBJECT_RESTORE family; this object carries the dump-side queries
 * whose payloads are rxe-private.
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

#define UVERBS_MODULE_NAME rdma_rxe
#include <rdma/uverbs_named_ioctl.h>

/*
 * Only connected/datagram transports carry the wire state QUERY_QP
 * emits. GSI/SMI and other special QP types are out of scope for CRIU
 * migration.
 */
static int rxe_migrate_chk_qp_type(const struct ib_qp *ibqp)
{
	switch (ibqp->qp_type) {
	case IB_QPT_RC:
	case IB_QPT_UC:
	case IB_QPT_UD:
		return 0;
	default:
		return -EOPNOTSUPP;
	}
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

static int rxe_finalize_context_cqs(struct rxe_dev *rxe,
				    struct ib_ucontext *ucontext)
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

		if (cq->is_user && ib_cq_ucontext(&cq->ibcq) == ucontext) {
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

static int rxe_finalize_context_qps(struct rxe_dev *rxe,
				    struct ib_ucontext *ucontext)
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

		if (!qp->is_user || ib_qp_ucontext(&qp->ibqp) != ucontext)
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

static int UVERBS_HANDLER(RXE_IB_METHOD_FINALIZE_CONTEXT)(struct uverbs_attr_bundle *attrs)
{
	struct ib_ucontext *ucontext = ib_uverbs_get_ucontext(attrs);
	struct rxe_dev *rxe;
	int err;

	if (IS_ERR(ucontext))
		return PTR_ERR(ucontext);
	if (!to_ruc(ucontext)->restore_mode)
		return -EACCES;

	rxe = to_rdev(ucontext->device);
	err = rxe_finalize_context_cqs(rxe, ucontext);
	if (err)
		return err;

	return rxe_finalize_context_qps(rxe, ucontext);
}

/*
 * Emit one optional fixed-length image attr verbatim (no ring cursors).
 * Used for the responder-resources array, which is a plain
 * max_dest_rd_atomic-entry table, not a producer/consumer ring.
 */
static int rxe_query_emit_image(struct uverbs_attr_bundle *attrs, u16 attr_id,
				const void *data, u32 len)
{
	int user_len;

	if (!uverbs_attr_is_valid(attrs, attr_id) || len == 0)
		return 0;

	user_len = uverbs_attr_get_len(attrs, attr_id);
	if (user_len < 0)
		return 0;
	if ((u32)user_len < len)
		return -ENOSPC;

	return uverbs_copy_to(attrs, attr_id, data, len);
}

static int UVERBS_HANDLER(RXE_IB_METHOD_QUERY_QP)(
	struct uverbs_attr_bundle *attrs)
{
	struct rxe_restore_qp_req blob = {};
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

	/* No user-side wire state to emit for kernel QPs. */
	if (!qp->is_user || !qp->sq.queue)
		return -ENXIO;

	/* Identity + ring mmap offsets. */
	blob.qpn = ibqp->qp_num;
	if (qp->sq.queue->ip)
		blob.sq_vm_pgoff = qp->sq.queue->ip->info.offset;
	if (qp->rq.queue && qp->rq.queue->ip)
		blob.rq_vm_pgoff = qp->rq.queue->ip->info.offset;

	/* ib_qp_attr-class wire state. */
	memcpy(&blob.av, &qp->pri_av, sizeof(blob.av));
	blob.dest_qp_num	= qp->attr.dest_qp_num;
	blob.qkey		= qp->attr.qkey;
	blob.sq_psn		= qp->attr.sq_psn;
	blob.rq_psn		= qp->attr.rq_psn;
	blob.qp_access_flags	= qp->attr.qp_access_flags;
	blob.max_rd_atomic	= qp->attr.max_rd_atomic;
	blob.max_dest_rd_atomic = qp->attr.max_dest_rd_atomic;
	blob.pkey_index		= qp->attr.pkey_index;
	blob.path_mtu		= qp->attr.path_mtu;
	blob.retry_cnt		= qp->attr.retry_cnt;
	blob.rnr_retry		= qp->attr.rnr_retry;
	blob.min_rnr_timer	= qp->attr.min_rnr_timer;
	blob.timeout		= qp->attr.timeout;
	blob.port_num		= qp->attr.port_num;
	blob.sq_sig_all		= (qp->sq_sig_type == IB_SIGNAL_ALL_WR) ? 1 : 0;

	/* Live protocol state ib_modify_qp cannot express. */
	blob.req_psn		= qp->req.psn;
	blob.comp_psn		= qp->comp.psn;
	blob.resp_psn		= qp->resp.psn;
	blob.resp_msn		= qp->resp.msn;
	blob.req_wqe_index	= qp->req.wqe_index;
	blob.ssn		= atomic_read(&qp->ssn);

	blob.resp_ack_psn	= qp->resp.ack_psn;
	blob.resp_opcode	= qp->resp.opcode;
	blob.resp_status	= qp->resp.status;
	blob.resp_aeth_syndrome	= qp->resp.aeth_syndrome;
	blob.res_head		= qp->resp.res_head;
	blob.res_tail		= qp->resp.res_tail;
	if (qp->resp.resources && qp->attr.max_dest_rd_atomic)
		blob.res_image_bytes = qp->attr.max_dest_rd_atomic *
				       sizeof(struct resp_res);

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

	return rxe_query_emit_image(attrs, RXE_IB_ATTR_QUERY_QP_RESP_RES,
				    qp->resp.resources, blob.res_image_bytes);
}

static int UVERBS_HANDLER(RXE_IB_METHOD_QUERY_CQ)(
	struct uverbs_attr_bundle *attrs)
{
	struct rxe_query_cq_resp blob = {};
	struct rxe_cq *cq;
	struct ib_cq *ibcq;

	ibcq = uverbs_attr_get_obj(attrs, RXE_IB_ATTR_QUERY_CQ_HANDLE);
	if (IS_ERR(ibcq))
		return PTR_ERR(ibcq);

	cq = to_rcq(ibcq);

	/* Kernel-mode CQs have no user mmap ring to identify. */
	if (!cq->is_user || !cq->queue || !cq->queue->ip)
		return -ENXIO;

	blob.vm_pgoff = cq->queue->ip->info.offset;
	blob.cqe      = ibcq->cqe;
	spin_lock_irq(&cq->cq_lock);
	blob.notify = cq->notify;
	spin_unlock_irq(&cq->cq_lock);

	return uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_CQ_RESP_BLOB,
			      &blob, sizeof(blob));
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

DECLARE_UVERBS_NAMED_METHOD(RXE_IB_METHOD_FINALIZE_CONTEXT);

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
	&UVERBS_METHOD(RXE_IB_METHOD_FINALIZE_CONTEXT));

const struct uapi_definition rxe_migrate_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(RXE_IB_OBJECT_MIGRATE),
	{},
};
