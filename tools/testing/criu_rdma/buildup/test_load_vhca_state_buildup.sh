#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build-up test for the MLX5_VFMIG_IOC_LOAD_VHCA_STATE ioctl surface
# (vfmig-rebase-0, staged-LOAD milestone). LOAD is the inverse of SAVE: a
# write-only anon-inode fd that ingests the SAVE blob. As of the
# staged-LOAD milestone the ioctl no longer runs LOAD_VHCA_STATE eagerly
# and no longer requires the VHCA at STOP -- closing the fd after a full
# blob only *stages* the payload into the VF's pending_load slot, and the
# VF's next mlx5_core probe applies it. The end-to-end stage+apply path is
# covered by test_restore_probe_buildup.sh; this harness validates just
# the ioctl surface (open, claim/exclusion, argument checks) and so
# deliberately never writes a complete blob (which would leave a staged
# slot behind).
#
# Subtests (the run fails if any fails):
#   1. LOAD opens on a RUNNING (not-suspended) migratable vf0 -> success.
#   2. A second concurrent LOAD session for the same vf_id -> -EBUSY.
#   3. SAVE and LOAD are mutually exclusive per vf_id (-EBUSY).
#   4. LOAD on an out-of-range vf_id is rejected.
#   5. LOAD on the non-migratable vf1 is rejected (-EOPNOTSUPP).
#
# Usage: sudo PF=0000:08:00.0 ./test_load_vhca_state_buildup.sh

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

# --- subtest 1: LOAD opens without requiring STOP ---------------------
echo "=== subtest 1: LOAD opens on RUNNING migratable vf0 (staged model) ==="
run_tool resume_vhca 0			# ensure a RUNNING baseline
run_tool load_vhca_state 0 /dev/null	# empty blob: opens, stages nothing
[ "$TOOL_RC" -eq 0 ] && pass "LOAD session opened without SUSPEND" ||
	fail "LOAD open on RUNNING vf0 failed rc=$TOOL_RC"

# --- subtest 2: concurrent LOAD session -> EBUSY ----------------------
echo "=== subtest 2: second concurrent LOAD on vf0 -> EBUSY ==="
run_tool load_busy 0
[ "$TOOL_RC" -eq 0 ] && pass "concurrent LOAD rejected with EBUSY" ||
	fail "concurrent LOAD not rejected with EBUSY (rc=$TOOL_RC)"

# --- subtest 3: SAVE vs LOAD mutual exclusion -------------------------
echo "=== subtest 3: SAVE excluded while a LOAD session is open -> EBUSY ==="
run_tool save_excl 0
[ "$TOOL_RC" -eq 0 ] && pass "SAVE during open LOAD rejected with EBUSY" ||
	fail "SAVE/LOAD not mutually exclusive (rc=$TOOL_RC)"

# --- subtest 4: out-of-range vf_id ------------------------------------
echo "=== subtest 4: LOAD out-of-range vf_id rejected ==="
run_tool load_vhca_state 99 /dev/null
[ "$TOOL_RC" -ne 0 ] && pass "out-of-range LOAD rejected" ||
	fail "out-of-range LOAD unexpectedly succeeded"

# --- subtest 5: non-migratable vf1 ------------------------------------
echo "=== subtest 5: LOAD on vf1 (non-migratable) rejected ==="
run_tool load_vhca_state 1 /dev/null
[ "$TOOL_RC" -ne 0 ] && pass "LOAD on non-migratable vf1 rejected" ||
	fail "LOAD on vf1 unexpectedly succeeded"

# --- summary ----------------------------------------------------------
echo
echo "==== load_vhca_state build-up summary: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" -eq 0 ] || { echo "RESULT: FAIL"; exit 1; }
echo "RESULT: PASS"
