#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build-up test for MLX5_VFMIG_IOC_SAVE_VHCA_STATE (vfmig-rebase-0,
# require-STOP milestone). SAVE captures a *stopped* VF's firmware state
# into a read-only anon-inode fd; this harness drives only the PF cdev
# ioctls against unbound VFs (never binding the VF to mlx5_core).
#
# The VF must first be quiesced to STOP via SUSPEND_VHCA and be
# migration-enabled. read()ing the returned fd yields a 16-byte FW_DATA
# wire header (record_size, flags, tag) followed by the firmware payload;
# mlx5_vfmig_min validates that record_size matches the payload length.
#
# Subtests (the run fails if any fails):
#   1. SAVE on a RUNNING (not-yet-suspended) VF is rejected (-EINVAL).
#   2. After SUSPEND->STOP, SAVE returns a non-trivial, well-framed blob.
#   3. A second concurrent SAVE session for the same vf_id -> -EBUSY.
#   4. SAVE on an out-of-range vf_id is rejected.
#   5. SAVE on the non-migratable / not-stopped vf1 is rejected.
#   6. After RESUME->RUNNING, SAVE is rejected again (STOP gate re-applies).
#
# Usage: sudo PF=0000:08:00.0 ./test_save_vhca_state_buildup.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PF=${PF:-0000:08:00.0}
TOOL=${TOOL:-$SCRIPT_DIR/mlx5_vfmig_min}
CDEV=/dev/mlx5_vfmig/$PF
SYS=/sys/bus/pci/devices/$PF

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

# --- subtest 1: SAVE before SUSPEND rejected --------------------------
echo "=== subtest 1: SAVE on RUNNING vf0 rejected (-EINVAL) ==="
run_tool resume_vhca 0			# ensure RUNNING baseline
run_tool save_vhca_state 0
[ "$TOOL_RC" -ne 0 ] && pass "SAVE on non-stopped VF rejected" ||
	fail "SAVE on RUNNING vf0 unexpectedly succeeded"

# --- subtest 2: SUSPEND then SAVE yields a well-framed blob -----------
echo "=== subtest 2: SUSPEND->STOP then SAVE ==="
run_tool suspend_vhca 0
[ "$TOOL_RC" -eq 0 ] || { echo "FATAL: SUSPEND vf0 failed"; exit 1; }
sudo dmesg -C
run_tool save_vhca_state 0
if [ "$TOOL_RC" -eq 0 ]; then
	pass "SAVE returned a well-framed blob (record_size == payload)"
else
	fail "SAVE after SUSPEND failed rc=$TOOL_RC"
fi
# payload must be non-trivial (> 0 bytes)
if echo "$TOOL_OUT" | grep -qE "payload 0\)"; then
	fail "SAVE produced an empty payload"
else
	pass "SAVE payload is non-trivial"
fi
dlog "vfmig: saved vf 0 .*bytes" >/dev/null &&
	pass "logged 'saved vf 0 ... bytes'" || fail "missing kernel 'saved vf 0' line"

# --- subtest 3: concurrent SAVE session -> EBUSY ----------------------
echo "=== subtest 3: second concurrent SAVE on vf0 -> EBUSY ==="
run_tool save_busy 0
[ "$TOOL_RC" -eq 0 ] && pass "concurrent SAVE rejected with EBUSY" ||
	fail "concurrent SAVE not rejected with EBUSY (rc=$TOOL_RC)"

# --- subtest 4: out-of-range vf_id ------------------------------------
echo "=== subtest 4: SAVE out-of-range vf_id rejected ==="
run_tool save_vhca_state 99
[ "$TOOL_RC" -ne 0 ] && pass "out-of-range SAVE rejected" ||
	fail "out-of-range SAVE unexpectedly succeeded"

# --- subtest 5: non-migratable / not-stopped vf1 ----------------------
echo "=== subtest 5: SAVE on vf1 (non-migratable, not stopped) rejected ==="
run_tool save_vhca_state 1
[ "$TOOL_RC" -ne 0 ] && pass "SAVE on vf1 rejected" ||
	fail "SAVE on vf1 unexpectedly succeeded"

# --- subtest 6: RESUME re-arms the STOP gate --------------------------
echo "=== subtest 6: after RESUME->RUNNING, SAVE rejected again ==="
run_tool resume_vhca 0
[ "$TOOL_RC" -eq 0 ] || { echo "FATAL: RESUME vf0 failed"; exit 1; }
run_tool save_vhca_state 0
[ "$TOOL_RC" -ne 0 ] && pass "SAVE rejected after RESUME (STOP gate re-applies)" ||
	fail "SAVE on resumed vf0 unexpectedly succeeded"

# --- summary ----------------------------------------------------------
echo
echo "==== save_vhca_state build-up summary: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" -eq 0 ] || { echo "RESULT: FAIL"; exit 1; }
echo "RESULT: PASS"
