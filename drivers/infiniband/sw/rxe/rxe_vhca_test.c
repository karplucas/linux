// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB

#include <kunit/test.h>
#include <linux/unaligned.h>

#include "rxe_vhca.h"

static void rxe_vhca_record_roundtrip_test(struct kunit *test)
{
	struct rxe_vhca_context_header context = {
		.ufile_id = cpu_to_le32(7),
		.cq_count = cpu_to_le32(1),
		.qp_count = cpu_to_le32(2),
	};
	struct rxe_vhca_record record;
	struct rxe_vhca_reader reader;
	struct rxe_vhca_writer writer;
	u8 image[128];
	int err;

	err = rxe_vhca_writer_init(&writer, image, sizeof(image));
	KUNIT_ASSERT_EQ(test, err, 0);
	err = rxe_vhca_write_record(&writer, RXE_VHCA_RECORD_CONTEXT, 0,
				    &context, sizeof(context));
	KUNIT_ASSERT_EQ(test, err, 0);

	err = rxe_vhca_reader_init(&reader, image, writer.length);
	KUNIT_ASSERT_EQ(test, err, 0);
	err = rxe_vhca_read_record(&reader, &record);
	KUNIT_ASSERT_EQ(test, err, 1);
	KUNIT_EXPECT_EQ(test, record.type, (u32)RXE_VHCA_RECORD_CONTEXT);
	KUNIT_EXPECT_EQ(test, record.flags, 0U);
	KUNIT_EXPECT_EQ(test, record.length, sizeof(context));
	KUNIT_EXPECT_MEMEQ(test, record.payload, &context, sizeof(context));
	KUNIT_EXPECT_EQ(test, rxe_vhca_read_record(&reader, &record), 0);
}

static void rxe_vhca_bad_magic_test(struct kunit *test)
{
	struct rxe_vhca_reader reader;
	__le32 image = 0;

	KUNIT_EXPECT_EQ(test,
			rxe_vhca_reader_init(&reader, &image, sizeof(image)),
			-EBADMSG);
}

static void rxe_vhca_truncated_record_test(struct kunit *test)
{
	struct rxe_vhca_record record;
	struct rxe_vhca_reader reader;
	u8 image[sizeof(struct rxe_vhca_image_header) + 1];

	put_unaligned_le32(RXE_VHCA_IMAGE_MAGIC, image);
	KUNIT_ASSERT_EQ(test,
			rxe_vhca_reader_init(&reader, image, sizeof(image)), 0);
	KUNIT_EXPECT_EQ(test, rxe_vhca_read_record(&reader, &record),
			-EBADMSG);
}

static void rxe_vhca_record_length_test(struct kunit *test)
{
	struct rxe_vhca_record_header *header;
	struct rxe_vhca_record record;
	struct rxe_vhca_reader reader;
	u8 image[sizeof(struct rxe_vhca_image_header) + sizeof(*header)];

	put_unaligned_le32(RXE_VHCA_IMAGE_MAGIC, image);
	header = (void *)(image + sizeof(struct rxe_vhca_image_header));
	put_unaligned_le32(RXE_VHCA_RECORD_QP, &header->type);
	put_unaligned_le32(0, &header->flags);
	put_unaligned_le64(U64_MAX, &header->length);

	KUNIT_ASSERT_EQ(test,
			rxe_vhca_reader_init(&reader, image, sizeof(image)), 0);
	KUNIT_EXPECT_EQ(test, rxe_vhca_read_record(&reader, &record),
			-EBADMSG);
}

static struct kunit_case rxe_vhca_test_cases[] = {
	KUNIT_CASE(rxe_vhca_record_roundtrip_test),
	KUNIT_CASE(rxe_vhca_bad_magic_test),
	KUNIT_CASE(rxe_vhca_truncated_record_test),
	KUNIT_CASE(rxe_vhca_record_length_test),
	{}
};

static struct kunit_suite rxe_vhca_test_suite = {
	.name = "rxe_vhca",
	.test_cases = rxe_vhca_test_cases,
};

kunit_test_suite(rxe_vhca_test_suite);
