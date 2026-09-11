/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
#ifndef RXE_VHCA_H
#define RXE_VHCA_H

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

int rxe_vhca_writer_init(struct rxe_vhca_writer *writer, void *data,
			 size_t capacity);
int rxe_vhca_write_record(struct rxe_vhca_writer *writer, u32 type,
			  u32 flags, const void *payload, size_t length);
int rxe_vhca_reader_init(struct rxe_vhca_reader *reader, const void *data,
			 size_t length);
int rxe_vhca_read_record(struct rxe_vhca_reader *reader,
			 struct rxe_vhca_record *record);

#endif /* RXE_VHCA_H */
