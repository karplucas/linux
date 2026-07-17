// SPDX-License-Identifier: GPL-2.0
/*
 * restored_reg_mr_probe -- exercise the *ordinary* ibv_reg_mr path
 * (libibverbs -> libmlx5 -> mlx5_ib create_real_mr) against a bound,
 * post-LOAD_VHCA_STATE ("restored") mlx5 VF ib_device.
 *
 * Unlike mr_restore_probe_mlx5_vfmig (which drives the CRIU adoption
 * verb UVERBS_METHOD_RESTORE_MR and bypasses create_real_mr entirely),
 * this probe issues a *fresh* registration -- the exact path a restored
 * userspace process hits when it calls ibv_reg_mr after CRIU restore.
 *
 * On a restored VF the kernel UMR QP/resources are never reconstituted
 * (mlx5r_umr_resource_init is skipped), so before the inline-CREATE_MKEY
 * gate in create_real_mr this call would post to a dead UMR QP and block
 * forever in the untimed mlx5r_umr_post_send_wait() -> wait_for_completion()
 * -- an unkillable D-state hang that also wedges subsequent SR-IOV
 * teardown. WITH the gate, create_real_mr forces the inline CREATE_MKEY
 * (command-ring) path, which is functional on restored VFs, and this
 * probe returns cleanly.
 *
 * DANGER: only run this probe against a kernel that carries the
 * restored-VF inline-CREATE_MKEY gate (drivers/infiniband/hw/mlx5/mr.c).
 * On an ungated kernel this probe will D-state hang and require a reboot.
 * See scratch/restored_vf_new_verbs.md.
 *
 * Build:
 *   make -C tools/testing/criu_rdma \
 *        uobject_restore/reg_mr_live/restored_reg_mr_probe
 *
 * Usage:
 *   ./restored_reg_mr_probe <ibdev> <mr_bytes> [<count>]
 *
 * <ibdev>     destination ib_device name (e.g. mlx5_0), typically from
 *             `find_ib_dev_for_pci <vf_bdf>` in the harness.
 * <mr_bytes>  size of each MR to register (e.g. 4096, or 536870912 for
 *             512 MiB to probe the >= MLX5_MAX_UMR_PAGES*PAGE_SIZE gate).
 * <count>     optional number of back-to-back reg/dereg cycles (default 1).
 *
 * Prints a per-MR line and a final "restored_reg_mr_probe: PASS/FAIL".
 * Exit 0 on all-success, 1 on any reg_mr failure, 2 on setup error.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <infiniband/verbs.h>

static struct ibv_context *open_ibdev(const char *name)
{
	struct ibv_device **list;
	struct ibv_context *ctx = NULL;
	int n, i;

	list = ibv_get_device_list(&n);
	if (!list || n == 0) {
		fprintf(stderr, "restored_reg_mr: no RDMA devices found\n");
		return NULL;
	}
	for (i = 0; i < n; i++) {
		if (strcmp(ibv_get_device_name(list[i]), name) == 0) {
			ctx = ibv_open_device(list[i]);
			break;
		}
	}
	if (!ctx) {
		fprintf(stderr, "restored_reg_mr: ibdev '%s' not found; available:",
			name);
		for (i = 0; i < n; i++)
			fprintf(stderr, " %s", ibv_get_device_name(list[i]));
		fprintf(stderr, "\n");
	}
	ibv_free_device_list(list);
	return ctx;
}

static double ms_since(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1e3 +
	       (b->tv_nsec - a->tv_nsec) / 1e6;
}

int main(int argc, char **argv)
{
	const int access = IBV_ACCESS_LOCAL_WRITE |
			   IBV_ACCESS_REMOTE_WRITE |
			   IBV_ACCESS_REMOTE_READ;
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	unsigned long count = 1;
	size_t mr_bytes;
	int fails = 0;
	void *buf;

	if (argc < 3 || argc > 4) {
		fprintf(stderr,
			"usage: %s <ibdev> <mr_bytes> [<count>]\n", argv[0]);
		return 2;
	}
	mr_bytes = (size_t)strtoull(argv[2], NULL, 0);
	if (mr_bytes == 0) {
		fprintf(stderr, "restored_reg_mr: mr_bytes must be > 0\n");
		return 2;
	}
	if (argc == 4) {
		count = strtoul(argv[3], NULL, 0);
		if (count == 0)
			count = 1;
	}

	ctx = open_ibdev(argv[1]);
	if (!ctx)
		return 2;

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		fprintf(stderr, "restored_reg_mr: ibv_alloc_pd: %s\n",
			strerror(errno));
		ibv_close_device(ctx);
		return 2;
	}

	if (posix_memalign(&buf, 4096, mr_bytes) != 0) {
		fprintf(stderr, "restored_reg_mr: posix_memalign(%zu): %s\n",
			mr_bytes, strerror(errno));
		ibv_dealloc_pd(pd);
		ibv_close_device(ctx);
		return 2;
	}
	/* Touch every page so the kernel has real pages to pin. */
	memset(buf, 0, mr_bytes);

	printf("restored_reg_mr: ibdev=%s mr_bytes=%zu count=%lu access=0x%x\n",
	       argv[1], mr_bytes, count, access);

	for (unsigned long i = 0; i < count; i++) {
		struct timespec t0, t1;
		struct ibv_mr *mr;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		mr = ibv_reg_mr(pd, buf, mr_bytes, access);
		clock_gettime(CLOCK_MONOTONIC, &t1);

		if (!mr) {
			fprintf(stderr,
				"  [%lu] FAIL ibv_reg_mr(%zu): %s\n",
				i, mr_bytes, strerror(errno));
			fails++;
			continue;
		}
		printf("  [%lu] OK reg_mr lkey=0x%x rkey=0x%x elapsed_ms=%.2f\n",
		       i, mr->lkey, mr->rkey, ms_since(&t0, &t1));
		if (ibv_dereg_mr(mr)) {
			fprintf(stderr, "  [%lu] FAIL ibv_dereg_mr: %s\n",
				i, strerror(errno));
			fails++;
		}
	}

	free(buf);
	ibv_dealloc_pd(pd);
	ibv_close_device(ctx);

	if (fails) {
		fprintf(stderr,
			"\nrestored_reg_mr_probe: FAIL (%d failure(s))\n", fails);
		return 1;
	}
	printf("\nrestored_reg_mr_probe: PASS\n");
	return 0;
}
