// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_vfmig_min - minimal /dev/mlx5_vfmig/<bdf> helper for the build-up
 * harness. Speaks only the incrementally-upstreamed ioctl surface (see
 * mlx5_vfmig_buildup.h): GET_VHCA_ID, MARK_RESTORED, QUERY_VF,
 * ENABLE_MIGRATABLE, SUSPEND/RESUME/SAVE/LOAD_VHCA_STATE, SET_TRACKED.
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
 *   mlx5_vfmig_min <pf-bdf> get_vhca_id       <vf_id>
 *   mlx5_vfmig_min <pf-bdf> mark_restored     <vf_id>
 *   mlx5_vfmig_min <pf-bdf> query_vf          <vf_id>
 *   mlx5_vfmig_min <pf-bdf> enable_migratable <vf_id>
 *   mlx5_vfmig_min <pf-bdf> suspend_vhca      <vf_id> [initiator|responder]
 *   mlx5_vfmig_min <pf-bdf> resume_vhca       <vf_id> [initiator|responder]
 *   mlx5_vfmig_min <pf-bdf> save_vhca_state   <vf_id> [outfile]
 *   mlx5_vfmig_min <pf-bdf> save_keep         <vf_id> [outfile]
 *   mlx5_vfmig_min <pf-bdf> load_vhca_state   <vf_id> <infile>
 *   mlx5_vfmig_min <pf-bdf> track             <vf_id>
 *   mlx5_vfmig_min <pf-bdf> untrack           <vf_id>
 *   mlx5_vfmig_min <pf-bdf> list
 */

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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

static int do_enable_migratable(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_enable_migratable arg = { .vf_id = vf_id };

	if (ioctl(fd, MLX5_VFMIG_IOC_ENABLE_MIGRATABLE, &arg) < 0) {
		perror("ENABLE_MIGRATABLE");
		return 1;
	}
	printf("vf %u: migratable cap enabled (call before driver bind)\n",
	       vf_id);
	return 0;
}

static int do_set_tracked(int fd, unsigned int vf_id, unsigned int enable)
{
	struct mlx5_vfmig_set_tracked arg = { .vf_id = vf_id, .enable = enable };

	if (ioctl(fd, MLX5_VFMIG_IOC_SET_TRACKED, &arg) < 0) {
		perror("SET_TRACKED");
		return 1;
	}
	printf("vf %u: %s\n", vf_id, enable ? "tracked" : "untracked");
	return 0;
}

/* Map an optional direction word to a MLX5_VFMIG_DIR_FLAG_* mask. */
static int parse_dir(const char *s, unsigned int *flags)
{
	if (!s || !strcmp(s, "all") || !strcmp(s, "both")) {
		*flags = 0;
		return 0;
	}
	if (!strcmp(s, "initiator") || !strcmp(s, "init")) {
		*flags = MLX5_VFMIG_DIR_FLAG_INITIATOR;
		return 0;
	}
	if (!strcmp(s, "responder") || !strcmp(s, "resp")) {
		*flags = MLX5_VFMIG_DIR_FLAG_RESPONDER;
		return 0;
	}
	return -1;
}

static int do_suspend(int fd, unsigned int vf_id, unsigned int flags)
{
	struct mlx5_vfmig_suspend_vhca arg = { .vf_id = vf_id, .flags = flags };

	if (ioctl(fd, MLX5_VFMIG_IOC_SUSPEND_VHCA, &arg) < 0) {
		perror("SUSPEND_VHCA");
		return 1;
	}
	printf("vf %u: suspended (flags 0x%x)\n", vf_id, flags);
	return 0;
}

static int do_resume(int fd, unsigned int vf_id, unsigned int flags)
{
	struct mlx5_vfmig_resume_vhca arg = { .vf_id = vf_id, .flags = flags };

	if (ioctl(fd, MLX5_VFMIG_IOC_RESUME_VHCA, &arg) < 0) {
		perror("RESUME_VHCA");
		return 1;
	}
	printf("vf %u: resumed (flags 0x%x)\n", vf_id, flags);
	return 0;
}

/* Drain @save_fd fully into a malloc'd buffer; *len gets the byte count. */
static int slurp_fd(int save_fd, unsigned char **buf, size_t *len)
{
	size_t cap = 1 << 16, off = 0;
	unsigned char *p;

	p = malloc(cap);
	if (!p)
		return -ENOMEM;

	for (;;) {
		ssize_t r;

		if (off == cap) {
			unsigned char *np = realloc(p, cap * 2);

			if (!np) {
				free(p);
				return -ENOMEM;
			}
			p = np;
			cap *= 2;
		}
		r = read(save_fd, p + off, cap - off);
		if (r < 0) {
			int e = -errno;

			free(p);
			return e;
		}
		if (r == 0)
			break;
		off += r;
	}

	*buf = p;
	*len = off;
	return 0;
}

static int do_save_vhca_state(int fd, unsigned int vf_id, unsigned int flags,
			      const char *outfile)
{
	struct mlx5_vfmig_save_state arg = { .vf_id = vf_id, .flags = flags };
	struct vfmig_wire_header hdr;
	unsigned char *buf = NULL;
	uint64_t record_size;
	size_t len = 0;
	int err;

	if (ioctl(fd, MLX5_VFMIG_IOC_SAVE_VHCA_STATE, &arg) < 0) {
		perror("SAVE_VHCA_STATE");
		return 1;
	}

	err = slurp_fd(arg.save_fd, &buf, &len);
	close(arg.save_fd);
	if (err) {
		errno = -err;
		perror("read(save_fd)");
		return 1;
	}

	if (len < sizeof(hdr)) {
		fprintf(stderr, "vf %u: short save stream (%zu bytes)\n",
			vf_id, len);
		free(buf);
		return 1;
	}

	memcpy(&hdr, buf, sizeof(hdr));
	record_size = le64toh(hdr.record_size);
	printf("vf %u: saved %zu bytes (fw_data tag %u flags 0x%x record_size %llu, payload %zu)\n",
	       vf_id, len, le32toh(hdr.tag), le32toh(hdr.flags),
	       (unsigned long long)record_size, len - sizeof(hdr));

	if (le32toh(hdr.tag) != VFMIG_WIRE_TAG_FW_DATA) {
		fprintf(stderr, "vf %u: unexpected wire tag %u\n",
			vf_id, le32toh(hdr.tag));
		free(buf);
		return 1;
	}
	if (record_size != len - sizeof(hdr)) {
		fprintf(stderr,
			"vf %u: record_size %llu != payload %zu\n",
			vf_id, (unsigned long long)record_size,
			len - sizeof(hdr));
		free(buf);
		return 1;
	}

	if (outfile) {
		FILE *f = fopen(outfile, "wb");

		if (!f || fwrite(buf, 1, len, f) != len) {
			perror("write outfile");
			if (f)
				fclose(f);
			free(buf);
			return 1;
		}
		fclose(f);
		printf("vf %u: wrote %zu bytes to %s\n", vf_id, len, outfile);
	}

	free(buf);
	return 0;
}

/*
 * Open a SAVE session and, while its fd is still open, attempt a second
 * SAVE on the same vf_id. Succeeds (returns 0) only if the second attempt
 * is rejected with EBUSY.
 */
static int do_save_busy(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_save_state a = { .vf_id = vf_id };
	struct mlx5_vfmig_save_state b = { .vf_id = vf_id };
	int ret = 0;

	if (ioctl(fd, MLX5_VFMIG_IOC_SAVE_VHCA_STATE, &a) < 0) {
		perror("SAVE_VHCA_STATE (first)");
		return 1;
	}

	if (ioctl(fd, MLX5_VFMIG_IOC_SAVE_VHCA_STATE, &b) == 0) {
		fprintf(stderr,
			"vf %u: second concurrent SAVE unexpectedly succeeded\n",
			vf_id);
		close(b.save_fd);
		ret = 1;
	} else if (errno != EBUSY) {
		fprintf(stderr, "vf %u: second SAVE failed with %s, want EBUSY\n",
			vf_id, strerror(errno));
		ret = 1;
	} else {
		printf("vf %u: second concurrent SAVE rejected with EBUSY\n",
		       vf_id);
	}

	close(a.save_fd);
	return ret;
}

/* Open a LOAD session and stream @infile's bytes into the load fd. */
static int do_load_vhca_state(int fd, unsigned int vf_id, const char *infile)
{
	struct mlx5_vfmig_load_state arg = { .vf_id = vf_id };
	unsigned char buf[1 << 16];
	size_t total = 0;
	FILE *f;
	int ret = 0;

	f = fopen(infile, "rb");
	if (!f) {
		perror("open infile");
		return 1;
	}

	if (ioctl(fd, MLX5_VFMIG_IOC_LOAD_VHCA_STATE, &arg) < 0) {
		perror("LOAD_VHCA_STATE");
		fclose(f);
		return 1;
	}

	for (;;) {
		size_t got = fread(buf, 1, sizeof(buf), f);
		unsigned char *p = buf;

		if (!got)
			break;
		while (got) {
			ssize_t w = write(arg.load_fd, p, got);

			if (w < 0) {
				perror("write(load_fd)");
				ret = 1;
				goto out;
			}
			p += w;
			got -= w;
			total += w;
		}
	}
out:
	if (close(arg.load_fd) < 0 && !ret) {
		perror("close(load_fd)");
		ret = 1;
	}
	fclose(f);
	if (!ret)
		printf("vf %u: staged %zu bytes from %s (applies on next probe)\n",
		       vf_id, total, infile);
	return ret;
}

/*
 * Open a LOAD session and, while its fd is open, attempt a second LOAD on
 * the same vf_id. Succeeds (returns 0) only if the second is rejected with
 * EBUSY. The VF must already be at STOP.
 */
static int do_load_busy(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_load_state a = { .vf_id = vf_id };
	struct mlx5_vfmig_load_state b = { .vf_id = vf_id };
	int ret = 0;

	if (ioctl(fd, MLX5_VFMIG_IOC_LOAD_VHCA_STATE, &a) < 0) {
		perror("LOAD_VHCA_STATE (first)");
		return 1;
	}

	if (ioctl(fd, MLX5_VFMIG_IOC_LOAD_VHCA_STATE, &b) == 0) {
		fprintf(stderr,
			"vf %u: second concurrent LOAD unexpectedly succeeded\n",
			vf_id);
		close(b.load_fd);
		ret = 1;
	} else if (errno != EBUSY) {
		fprintf(stderr, "vf %u: second LOAD failed with %s, want EBUSY\n",
			vf_id, strerror(errno));
		ret = 1;
	} else {
		printf("vf %u: second concurrent LOAD rejected with EBUSY\n",
		       vf_id);
	}

	close(a.load_fd);
	return ret;
}

/*
 * Open a LOAD session and, while its fd is open, attempt a SAVE on the
 * same vf_id. Succeeds (returns 0) only if SAVE is rejected with EBUSY,
 * proving SAVE/LOAD are mutually exclusive per VF. VF must be at STOP.
 */
static int do_save_excl(int fd, unsigned int vf_id)
{
	struct mlx5_vfmig_load_state l = { .vf_id = vf_id };
	struct mlx5_vfmig_save_state s = { .vf_id = vf_id };
	int ret = 0;

	if (ioctl(fd, MLX5_VFMIG_IOC_LOAD_VHCA_STATE, &l) < 0) {
		perror("LOAD_VHCA_STATE (first)");
		return 1;
	}

	if (ioctl(fd, MLX5_VFMIG_IOC_SAVE_VHCA_STATE, &s) == 0) {
		fprintf(stderr,
			"vf %u: SAVE during open LOAD unexpectedly succeeded\n",
			vf_id);
		close(s.save_fd);
		ret = 1;
	} else if (errno != EBUSY) {
		fprintf(stderr, "vf %u: SAVE during LOAD failed with %s, want EBUSY\n",
			vf_id, strerror(errno));
		ret = 1;
	} else {
		printf("vf %u: SAVE during open LOAD rejected with EBUSY\n",
		       vf_id);
	}

	close(l.load_fd);
	return ret;
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
	printf("vf %u: vhca_id 0x%04x restored %u tracked %u (num_vfs %u)\n",
	       vf_id, info.vhca_id, info.restored, info.tracked, info.num_vfs);
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

	printf("%-6s %-9s %-9s %s\n", "vf_id", "vhca_id", "restored", "tracked");
	for (i = 0; i < n; i++) {
		err = query_one(fd, i, &info);
		if (err) {
			errno = -err;
			fprintf(stderr, "QUERY_VF vf %u: %s\n",
				i, strerror(errno));
			continue;
		}
		printf("%-6u 0x%04x    %-9u %u\n",
		       i, info.vhca_id, info.restored, info.tracked);
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
		"         enable_migratable <vf_id>\n"
		"         suspend_vhca <vf_id> [initiator|responder]\n"
		"         resume_vhca <vf_id> [initiator|responder]\n"
		"         save_vhca_state <vf_id> [outfile]\n"
		"         save_keep <vf_id> [outfile]\n"
		"         save_busy <vf_id>\n"
		"         load_vhca_state <vf_id> <infile>\n"
		"         load_busy <vf_id>\n"
		"         save_excl <vf_id>\n"
		"         track <vf_id>\n"
		"         untrack <vf_id>\n"
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
	} else if (!strcmp(verb, "enable_migratable") ||
		   !strcmp(verb, "enable-migratable")) {
		ret = do_enable_migratable(fd, strtoul(argv[3], NULL, 0));
	} else if (!strcmp(verb, "suspend_vhca") ||
		   !strcmp(verb, "suspend-vhca")) {
		unsigned int flags = 0;

		if (parse_dir(argc > 4 ? argv[4] : NULL, &flags)) {
			usage(argv[0]);
			ret = 2;
		} else {
			ret = do_suspend(fd, strtoul(argv[3], NULL, 0), flags);
		}
	} else if (!strcmp(verb, "resume_vhca") ||
		   !strcmp(verb, "resume-vhca")) {
		unsigned int flags = 0;

		if (parse_dir(argc > 4 ? argv[4] : NULL, &flags)) {
			usage(argv[0]);
			ret = 2;
		} else {
			ret = do_resume(fd, strtoul(argv[3], NULL, 0), flags);
		}
	} else if (!strcmp(verb, "save_vhca_state") ||
		   !strcmp(verb, "save-vhca-state")) {
		ret = do_save_vhca_state(fd, strtoul(argv[3], NULL, 0), 0,
					 argc > 4 ? argv[4] : NULL);
	} else if (!strcmp(verb, "save_keep") ||
		   !strcmp(verb, "save-keep")) {
		ret = do_save_vhca_state(fd, strtoul(argv[3], NULL, 0),
					 MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED,
					 argc > 4 ? argv[4] : NULL);
	} else if (!strcmp(verb, "save_busy") ||
		   !strcmp(verb, "save-busy")) {
		ret = do_save_busy(fd, strtoul(argv[3], NULL, 0));
	} else if (!strcmp(verb, "load_vhca_state") ||
		   !strcmp(verb, "load-vhca-state")) {
		if (argc < 5) {
			usage(argv[0]);
			ret = 2;
		} else {
			ret = do_load_vhca_state(fd, strtoul(argv[3], NULL, 0),
						 argv[4]);
		}
	} else if (!strcmp(verb, "load_busy") ||
		   !strcmp(verb, "load-busy")) {
		ret = do_load_busy(fd, strtoul(argv[3], NULL, 0));
	} else if (!strcmp(verb, "save_excl") ||
		   !strcmp(verb, "save-excl")) {
		ret = do_save_excl(fd, strtoul(argv[3], NULL, 0));
	} else if (!strcmp(verb, "track")) {
		ret = do_set_tracked(fd, strtoul(argv[3], NULL, 0), 1);
	} else if (!strcmp(verb, "untrack")) {
		ret = do_set_tracked(fd, strtoul(argv[3], NULL, 0), 0);
	} else {
		usage(argv[0]);
		ret = 2;
	}

	close(fd);
	return ret;
}
