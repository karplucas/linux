// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB

#include <kunit/test.h>
#include <linux/unaligned.h>

#include "rxe.h"
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

static void rxe_vhca_resp_resource_roundtrip_test(struct kunit *test)
{
	static const int types[] = {
		RXE_READ_MASK,
		RXE_ATOMIC_MASK,
		RXE_ATOMIC_WRITE_MASK,
		RXE_FLUSH_MASK,
	};
	struct rxe_vhca_resp_resource record;
	struct resp_res restored;
	struct resp_res source;
	int err;
	int i;

	for (i = 0; i < ARRAY_SIZE(types); i++) {
		memset(&source, 0, sizeof(source));
		source.type = types[i];
		source.replay = 1;
		source.first_psn = 11;
		source.last_psn = 13;
		source.cur_psn = 12;
		source.state = rdatm_res_state_replay;

		if (source.type == RXE_READ_MASK) {
			source.read.va_org = 0x1000;
			source.read.va = 0x1100;
			source.read.rkey = 0x1234;
			source.read.length = 1024;
			source.read.resid = 768;
		} else if (source.type == RXE_ATOMIC_MASK) {
			source.atomic.orig_val = 0x1122334455667788;
		} else if (source.type == RXE_FLUSH_MASK) {
			source.flush.va = 0x2000;
			source.flush.length = 4096;
			source.flush.type = IB_FLUSH_GLOBAL;
			source.flush.level = IB_FLUSH_RANGE;
		}

		err = rxe_vhca_encode_resp_resource(&record, 3, &source);
		KUNIT_ASSERT_EQ(test, err, 0);
		memset(&restored, 0, sizeof(restored));
		err = rxe_vhca_decode_resp_resource(&record, 4, &restored);
		KUNIT_ASSERT_EQ(test, err, 0);
		KUNIT_EXPECT_MEMEQ(test, &restored, &source, sizeof(source));
	}
}

static void rxe_vhca_resp_resource_validation_test(struct kunit *test)
{
	struct rxe_vhca_resp_resource record = {};
	struct resp_res resource = {};

	record.slot = cpu_to_le32(2);
	record.type = cpu_to_le32(RXE_VHCA_RESP_RESOURCE_READ);
	KUNIT_EXPECT_EQ(test,
			rxe_vhca_decode_resp_resource(&record, 2, &resource),
			-EINVAL);

	record.slot = 0;
	record.type = cpu_to_le32(U32_MAX);
	KUNIT_EXPECT_EQ(test,
			rxe_vhca_decode_resp_resource(&record, 2, &resource),
			-EBADMSG);

	record.type = cpu_to_le32(RXE_VHCA_RESP_RESOURCE_READ);
	record.data.read.length = cpu_to_le32(8);
	record.data.read.resid = cpu_to_le32(9);
	KUNIT_EXPECT_EQ(test,
			rxe_vhca_decode_resp_resource(&record, 2, &resource),
			-EBADMSG);
}

static struct kunit_case rxe_vhca_test_cases[] = {
	KUNIT_CASE(rxe_vhca_record_roundtrip_test),
	KUNIT_CASE(rxe_vhca_bad_magic_test),
	KUNIT_CASE(rxe_vhca_truncated_record_test),
	KUNIT_CASE(rxe_vhca_record_length_test),
	KUNIT_CASE(rxe_vhca_resp_resource_roundtrip_test),
	KUNIT_CASE(rxe_vhca_resp_resource_validation_test),
	{}
};

static struct kunit_suite rxe_vhca_test_suite = {
	.name = "rxe_vhca",
	.test_cases = rxe_vhca_test_cases,
};

kunit_test_suite(rxe_vhca_test_suite);
