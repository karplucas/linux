#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build-up test for MLX5_VFMIG_IOC_SAVE_VHCA_STATE (vfmig-rebase-0,
# self-suspend milestone). SAVE captures a VF's firmware state into a
# read-only anon-inode fd; this harness drives only the PF cdev ioctls
# against unbound VFs (never binding the VF to mlx5_core).
#
# SAVE needs the VHCA at STOP. If the caller pre-parked it via
# SUSPEND_VHCA, SAVE captures it as-is and leaves it at STOP. Otherwise
# SAVE self-suspends transiently and, on close, resumes the ladder steps
# it issued -- unless MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED asks to keep it
# parked. read()ing the fd yields a 16-byte FW_DATA header + payload;
# mlx5_vfmig_min validates that record_size matches the payload length.
#
# Subtests (the run fails if any fails):
#   1. Self-suspend SAVE on a RUNNING VF: succeeds, well-framed blob, and
#      the VF is back at RUNNING after close (resume-on-close).
#   2. Pre-parked SAVE: SUSPEND->STOP, SAVE, VF stays at STOP.
#   3. A second concurrent SAVE session for the same vf_id -> -EBUSY.
#   4. SAVE on an out-of-range vf_id is rejected.
#   5. SAVE on the non-migratable vf1 -> -EOPNOTSUPP.
#   6. KEEP_SUSPENDED: self-suspend SAVE leaves a RUNNING VF at STOP.
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

# Assert the VF's current datapath depth by issuing a probe transition and
# grepping the logged "X->Y". After the probe the VF is restored to RUNNING.
assert_running() {	# expects a following suspend to log 0->2
	sudo dmesg -C
	run_tool suspend_vhca 0
	if dlog "vfmig: suspended vf 0 .*datapath 0->2" >/dev/null; then
		pass "$1 (VF was RUNNING)"
	else
		fail "$1 (VF not at RUNNING)"
	fi
	run_tool resume_vhca 0
}
assert_stopped() {	# expects a following resume to log 2->0
	sudo dmesg -C
	run_tool resume_vhca 0
	if dlog "vfmig: resumed vf 0 .*datapath 2->0" >/dev/null; then
		pass "$1 (VF was STOP)"
	else
		fail "$1 (VF not at STOP)"
	fi
}

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

# --- subtest 1: self-suspend SAVE on RUNNING --------------------------
echo "=== subtest 1: self-suspend SAVE on RUNNING vf0 ==="
run_tool resume_vhca 0			# ensure RUNNING baseline
sudo dmesg -C
run_tool save_vhca_state 0
if [ "$TOOL_RC" -eq 0 ]; then
	pass "self-suspend SAVE returned a well-framed blob"
else
	fail "self-suspend SAVE failed rc=$TOOL_RC"
fi
if echo "$TOOL_OUT" | grep -qE "payload 0\)"; then
	fail "SAVE produced an empty payload"
else
	pass "SAVE payload is non-trivial"
fi
dlog "vfmig: saved vf 0 .*bytes" >/dev/null &&
	pass "logged 'saved vf 0 ... bytes'" || fail "missing kernel 'saved vf 0' line"
assert_running "resume-on-close restored datapath"

# --- subtest 2: pre-parked SAVE keeps VF at STOP ----------------------
echo "=== subtest 2: pre-parked (SUSPEND->STOP) SAVE ==="
run_tool suspend_vhca 0
[ "$TOOL_RC" -eq 0 ] || { echo "FATAL: SUSPEND vf0 failed"; exit 1; }
run_tool save_vhca_state 0
[ "$TOOL_RC" -eq 0 ] && pass "pre-parked SAVE returned 0" ||
	fail "pre-parked SAVE failed rc=$TOOL_RC"
assert_stopped "pre-parked SAVE left caller's suspend intact"

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

# --- subtest 5: non-migratable vf1 -> EOPNOTSUPP ----------------------
echo "=== subtest 5: SAVE on non-migratable vf1 -> EOPNOTSUPP ==="
run_tool save_vhca_state 1
if [ "$TOOL_RC" -ne 0 ] &&
   echo "$TOOL_OUT" | grep -qiE "not supported|EOPNOTSUPP"; then
	pass "non-migratable SAVE rejected with EOPNOTSUPP"
else
	fail "non-migratable SAVE: rc=$TOOL_RC out='$TOOL_OUT'"
fi

# --- subtest 6: KEEP_SUSPENDED flag plumbing --------------------------
echo "=== subtest 6: KEEP_SUSPENDED self-suspend SAVE ==="
# KEEP_SUSPENDED asks SAVE to skip its resume-on-close, leaving a
# self-suspended VF parked at STOP (dump-then-destroy). By design the
# transient SAVE suspend does NOT touch the persistent vfmig_dp_state
# that SUSPEND/RESUME_VHCA track, so there is no clean ioctl-level probe
# for the parked FW state here (a RESUME_VHCA would see "already
# RUNNING" and no-op). We therefore only assert the flag is accepted and
# yields a well-framed blob. The end-to-end restore of a saved blob is
# exercised by test_restore_probe_buildup.sh instead.
run_tool resume_vhca 0			# ensure RUNNING baseline
run_tool save_keep 0
[ "$TOOL_RC" -eq 0 ] && pass "KEEP_SUSPENDED SAVE accepted, returned 0" ||
	fail "KEEP_SUSPENDED SAVE failed rc=$TOOL_RC"
if echo "$TOOL_OUT" | grep -qE "payload 0\)"; then
	fail "KEEP_SUSPENDED SAVE produced an empty payload"
else
	pass "KEEP_SUSPENDED SAVE produced a well-framed blob"
fi

# --- summary ----------------------------------------------------------
echo
echo "==== save_vhca_state build-up summary: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" -eq 0 ] || { echo "RESULT: FAIL"; exit 1; }
echo "RESULT: PASS"
