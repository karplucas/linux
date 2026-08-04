// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_vfmig_min - minimal /dev/mlx5_vfmig/<bdf> helper for the build-up
 * harness. Speaks only the incrementally-upstreamed ioctl surface (see
 * mlx5_vfmig_buildup.h): GET_VHCA_ID, MARK_RESTORED, QUERY_VF.
 *
 * The full-featured tools/mlx5_vfmig helper targets the oracle's
 * complete ABI and will not run against a build-up-branch kernel
 * (mismatched QUERY_VF command number, unimplemented verbs). This
 * stripped helper exists so the build-up smoke can go green the moment
 * the branch kernel is loaded, and grows one verb at a time alongside
 * the kernel.
 *
 * Build via the directory Makefile:
 *   make -C tools/testing/criu_rdma buildup/mlx5_vfmig_min
 *
 * Use:
 *   mlx5_vfmig_min <pf-bdf> get_vhca_id  <vf_id>
 *   mlx5_vfmig_min <pf-bdf> mark_restored <vf_id>
 *   mlx5_vfmig_min <pf-bdf> query_vf     <vf_id>
 *   mlx5_vfmig_min <pf-bdf> list
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "mlx5_vfmig_buildup.h"

static int query_one(int fd, unsigned int vf_id,
		     struct mlx5_vfmig_query_vf *info)
{
	memset(info, 0, sizeof(*info));
	info->vf_id = vf_id;
	if (ioctl(fd, MLX5_VFMIG_IOC_QUERY_VF, info) < 0)
		return -errno;
	return 0;
}

static int do_get_vhca_id(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_get_vhca_id arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_GET_VHCA_ID, &arg) < 0) {
		perror("GET_VHCA_ID");
		return 1;
	}
	printf("vf %u: vhca_id 0x%04x\n", vf_id, arg.vhca_id);
	return 0;
}

static int do_mark_restored(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_mark_restored arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_MARK_RESTORED, &arg) < 0) {
		perror("MARK_RESTORED");
		return 1;
	}
	printf("vf %u: marked restored\n", vf_id);
	return 0;
}

static int do_query_vf(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_query_vf info;
	int err;

	err = query_one(fd, vf_id, &info);
	if (err) {
		errno = -err;
		perror("QUERY_VF");
		return 1;
	}
	printf("vf %u: vhca_id 0x%04x restored %u (num_vfs %u)\n",
	       vf_id, info.vhca_id, info.restored, info.num_vfs);
	return 0;
}

static int do_list(int fd)
{
	struct mlx5_vfmig_query_vf info;
	unsigned int i, n;
	int err;

	err = query_one(fd, 0, &info);
	if (err && err != -ERANGE) {
		errno = -err;
		perror("QUERY_VF");
		return 1;
	}
	n = info.num_vfs;
	if (n == 0) {
		printf("no VFs provisioned (sriov_numvfs == 0)\n");
		return 0;
	}

	printf("%-6s %-9s %s\n", "vf_id", "vhca_id", "restored");
	for (i = 0; i < n; i++) {
		err = query_one(fd, i, &info);
		if (err) {
			errno = -err;
			fprintf(stderr, "QUERY_VF vf %u: %s\n",
				i, strerror(errno));
			continue;
		}
		printf("%-6u 0x%04x    %u\n", i, info.vhca_id, info.restored);
	}
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s <pf-bdf> <verb> [args]\n"
		"  verbs: get_vhca_id <vf_id>\n"
		"         mark_restored <vf_id>\n"
		"         query_vf <vf_id>\n"
		"         list\n",
		argv0);
}

int main(int argc, char **argv)
{
	char path[128];
	const char *verb;
	int fd, ret;

	if (argc < 3) {
		usage(argv[0]);
		return 2;
	}

	snprintf(path, sizeof(path), "/dev/mlx5_vfmig/%s", argv[1]);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}

	verb = argv[2];
	if (!strcmp(verb, "list")) {
		ret = do_list(fd);
	} else if (argc < 4) {
		usage(argv[0]);
		ret = 2;
	} else if (!strcmp(verb, "get_vhca_id") || !strcmp(verb, "get-vhca-id")) {
		ret = do_get_vhca_id(fd, strtoul(argv[3], NULL, 0));
	} else if (!strcmp(verb, "mark_restored") ||
		   !strcmp(verb, "mark-restored")) {
		ret = do_mark_restored(fd, strtoul(argv[3], NULL, 0));
	} else if (!strcmp(verb, "query_vf") || !strcmp(verb, "query-vf")) {
		ret = do_query_vf(fd, strtoul(argv[3], NULL, 0));
	} else {
		usage(argv[0]);
		ret = 2;
	}

	close(fd);
	return ret;
}
