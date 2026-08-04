#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build-up test for MLX5_VFMIG_IOC_SUSPEND_VHCA / RESUME_VHCA
# (vfmig-rebase-0). SUSPEND/RESUME are PF-issued other_function commands,
# so the VF datapath is parked/un-parked without ever binding the VF to
# mlx5_core -- this harness only drives the PF cdev ioctls and reads back
# the kernel's info-level log lines.
#
# Datapath ladder: RUNNING(0) <-> RUNNING_P2P(1) <-> STOP(2).
#
# Subtests (the run fails if any fails):
#   1. Fused SUSPEND drives RUNNING->STOP (datapath 0->2) and returns 0.
#   2. SUSPEND is idempotent (second call: 0, no new log line).
#   3. Fused RESUME drives STOP->RUNNING (datapath 2->0) and returns 0.
#   4. RESUME is idempotent.
#   5. Directional ladder: suspend initiator (0->1), suspend responder
#      (1->2), resume responder (2->1), resume initiator (1->0).
#   6. Out-of-order directional steps are rejected (-EINVAL): responder
#      suspend from RUNNING, initiator resume from STOP.
#   7. SUSPEND on an out-of-range vf_id is rejected.
#   8. SUSPEND on a non-migratable VF is rejected (-EOPNOTSUPP).
#
# Usage: sudo PF=0000:08:00.0 ./test_suspend_resume_buildup.sh

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

# Run tool, capture rc + combined output into TOOL_RC / TOOL_OUT.
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

# --- subtest 1: fused SUSPEND RUNNING->STOP ---------------------------
echo "=== subtest 1: fused SUSPEND (datapath 0->2) ==="
sudo dmesg -C
run_tool suspend_vhca 0
[ "$TOOL_RC" -eq 0 ] && pass "SUSPEND returned 0" || fail "SUSPEND rc=$TOOL_RC"
dlog "vfmig: suspended vf 0 .*datapath 0->2" >/dev/null &&
	pass "logged datapath 0->2" || fail "missing 'suspended vf 0 ... datapath 0->2'"

# --- subtest 2: SUSPEND idempotent ------------------------------------
echo "=== subtest 2: SUSPEND idempotent ==="
sudo dmesg -C
run_tool suspend_vhca 0
[ "$TOOL_RC" -eq 0 ] && pass "second SUSPEND returned 0" || fail "second SUSPEND rc=$TOOL_RC"
if dlog "vfmig: suspended vf 0" >/dev/null; then
	fail "idempotent SUSPEND emitted a new pause line"
else
	pass "idempotent SUSPEND issued no firmware traffic"
fi

# --- subtest 3: fused RESUME STOP->RUNNING ----------------------------
echo "=== subtest 3: fused RESUME (datapath 2->0) ==="
sudo dmesg -C
run_tool resume_vhca 0
[ "$TOOL_RC" -eq 0 ] && pass "RESUME returned 0" || fail "RESUME rc=$TOOL_RC"
dlog "vfmig: resumed vf 0 .*datapath 2->0" >/dev/null &&
	pass "logged datapath 2->0" || fail "missing 'resumed vf 0 ... datapath 2->0'"

# --- subtest 4: RESUME idempotent -------------------------------------
echo "=== subtest 4: RESUME idempotent ==="
run_tool resume_vhca 0
[ "$TOOL_RC" -eq 0 ] && pass "second RESUME returned 0" || fail "second RESUME rc=$TOOL_RC"

# --- subtest 5: directional ladder ------------------------------------
echo "=== subtest 5: directional ladder 0->1->2->1->0 ==="
step() { # <verb> <dir> <from>-><to>
	local re
	sudo dmesg -C
	run_tool "$1" 0 "$2"
	[ "$1" = suspend_vhca ] && re="vfmig: suspended vf 0" || re="vfmig: resumed vf 0"
	if [ "$TOOL_RC" -eq 0 ] && dlog "$re .*datapath $3" >/dev/null; then
		pass "$1 $2 -> datapath $3"
	else
		fail "$1 $2 (rc=$TOOL_RC, expected datapath $3)"
	fi
}
step suspend_vhca initiator "0->1"
step suspend_vhca responder "1->2"
step resume_vhca  responder "2->1"
step resume_vhca  initiator "1->0"

# --- subtest 6: out-of-order directional rejected ---------------------
echo "=== subtest 6: out-of-order directional steps rejected ==="
run_tool resume_vhca 0                       # ensure RUNNING baseline
run_tool suspend_vhca 0 responder            # responder-from-RUNNING: invalid
[ "$TOOL_RC" -ne 0 ] && pass "responder suspend from RUNNING rejected" ||
	fail "responder suspend from RUNNING unexpectedly succeeded"
run_tool suspend_vhca 0                       # -> STOP
run_tool resume_vhca 0 initiator             # initiator-from-STOP: invalid
[ "$TOOL_RC" -ne 0 ] && pass "initiator resume from STOP rejected" ||
	fail "initiator resume from STOP unexpectedly succeeded"
run_tool resume_vhca 0                        # back to RUNNING

# --- subtest 7: out-of-range vf_id ------------------------------------
echo "=== subtest 7: SUSPEND out-of-range vf_id rejected ==="
run_tool suspend_vhca 99
[ "$TOOL_RC" -ne 0 ] && pass "out-of-range SUSPEND rejected" ||
	fail "out-of-range SUSPEND unexpectedly succeeded"

# --- subtest 8: non-migratable VF -> EOPNOTSUPP -----------------------
echo "=== subtest 8: SUSPEND on non-migratable vf1 rejected ==="
run_tool suspend_vhca 1
if [ "$TOOL_RC" -ne 0 ] &&
   echo "$TOOL_OUT" | grep -qiE "not supported|EOPNOTSUPP"; then
	pass "non-migratable SUSPEND rejected with EOPNOTSUPP"
else
	fail "non-migratable SUSPEND: rc=$TOOL_RC out='$TOOL_OUT'"
fi

# --- summary ----------------------------------------------------------
echo
echo "==== suspend/resume build-up summary: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" -eq 0 ] || { echo "RESULT: FAIL"; exit 1; }
echo "RESULT: PASS"
