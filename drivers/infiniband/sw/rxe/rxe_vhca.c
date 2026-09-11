// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "rxe_vhca.h"

static_assert(sizeof(struct rxe_vhca_image_header) == 4);
static_assert(sizeof(struct rxe_vhca_record_header) == 16);

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
