#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build-up test for MLX5_VFMIG_IOC_ENABLE_MIGRATABLE (vfmig-rebase-0).
# Exercises the real firmware effect: set the per-VF migratable cap on an
# unbound VF, and confirm the ioctl's idempotent contract.
#
# Acceptance criteria:
#   1. ENABLE_MIGRATABLE on a freshly-provisioned, unbound VF returns 0.
#   2. A second ENABLE_MIGRATABLE on the same VF also returns 0 (the bit
#      is already set; no firmware traffic).
#   3. ENABLE_MIGRATABLE on an out-of-range vf_id fails.
#
# A PF whose firmware lacks migration / vhca_resource_manager makes the
# ioctl return -EOPNOTSUPP; this test then reports SKIP rather than FAIL.
#
# Usage: sudo PF=0000:08:00.0 ./test_enable_migratable_buildup.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PF=${PF:-0000:08:00.0}
TOOL=${TOOL:-$SCRIPT_DIR/mlx5_vfmig_min}
CDEV=/dev/mlx5_vfmig/$PF
SYS=/sys/bus/pci/devices/$PF

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit 0; }

[ -x "$TOOL" ] || fail "build $TOOL first: make -C $SCRIPT_DIR/.. buildup/mlx5_vfmig_min"
[ -e "$CDEV" ] || fail "cdev $CDEV missing (is a build-up-branch kernel loaded?)"

# clean slate + close the autoprobe window so the VF stays unbound
echo 0 | sudo tee "$SYS/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$SYS/sriov_drivers_autoprobe" >/dev/null

cleanup() {
	echo 1 | sudo tee "$SYS/sriov_drivers_autoprobe" >/dev/null 2>&1 || true
	echo 0 | sudo tee "$SYS/sriov_numvfs" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo 1 | sudo tee "$SYS/sriov_numvfs" >/dev/null
for _ in $(seq 1 20); do
	[ -e "$SYS/virtfn0" ] && break
	sleep 0.1
done
[ -e "$SYS/virtfn0" ] || fail "VF did not appear"

# 1. first enable -- may legitimately be unsupported by the PF firmware
if ! out=$(sudo "$TOOL" "$PF" enable_migratable 0 2>&1); then
	echo "$out"
	echo "$out" | grep -qi "not supported\|EOPNOTSUPP\|95" &&
		skip "PF firmware lacks migration caps (EOPNOTSUPP)"
	fail "ENABLE_MIGRATABLE vf 0 failed unexpectedly"
fi
pass "ENABLE_MIGRATABLE vf 0"

# 2. idempotent second call
sudo "$TOOL" "$PF" enable_migratable 0
pass "ENABLE_MIGRATABLE vf 0 idempotent"

# 3. out-of-range vf_id must fail
if sudo "$TOOL" "$PF" enable_migratable 9999; then
	fail "ENABLE_MIGRATABLE on out-of-range vf_id should have failed"
fi
pass "ENABLE_MIGRATABLE rejected out-of-range vf_id"

echo "ALL PASS"
