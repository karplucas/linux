/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
#ifndef RXE_VHCA_H
#define RXE_VHCA_H

#include <linux/bits.h>
#include <linux/types.h>

#define RXE_VHCA_IMAGE_MAGIC	0x56455852

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

struct rxe_vhca_qp_header {
	__le32 uobject_handle;
	__le32 resp_resource_count;
};

enum rxe_vhca_resp_resource_type {
	RXE_VHCA_RESP_RESOURCE_READ = 1,
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
int rxe_vhca_encode_resp_resource(struct rxe_vhca_resp_resource *record,
				  u32 slot, const struct resp_res *resource);
int rxe_vhca_decode_resp_resource(const struct rxe_vhca_resp_resource *record,
				  u32 slots, struct resp_res *resource);

#endif /* RXE_VHCA_H */
