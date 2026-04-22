# SPDX-License-Identifier: GPL-2.0
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter

BINARIES := mlx5_vfmig mlx5_vfmig_save mlx5_vfmig_synth

all: $(BINARIES)

mlx5_vfmig: mlx5_vfmig.c ../../../include/uapi/linux/mlx5_vfmig.h
	$(CC) $(CFLAGS) -o $@ $<

mlx5_vfmig_save: mlx5_vfmig_save.c
	$(CC) $(CFLAGS) -o $@ $<

mlx5_vfmig_synth: mlx5_vfmig_synth.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINARIES)

.PHONY: all clean
