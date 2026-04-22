// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_vfmig - tiny userspace helper for the M1' /dev/mlx5_vfmig/<bdf>
 * cdev. Intended for development and triage; production users should
 * speak the ioctls from a real CRIU plugin.
 *
 * Build:
 *   cc -O2 -Wall -o mlx5_vfmig mlx5_vfmig.c
 *
 * Use:
 *   mlx5_vfmig <pf-bdf> {mark_restored|mark-restored} <vf_id>
 *   mlx5_vfmig <pf-bdf> {get_vhca_id|get-vhca-id}     <vf_id>
 *   mlx5_vfmig <pf-bdf> {query_vf|query-vf}           <vf_id>
 *   mlx5_vfmig <pf-bdf> list
 *
 * Examples:
 *   mlx5_vfmig 0000:00:08.0 mark_restored 0
 *   mlx5_vfmig 0000:00:08.0 list
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../../include/uapi/linux/mlx5_vfmig.h"

static int do_mark(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_mark_restored arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_MARK_RESTORED, &arg) < 0) {
		if (errno == EALREADY)
			fprintf(stderr,
				"vf %u is already marked restored\n", vf_id);
		else if (errno == EINVAL)
			fprintf(stderr,
				"vf_id %u out of range (have you set sriov_numvfs?)\n",
				vf_id);
		else
			perror("MARK_RESTORED");
		return 1;
	}
	printf("marked vf %u as restored\n", vf_id);
	return 0;
}

static int do_get(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_get_vhca_id arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_GET_VHCA_ID, &arg) < 0) {
		perror("GET_VHCA_ID");
		return 1;
	}
	printf("vf %u vhca_id 0x%04x\n", vf_id, arg.vhca_id);
	return 0;
}

static int query_one(int fd, unsigned int vf_id, struct mlx5_vfmig_query_vf *out)
{
	struct mlx5_vfmig_query_vf arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_QUERY_VF, &arg) < 0)
		return -errno;
	*out = arg;
	return 0;
}

static int do_query(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_query_vf info;
	int err = query_one(fd, vf_id, &info);

	if (err == -ERANGE) {
		fprintf(stderr,
			"vf_id %u out of range; PF has %u VFs\n",
			vf_id, info.num_vfs);
		return 1;
	}
	if (err) {
		errno = -err;
		perror("QUERY_VF");
		return 1;
	}
	printf("vf %u vhca_id 0x%04x restored=%u (num_vfs=%u)\n",
	       vf_id, info.vhca_id, info.restored, info.num_vfs);
	return 0;
}

static int do_list(int fd)
{
	struct mlx5_vfmig_query_vf info;
	unsigned int i, n;
	int err;

	/*
	 * vf 0 may not exist (PF has 0 VFs). Use ERANGE handling: the
	 * kernel always returns num_vfs even on out-of-range.
	 */
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
		printf("%-6u 0x%04x    %u\n",
		       i, info.vhca_id, info.restored);
	}
	return 0;
}

/*
 * Verb match that accepts either "mark_restored" or "mark-restored",
 * etc. Treats '_' and '-' as equivalent so callers don't have to
 * remember which spelling the tool uses.
 */
static int verb_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char ca = (*a == '-') ? '_' : *a;
		char cb = (*b == '-') ? '_' : *b;

		if (ca != cb)
			return 0;
	}
	return *a == 0 && *b == 0;
}

static int looks_like_bdf(const char *s)
{
	/* "DDDD:BB:DD.F" -- at least one ':' and no '/' */
	return strchr(s, ':') && !strchr(s, '/');
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <pf-bdf> <verb> [vf_id]\n"
		"  verbs: mark_restored | get_vhca_id | query_vf  (require vf_id)\n"
		"         list                                     (no vf_id)\n",
		argv0);
}

int main(int argc, char **argv)
{
	char path[256];
	const char *target;
	const char *verb;
	int fd, ret;
	unsigned int vf_id = 0;
	int needs_vf_id;

	if (argc < 3) {
		usage(argv[0]);
		return 2;
	}
	verb = argv[2];
	needs_vf_id = !verb_eq(verb, "list");

	if (needs_vf_id) {
		if (argc != 4) {
			usage(argv[0]);
			return 2;
		}
		vf_id = strtoul(argv[3], NULL, 0);
	} else if (argc != 3) {
		usage(argv[0]);
		return 2;
	}

	if (looks_like_bdf(argv[1])) {
		snprintf(path, sizeof(path), "/dev/mlx5_vfmig/%s", argv[1]);
		target = path;
	} else {
		target = argv[1];
	}

	fd = open(target, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", target, strerror(errno));
		return 1;
	}

	if (verb_eq(verb, "mark_restored"))
		ret = do_mark(fd, vf_id);
	else if (verb_eq(verb, "get_vhca_id"))
		ret = do_get(fd, vf_id);
	else if (verb_eq(verb, "query_vf"))
		ret = do_query(fd, vf_id);
	else if (verb_eq(verb, "list"))
		ret = do_list(fd);
	else {
		fprintf(stderr, "unknown verb: %s\n", verb);
		ret = 2;
	}

	close(fd);
	return ret;
}
