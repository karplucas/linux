// SPDX-License-Identifier: GPL-2.0
/*
 * user_object_replay_probe -- empirical probe for user_mr_dma stage 2.
 *
 * Allocates a configurable set of user-mode RDMA resources whose backing
 * buffers flow through vfmig_dma_ops on a tracked VF -- PD, N MRs, CQ,
 * QP, SRQ (best-effort, may fail on restored VFs) -- prints a
 * key=value manifest of FW ids and expected awaiting_bind counts, then
 * blocks on stdin until it reads "quit\n". This shape lets the
 * test_user_object_replay.sh harness:
 *
 *     - Fork the probe in the background.
 *     - Drain stdout until "READY" -- at that point all resources are
 *       alive in FW and their backing buffers are tagged in the per-VF
 *       vfmig_iova_domain (after C6-C10 of the stage-2 series land;
 *       until then the per-buffer entries are auto-numbered and don't
 *       carry a (kind, fw_id) identity).
 *     - SAVE the source VHCA. The vfmig SAVE path (after C4 lands)
 *       snapshots the external IOVA registry into HOST_USER_PAGE wire
 *       records, one per retagged uobject.
 *     - On the destination, LOAD pre-installs awaiting_bind=true
 *       placeholders for each replayed record. The harness's Phase F
 *       reads them back via MLX5_VFMIG_IOC_QUERY_AWAITING_BIND.
 *     - Issue "quit" to release the probe.
 *
 * The manifest's expected_* counts represent the values the harness
 * SHOULD see in the QUERY_AWAITING_BIND output AFTER all of C4 + C5 +
 * C6..C10 have landed. Until that point the harness compares against
 * env-configurable EXPECT_* overrides that default to zero (the
 * baseline observation: ioctl callable, no replay records on the wire,
 * no placeholders on the destination).
 *
 * Multi-page MR coverage
 * ----------------------
 * --num-unaligned-mrs U adds U additional MRs whose umem straddles
 * two pages: a 4 KiB MR registered at offset 0x800 inside a 2-page
 * buffer. ib_umem_get pins both backing pages, and unless the buddy
 * allocator happened to hand them out physically contiguous,
 * sg_alloc_append_table_from_pages produces 2 sg entries -- one per
 * page -- which then become 2 external registry entries via
 * vfmig_dma_ops.map_sg. Each unaligned MR therefore contributes
 * either 1 (rare, contiguous case) or 2 (typical) entries, sharing
 * the same VFMIG_HUOBJ_KEY(KIND_MR, mkey_index).
 *
 * This is the regression case for the secondary-index multi-page
 * bug: before the (instance_key, iova) composite-key fix landed, the
 * second sibling's user_index_insert_locked() returned -EEXIST
 * against the first sibling, the rollback wiped both pages back to
 * KIND_NONE, and SAVE silently emitted ZERO HOST_USER_PAGE records
 * for the MR. The destination would then trip RESTORE_MR with
 * -ENOENT. With the fix in place each unaligned MR contributes >= 1
 * entry; the harness asserts the [min, max] band so a regression
 * back to obs_mr == num_aligned_mrs (i.e. unaligned MRs silently
 * dropped) trips a loud failure.
 *
 * Build:
 *   make -C tools/testing/mlx5_vfmig \
 *        save_load/user_object_replay/user_object_replay_probe
 *
 * Usage:
 *   ./user_object_replay_probe <ibdev> [--num-mrs N] [--num-unaligned-mrs U]
 *
 *   <ibdev>      name of an ib_device exposed by the tracked VF
 *                (e.g. mlx5_2). The harness picks this up via
 *                find_ib_dev_for_pci() on the bound VF's BDF.
 *
 *   --num-mrs N  number of page-aligned 4 KiB MRs to register.
 *                Default 4. Each MR's umem.sgt produces exactly 1
 *                external registry entry -- the stage-2 retag
 *                landed in C6 keys each with
 *                VFMIG_HUOBJ_KEY(KIND_MR, mkey_index).
 *
 *   --num-unaligned-mrs U  number of additional MRs whose umem
 *                straddles two pages (4 KiB at offset 0x800 inside
 *                a 2-page buffer). Default 0. Each unaligned MR
 *                contributes 1 or 2 external entries depending on
 *                physical-page contiguity; the manifest emits
 *                expected_mr / expected_mr_max bounding the band.
 *                Total array slots used = N + U, must be
 *                <= MAX_NUM_MRS.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

#define DEFAULT_NUM_MRS	4
#define MAX_NUM_MRS	64
#define MR_LEN		4096

/*
 * Per-page byte offset inside the 2-page umem buffer for unaligned
 * MRs. 0x800 puts the MR registration at exactly mid-page, so
 * [base+0x800, base+0x800+MR_LEN) covers the second half of page 0
 * + the first half of page 1 -- guaranteed multi-page coverage
 * (umem_offset + umem_length = 0x800 + 0x1000 = 0x1800 > PAGE_SIZE).
 * Any non-zero offset whose sum-with-MR_LEN exceeds PAGE_SIZE works;
 * 0x800 is just the cleanest mid-page split.
 */
#define UNALIGNED_MR_OFFSET	0x800
#define UNALIGNED_MR_BUF_SIZE	(2 * 4096)

static struct ibv_device *find_ibdev(const char *name)
{
	struct ibv_device **list;
	struct ibv_device *match = NULL;
	int n, i;

	list = ibv_get_device_list(&n);
	if (!list || n == 0) {
		fprintf(stderr, "uor: ibv_get_device_list returned 0 devices\n");
		return NULL;
	}
	for (i = 0; i < n; i++) {
		if (!strcmp(ibv_get_device_name(list[i]), name)) {
			match = list[i];
			break;
		}
	}
	if (!match) {
		fprintf(stderr, "uor: ibdev '%s' not found; available:", name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return match;
}

static int extract_pdn(struct ibv_pd *pd, uint32_t *out)
{
	struct mlx5dv_pd dvpd = {};
	struct mlx5dv_obj obj = { .pd = { .in = pd, .out = &dvpd } };
	int err = mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD);

	if (err) {
		fprintf(stderr, "uor: mlx5dv_init_obj(PD) failed: %d\n", err);
		return -1;
	}
	*out = dvpd.pdn;
	return 0;
}

static int extract_cqn(struct ibv_cq *cq, uint32_t *out)
{
	struct mlx5dv_cq dvcq = {};
	struct mlx5dv_obj obj = { .cq = { .in = cq, .out = &dvcq } };
	int err = mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ);

	if (err) {
		fprintf(stderr, "uor: mlx5dv_init_obj(CQ) failed: %d\n", err);
		return -1;
	}
	*out = dvcq.cqn;
	return 0;
}

static int extract_srqn(struct ibv_srq *srq, uint32_t *out)
{
	struct mlx5dv_srq dvsrq = {};
	struct mlx5dv_obj obj = { .srq = { .in = srq, .out = &dvsrq } };
	int err = mlx5dv_init_obj(&obj, MLX5DV_OBJ_SRQ);

	if (err) {
		fprintf(stderr, "uor: mlx5dv_init_obj(SRQ) failed: %d\n", err);
		return -1;
	}
	*out = dvsrq.srqn;
	return 0;
}

int main(int argc, char **argv)
{
	const char *ibdev_name;
	int num_mrs = DEFAULT_NUM_MRS;
	int num_unaligned_mrs = 0;
	int total_mrs;
	struct ibv_device *dev;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd = NULL;
	struct ibv_cq *cq = NULL;
	struct ibv_qp *qp = NULL;
	struct ibv_srq *srq = NULL;
	struct ibv_mr *mrs[MAX_NUM_MRS] = {};
	void *mr_bufs[MAX_NUM_MRS] = {};
	/*
	 * mr_unaligned[i] flags slot i as an unaligned-buffer MR so the
	 * teardown free()s the raw 2-page allocation rather than the
	 * mid-page MR address (which is not a malloc base).
	 */
	int mr_unaligned[MAX_NUM_MRS] = {};
	uint32_t pdn = 0, cqn = 0, qpn = 0, srqn = 0;
	int srq_ok = 0;
	int rc = 1;
	int i;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <ibdev> [--num-mrs N] [--num-unaligned-mrs U]\n",
			argv[0]);
		return 2;
	}
	ibdev_name = argv[1];
	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--num-mrs") && i + 1 < argc) {
			num_mrs = atoi(argv[++i]);
			if (num_mrs < 1 || num_mrs > MAX_NUM_MRS) {
				fprintf(stderr,
					"uor: num-mrs must be in [1, %d]\n",
					MAX_NUM_MRS);
				return 2;
			}
		} else if (!strcmp(argv[i], "--num-unaligned-mrs") &&
			   i + 1 < argc) {
			num_unaligned_mrs = atoi(argv[++i]);
			if (num_unaligned_mrs < 0 ||
			    num_unaligned_mrs > MAX_NUM_MRS) {
				fprintf(stderr,
					"uor: num-unaligned-mrs must be in [0, %d]\n",
					MAX_NUM_MRS);
				return 2;
			}
		} else {
			fprintf(stderr, "uor: unknown arg '%s'\n", argv[i]);
			return 2;
		}
	}

	total_mrs = num_mrs + num_unaligned_mrs;
	if (total_mrs > MAX_NUM_MRS) {
		fprintf(stderr,
			"uor: num-mrs (%d) + num-unaligned-mrs (%d) = %d exceeds MAX_NUM_MRS=%d\n",
			num_mrs, num_unaligned_mrs, total_mrs, MAX_NUM_MRS);
		return 2;
	}

	dev = find_ibdev(ibdev_name);
	if (!dev)
		return 1;

	ctx = ibv_open_device(dev);
	if (!ctx) {
		fprintf(stderr, "uor: ibv_open_device(%s) failed: %s\n",
			ibdev_name, strerror(errno));
		return 1;
	}

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		fprintf(stderr, "uor: ibv_alloc_pd failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (extract_pdn(pd, &pdn))
		goto out;

	/*
	 * Aligned MRs first. Each backed by a 4 KiB page-aligned user
	 * buffer; ib_umem_get pins one page, sg_alloc_append_table_from
	 * _pages produces one sg entry, vfmig_dma_ops.map_sg installs
	 * one external registry entry. C6 retags each entry with
	 * VFMIG_HUOBJ_KEY(KIND_MR, mkey_index = lkey >> 8).
	 */
	for (i = 0; i < num_mrs; i++) {
		mr_bufs[i] = aligned_alloc(4096, MR_LEN);
		if (!mr_bufs[i]) {
			fprintf(stderr, "uor: aligned_alloc(MR %d) failed\n", i);
			goto out;
		}
		memset(mr_bufs[i], 0, MR_LEN);
		mrs[i] = ibv_reg_mr(pd, mr_bufs[i], MR_LEN,
				    IBV_ACCESS_LOCAL_WRITE |
				    IBV_ACCESS_REMOTE_WRITE |
				    IBV_ACCESS_REMOTE_READ);
		if (!mrs[i]) {
			fprintf(stderr,
				"uor: ibv_reg_mr #%d failed: %s\n",
				i, strerror(errno));
			goto out;
		}
	}

	/*
	 * Unaligned MRs. Each backed by a 2-page buffer; the MR is
	 * registered at offset UNALIGNED_MR_OFFSET inside the buffer
	 * so the umem covers the second half of page 0 + the first
	 * half of page 1. ib_umem_get pins both pages; the umem.sgt
	 * yields 2 sg entries (typical: anonymous user pages aren't
	 * physically contiguous) -> 2 external registry entries that
	 * vfmig_iova_retag_external_range stamps with the SAME
	 * (KIND_MR, mkey_index). Pre-fix this is exactly where
	 * user_index_insert_locked tripped -EEXIST on the second
	 * sibling and silently dropped the entire MR; post-fix the
	 * composite-key index keeps both siblings under one key.
	 */
	for (i = 0; i < num_unaligned_mrs; i++) {
		int slot = num_mrs + i;
		void *raw;
		void *mr_addr;

		raw = aligned_alloc(4096, UNALIGNED_MR_BUF_SIZE);
		if (!raw) {
			fprintf(stderr,
				"uor: aligned_alloc(unaligned MR %d) failed\n",
				i);
			goto out;
		}
		memset(raw, 0, UNALIGNED_MR_BUF_SIZE);
		mr_bufs[slot] = raw;
		mr_unaligned[slot] = 1;
		mr_addr = (char *)raw + UNALIGNED_MR_OFFSET;
		mrs[slot] = ibv_reg_mr(pd, mr_addr, MR_LEN,
				       IBV_ACCESS_LOCAL_WRITE |
				       IBV_ACCESS_REMOTE_WRITE |
				       IBV_ACCESS_REMOTE_READ);
		if (!mrs[slot]) {
			fprintf(stderr,
				"uor: ibv_reg_mr unaligned #%d failed: %s\n",
				i, strerror(errno));
			goto out;
		}
	}

	/*
	 * One CQ. Its CQE buffer + doorbell record both flow through
	 * vfmig_dma_ops:
	 *   - the CQE buffer (16 KiB by default for cqe=16, sizeof(cqe)=64)
	 *     is umem-pinned by mlx5_ib_create_cq via create_cq_user ->
	 *     ib_umem_get and mapped via .map_sg.
	 *   - the doorbell record page is a separate shared-pgdir page
	 *     pinned by mlx5_ib_db_map_user. C7 retags the DBR page with
	 *     VFMIG_HUOBJ_KEY(KIND_DBR, virt & PAGE_MASK); C8 retags the
	 *     CQE buffer's umem range with VFMIG_HUOBJ_KEY(KIND_CQ, cqn).
	 */
	cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!cq) {
		fprintf(stderr, "uor: ibv_create_cq failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (extract_cqn(cq, &cqn))
		goto out;

	/*
	 * One RC QP. Send/recv queues, doorbell record. C9 retags the
	 * QP's umem range with VFMIG_HUOBJ_KEY(KIND_QP, qpn). DBR page
	 * may be shared with the CQ (mlx5_ib_db_map_user dedups by
	 * pgdir page; multiple uobjects can share one DBR page) -- in
	 * which case the DBR retag (C7) only fires once on the
	 * first-touching uobject and the QP's create-side DBR-retag
	 * call short-circuits.
	 */
	{
		struct ibv_qp_init_attr qa = {
			.send_cq = cq,
			.recv_cq = cq,
			.qp_type = IBV_QPT_RC,
			.cap = {
				.max_send_wr = 4,
				.max_recv_wr = 4,
				.max_send_sge = 1,
				.max_recv_sge = 1,
			},
		};
		qp = ibv_create_qp(pd, &qa);
		if (!qp) {
			fprintf(stderr, "uor: ibv_create_qp failed: %s\n",
				strerror(errno));
			goto out;
		}
		qpn = qp->qp_num;
	}

	/*
	 * One SRQ -- best-effort: the restored-VF SRQ EINVAL gate
	 * (track_known_srq_gate) means destination-side SRQ creation
	 * is intentionally fragile in v0. Source-side creation here
	 * usually works on a fresh VF; the harness reads srq_ok=0/1
	 * to decide whether to factor SRQ counts into the verdict.
	 */
	{
		struct ibv_srq_init_attr sa = {
			.attr = { .max_wr = 4, .max_sge = 1 },
		};
		srq = ibv_create_srq(pd, &sa);
		if (!srq) {
			fprintf(stderr,
				"uor: ibv_create_srq failed (best-effort): %s\n",
				strerror(errno));
		} else if (extract_srqn(srq, &srqn) == 0) {
			srq_ok = 1;
		} else {
			ibv_destroy_srq(srq);
			srq = NULL;
		}
	}

	/* Manifest: shell-eval-able key=value lines. */
	printf("ibdev=%s\n", ibdev_name);
	printf("pdn=%u\n", pdn);
	printf("num_mrs=%d\n", num_mrs);
	printf("num_unaligned_mrs=%d\n", num_unaligned_mrs);
	for (i = 0; i < total_mrs; i++) {
		void *mr_addr;

		mr_addr = mr_unaligned[i]
			? (char *)mr_bufs[i] + UNALIGNED_MR_OFFSET
			: mr_bufs[i];
		printf("mr_%d_addr=0x%016llx\n", i,
		       (unsigned long long)(uintptr_t)mr_addr);
		printf("mr_%d_length=0x%016llx\n", i,
		       (unsigned long long)MR_LEN);
		printf("mr_%d_lkey=0x%08x\n", i, mrs[i]->lkey);
		printf("mr_%d_rkey=0x%08x\n", i, mrs[i]->rkey);
		printf("mr_%d_mkey_index=%u\n", i, mrs[i]->lkey >> 8);
		printf("mr_%d_unaligned=%d\n", i, mr_unaligned[i]);
	}
	printf("cqn=%u\n", cqn);
	printf("qpn=%u\n", qpn);
	if (srq_ok)
		printf("srqn=%u\n", srqn);
	else
		printf("srqn=SKIPPED\n");

	/*
	 * expected_* counts: what the destination's
	 * MLX5_VFMIG_IOC_QUERY_AWAITING_BIND should report AFTER all of
	 * C4 + C5 + C6..C10 of stage 2 have landed.
	 *
	 *   - MR: aligned MRs contribute exactly 1 external entry each
	 *         (single-page umem). Unaligned MRs contribute 1 OR 2
	 *         depending on whether ib_umem_get's pinned 2-page run
	 *         happens to be physically contiguous (rare; typical is
	 *         non-contiguous => 2 sg entries => 2 registry entries
	 *         sharing a single (KIND_MR, mkey_index)). The probe
	 *         emits both bounds; the harness range-checks
	 *         obs_mr in [expected_mr, expected_mr_max] when U > 0
	 *         and falls back to the legacy exact-match comparison
	 *         when U == 0 (expected_mr == expected_mr_max).
	 *   - CQ: one external entry per ibv_create_cq's CQE-buffer
	 *         umem (DBR is separate, accounted under KIND_DBR).
	 *   - QP: one external entry per ibv_create_qp's send+recv
	 *         queue umem.
	 *   - SRQ: one external entry when srq_ok, else 0.
	 *   - DBR: total shared doorbell pages touched by CQ/QP/SRQ.
	 *          mlx5_ib_db_map_user dedups by pgdir page; with one
	 *          CQ + one QP + (optional SRQ) the typical answer is
	 *          1 (all sharing a single pgdir page) but FW config
	 *          and umem-pin ordering can yield 2 or 3. Until C7
	 *          lands and the harness can compare against the
	 *          actual ioctl reading, expected_dbr is left as a
	 *          floor (>= 1 when any DBR-allocating uobject was
	 *          created) and the harness defaults EXPECT_DBR_COUNT
	 *          to that floor.
	 */
	printf("expected_mr=%d\n", num_mrs + num_unaligned_mrs);
	printf("expected_mr_max=%d\n", num_mrs + 2 * num_unaligned_mrs);
	printf("expected_cq=1\n");
	printf("expected_qp=1\n");
	printf("expected_srq=%d\n", srq_ok);
	printf("expected_dbr_min=1\n");
	printf("READY\n");
	fflush(stdout);

	{
		char line[64];

		while (fgets(line, sizeof(line), stdin)) {
			if (!strncmp(line, "quit", 4))
				break;
		}
	}

	rc = 0;

out:
	if (qp)
		ibv_destroy_qp(qp);
	if (srq)
		ibv_destroy_srq(srq);
	for (i = 0; i < MAX_NUM_MRS; i++) {
		if (mrs[i])
			ibv_dereg_mr(mrs[i]);
		free(mr_bufs[i]);
	}
	if (cq)
		ibv_destroy_cq(cq);
	if (pd)
		ibv_dealloc_pd(pd);
	if (ctx)
		ibv_close_device(ctx);
	return rc;
}
