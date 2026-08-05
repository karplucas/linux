#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build-up test for MLX5_VFMIG_IOC_LOAD_VHCA_STATE (vfmig-rebase-0, eager
# require-STOP milestone). LOAD is the inverse of SAVE: a write-only
# anon-inode fd that ingests the SAVE blob and runs LOAD_VHCA_STATE on the
# (already suspended) VHCA. This harness drives only PF cdev ioctls
# against an unbound VF and validates a SAVE->LOAD round-trip.
#
# Firmware rejects LOAD unless the VHCA is fully suspended to STOP, so the
# caller must SUSPEND_VHCA first and RESUME_VHCA afterwards. The eager
# ioctl runs LOAD_VHCA_STATE on the existing VHCA; wiring LOAD into the VF
# restore probe (skip INIT_HCA) is a later milestone.
#
# Subtests (the run fails if any fails):
#   1. LOAD on a RUNNING (not-suspended) VF is rejected (-EINVAL).
#   2. Round-trip: SUSPEND->STOP, SAVE to a file, LOAD it back, RESUME.
#   3. A second concurrent LOAD session for the same vf_id -> -EBUSY.
#   4. SAVE and LOAD are mutually exclusive per vf_id (-EBUSY).
#   5. LOAD on an out-of-range vf_id is rejected.
#   6. LOAD on the non-migratable / not-stopped vf1 is rejected.
#
# Usage: sudo PF=0000:08:00.0 ./test_load_vhca_state_buildup.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PF=${PF:-0000:08:00.0}
TOOL=${TOOL:-$SCRIPT_DIR/mlx5_vfmig_min}
CDEV=/dev/mlx5_vfmig/$PF
SYS=/sys/bus/pci/devices/$PF
BLOB=$(mktemp /tmp/vfmig_blob.XXXXXX)

PASS=0
FAIL=0
pass() { echo "  PASS: $*"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }

[ -x "$TOOL" ] || { echo "build $TOOL first: make -C $SCRIPT_DIR/.. buildup/mlx5_vfmig_min"; exit 1; }
[ -e "$CDEV" ] || { echo "cdev $CDEV missing (build-up-branch kernel loaded?)"; exit 1; }

run_tool() {
	TOOL_OUT=$(sudo "$TOOL" "$PF" "$@" 2>&1)
	TOOL_RC=$?
	echo "    \$ mlx5_vfmig_min $PF $* -> rc=$TOOL_RC"
	[ -n "$TOOL_OUT" ] && echo "$TOOL_OUT" | sed 's/^/      /'
	return 0
}

dlog() { sudo dmesg | grep -E "$1"; }

wait_for_path() { for _ in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }

cleanup() {
	rm -f "$BLOB"
	echo 1 | sudo tee "$SYS/sriov_drivers_autoprobe" >/dev/null 2>&1 || true
	echo 0 | sudo tee "$SYS/sriov_numvfs"            >/dev/null 2>&1 || true
}
trap cleanup EXIT

# --- provision 2 unbound VFs; make only vf0 migratable ----------------
echo "=== provision 2 unbound VFs on $PF (vf0 migratable, vf1 not) ==="
echo 0 | sudo tee "$SYS/sriov_numvfs"            >/dev/null
echo 0 | sudo tee "$SYS/sriov_drivers_autoprobe" >/dev/null
echo 2 | sudo tee "$SYS/sriov_numvfs"            >/dev/null
wait_for_path "$SYS/virtfn1" || { echo "FAIL: VFs did not appear"; exit 1; }

run_tool enable_migratable 0
if [ "$TOOL_RC" -ne 0 ]; then
	echo "$TOOL_OUT" | grep -qiE "not supported|EOPNOTSUPP|Operation not supported" &&
		{ echo "SKIP: PF firmware lacks migration caps"; exit 0; }
	echo "FATAL: enable_migratable vf0 failed"; exit 1
fi

# --- subtest 1: LOAD before SUSPEND rejected --------------------------
echo "=== subtest 1: LOAD on RUNNING vf0 rejected (-EINVAL) ==="
run_tool resume_vhca 0			# ensure RUNNING baseline
run_tool load_vhca_state 0 /dev/null
[ "$TOOL_RC" -ne 0 ] && pass "LOAD on non-stopped VF rejected" ||
	fail "LOAD on RUNNING vf0 unexpectedly succeeded"

# --- subtest 2: SAVE->LOAD round-trip ---------------------------------
echo "=== subtest 2: SUSPEND->STOP, SAVE, LOAD, RESUME ==="
run_tool suspend_vhca 0
[ "$TOOL_RC" -eq 0 ] || { echo "FATAL: SUSPEND vf0 failed"; exit 1; }
run_tool save_vhca_state 0 "$BLOB"
[ "$TOOL_RC" -eq 0 ] || { echo "FATAL: SAVE vf0 failed"; exit 1; }
[ -s "$BLOB" ] && pass "SAVE produced a non-empty blob ($(stat -c%s "$BLOB") bytes)" ||
	fail "SAVE blob is empty"
sudo dmesg -C
run_tool load_vhca_state 0 "$BLOB"
[ "$TOOL_RC" -eq 0 ] && pass "LOAD of saved blob returned 0" ||
	fail "LOAD failed rc=$TOOL_RC"
dlog "vfmig: loaded vf 0 .*bytes" >/dev/null &&
	pass "logged 'loaded vf 0 ... bytes'" || fail "missing kernel 'loaded vf 0' line"
run_tool resume_vhca 0
[ "$TOOL_RC" -eq 0 ] && pass "RESUME after LOAD returned 0" ||
	fail "RESUME after LOAD failed rc=$TOOL_RC"

# --- subtest 3: concurrent LOAD session -> EBUSY ----------------------
echo "=== subtest 3: second concurrent LOAD on vf0 -> EBUSY ==="
run_tool suspend_vhca 0
run_tool load_busy 0
[ "$TOOL_RC" -eq 0 ] && pass "concurrent LOAD rejected with EBUSY" ||
	fail "concurrent LOAD not rejected with EBUSY (rc=$TOOL_RC)"

# --- subtest 4: SAVE vs LOAD mutual exclusion -------------------------
echo "=== subtest 4: SAVE excluded while a LOAD session is open -> EBUSY ==="
run_tool save_excl 0		# VF still at STOP from subtest 3
[ "$TOOL_RC" -eq 0 ] && pass "SAVE during open LOAD rejected with EBUSY" ||
	fail "SAVE/LOAD not mutually exclusive (rc=$TOOL_RC)"
run_tool resume_vhca 0

# --- subtest 5: out-of-range vf_id ------------------------------------
echo "=== subtest 5: LOAD out-of-range vf_id rejected ==="
run_tool load_vhca_state 99 /dev/null
[ "$TOOL_RC" -ne 0 ] && pass "out-of-range LOAD rejected" ||
	fail "out-of-range LOAD unexpectedly succeeded"

# --- subtest 6: non-migratable / not-stopped vf1 ----------------------
echo "=== subtest 6: LOAD on vf1 (non-migratable, not stopped) rejected ==="
run_tool load_vhca_state 1 /dev/null
[ "$TOOL_RC" -ne 0 ] && pass "LOAD on vf1 rejected" ||
	fail "LOAD on vf1 unexpectedly succeeded"

# --- summary ----------------------------------------------------------
echo
echo "==== load_vhca_state build-up summary: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" -eq 0 ] || { echo "RESULT: FAIL"; exit 1; }
echo "RESULT: PASS"
