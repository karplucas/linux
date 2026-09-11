/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
#ifndef RXE_VHCA_H
#define RXE_VHCA_H

#include <linux/bits.h>
#include <linux/types.h>
#include <rdma/rdma_user_rxe.h>

#define RXE_VHCA_IMAGE_MAGIC	0x56455852
#define RXE_VHCA_RECORD_F_CONSUMED BIT(0)

enum rxe_vhca_record_type {
	RXE_VHCA_RECORD_CONTEXT = 1,
	RXE_VHCA_RECORD_CQ,
	RXE_VHCA_RECORD_QP,
	RXE_VHCA_RECORD_RESP_RESOURCE,
};

struct rxe_vhca_image_header {
	__le32 magic;
};

struct rxe_vhca_record_header {
	__le32 type;
	__le32 flags;
	__le64 length;
};

struct rxe_vhca_context_header {
	__le32 ufile_id;
	__le32 cq_count;
	__le32 qp_count;
	__le32 reserved;
};

struct rxe_vhca_cq {
	__le32 uobject_handle;
	__le32 notify;
};

struct rxe_vhca_qp_header {
	__le32 uobject_handle;
	__le32 resp_resource_count;
};

struct rxe_vhca_ip_address {
	__le16 family;
	__le16 port;
	__le32 flowinfo;
	u8 address[16];
	__le32 scope_id;
};

struct rxe_vhca_av {
	u8 port_num;
	u8 network_type;
	u8 dmac[6];
	u8 dgid[16];
	__le32 flow_label;
	u8 sgid_index;
	u8 hop_limit;
	u8 traffic_class;
	u8 reserved;
	struct rxe_vhca_ip_address sgid_addr;
	struct rxe_vhca_ip_address dgid_addr;
};

struct rxe_vhca_qp_state {
	struct rxe_vhca_av av;
	__le64 sq_vm_pgoff;
	__le64 rq_vm_pgoff;
	__le32 qpn;
	__le32 dest_qp_num;
	__le32 qkey;
	__le32 sq_psn;
	__le32 rq_psn;
	__le32 qp_access_flags;
	__le32 max_rd_atomic;
	__le32 max_dest_rd_atomic;
	__le32 req_psn;
	__le32 comp_psn;
	__le32 resp_psn;
	__le32 resp_msn;
	__le32 req_wqe_index;
	__le32 ssn;
	__le32 resp_ack_psn;
	__le32 resp_opcode;
	__le32 resp_status;
	__le32 res_head;
	__le32 res_tail;
	__le16 pkey_index;
	u8 path_mtu;
	u8 retry_cnt;
	u8 rnr_retry;
	u8 retry_cnt_left;
	u8 rnr_retry_left;
	u8 min_rnr_timer;
	u8 timeout;
	u8 port_num;
	u8 sq_sig_all;
	u8 resp_aeth_syndrome;
	u8 retrans_pending;
	u8 rnr_pending;
	u8 reserved[2];
	__le64 retrans_remaining_ns;
	__le64 rnr_remaining_ns;
};

struct rxe_vhca_qp {
	struct rxe_vhca_qp_header header;
	struct rxe_vhca_qp_state state;
};

enum rxe_vhca_resp_resource_type {
	RXE_VHCA_RESP_RESOURCE_EMPTY,
	RXE_VHCA_RESP_RESOURCE_READ,
	RXE_VHCA_RESP_RESOURCE_ATOMIC,
	RXE_VHCA_RESP_RESOURCE_ATOMIC_WRITE,
	RXE_VHCA_RESP_RESOURCE_FLUSH,
};

enum rxe_vhca_resp_resource_state {
	RXE_VHCA_RESP_RESOURCE_NEXT,
	RXE_VHCA_RESP_RESOURCE_NEW,
	RXE_VHCA_RESP_RESOURCE_REPLAY,
};

#define RXE_VHCA_RESP_RESOURCE_F_REPLAY	BIT(0)

struct rxe_vhca_resp_resource {
	__le32 slot;
	__le32 type;
	__le32 flags;
	__le32 state;
	__le32 first_psn;
	__le32 last_psn;
	__le32 cur_psn;
	__le32 reserved;
	union {
		struct {
			__le64 va_org;
			__le64 va;
			__le32 rkey;
			__le32 length;
			__le32 resid;
			__le32 reserved;
		} read;
		struct {
			__le64 orig_val;
			__le64 reserved[3];
		} atomic;
		struct {
			__le64 va;
			__le32 length;
			u8 type;
			u8 level;
			__le16 reserved;
			__le64 reserved2[2];
		} flush;
		u8 raw[32];
	} data;
};

struct rxe_vhca_writer {
	u8 *data;
	size_t capacity;
	size_t length;
};

struct rxe_vhca_reader {
	const u8 *data;
	size_t length;
	size_t offset;
};

struct rxe_vhca_record {
	const u8 *payload;
	size_t length;
	u32 type;
	u32 flags;
};

struct resp_res;

int rxe_vhca_writer_init(struct rxe_vhca_writer *writer, void *data,
			 size_t capacity);
int rxe_vhca_write_record(struct rxe_vhca_writer *writer, u32 type,
			  u32 flags, const void *payload, size_t length);
int rxe_vhca_reader_init(struct rxe_vhca_reader *reader, const void *data,
			 size_t length);
int rxe_vhca_read_record(struct rxe_vhca_reader *reader,
			 struct rxe_vhca_record *record);
int rxe_vhca_validate_contexts(const void *data, size_t length);
bool rxe_vhca_has_context(const void *data, size_t length, u32 ufile_id);
int rxe_vhca_find_cq(void *data, size_t length, u32 ufile_id,
		     u32 uobject_handle, struct rxe_vhca_cq *cq);
int rxe_vhca_find_qp(void *data, size_t length, u32 ufile_id,
		     u32 uobject_handle, struct rxe_restore_qp_req *state,
		     struct resp_res **resources);
bool rxe_vhca_all_objects_consumed(const void *data, size_t length);
int rxe_vhca_encode_resp_resource(struct rxe_vhca_resp_resource *record,
				  u32 slot, const struct resp_res *resource);
int rxe_vhca_decode_resp_resource(const struct rxe_vhca_resp_resource *record,
				  u32 slots, struct resp_res *resource);
int rxe_vhca_encode_qp_state(struct rxe_vhca_qp_state *image,
			     const struct rxe_restore_qp_req *state);
int rxe_vhca_decode_qp_state(const struct rxe_vhca_qp_state *image,
			     struct rxe_restore_qp_req *state);

#endif /* RXE_VHCA_H */
