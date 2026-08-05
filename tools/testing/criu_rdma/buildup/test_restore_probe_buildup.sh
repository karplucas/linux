#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build-up test for the vfmig restore *probe* path (vfmig-rebase-0,
# staged-LOAD milestone). Where the SAVE/LOAD harnesses only drive PF
# cdev ioctls against an unbound VF, this one exercises the piece that
# makes LOAD_VHCA_STATE actually restore a VF: the deferred apply that
# runs from the VF's own mlx5_core probe.
#
# Flow (single host, self-restore):
#   1. provision an unbound, migratable vf0.
#   2. SUSPEND vf0 -> STOP and SAVE its (pristine) VHCA to a blob.
#   3. LOAD the blob back: closing the fd *stages* it into the VF's
#      pending_load slot and latches "restored" (no FW command yet).
#   4. bind vf0 to mlx5_core. Its probe consumes the latch, applies the
#      staged blob (SUSPEND -> LOAD_VHCA_STATE -> RESUME on the PF mdev)
#      and skips ENABLE_HCA(self)/SET_ISSI/boot-pages/INIT_HCA.
#
# Gate (Tier-1 plumbing): the probe dmesg must show the staged blob being
# applied and INIT_HCA being skipped, with no kernel splat. The bind
# itself is EXPECTED to fail afterwards: on a native (non-VFIO, non-VM)
# probe without deterministic IOVAs the restored VHCA's command ring is
# left non-functional, so mlx5_function_open()'s QUERY_HCA_CAP fails.
# That is the documented native-restore wall; making a restored VF fully
# usable is the deterministic-IOVA milestone. We report the bind result
# but do not fail the run on the expected failure.
#
# Usage: sudo PF=0000:08:00.0 ./test_restore_probe_buildup.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PF=${PF:-0000:08:00.0}
TOOL=${TOOL:-$SCRIPT_DIR/mlx5_vfmig_min}
CDEV=/dev/mlx5_vfmig/$PF
SYS=/sys/bus/pci/devices/$PF
DRV=/sys/bus/pci/drivers/mlx5_core
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

wait_for_path() { for _ in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }

VF_BDF=""

cleanup() {
	rm -f "$BLOB"
	# Make sure the VF is not left bound, drop the driver_override, then
	# tear the VFs down so no stale pending_load slot survives the run.
	if [ -n "$VF_BDF" ]; then
		echo "$VF_BDF" | sudo tee "$DRV/unbind" >/dev/null 2>&1 || true
		echo "" | sudo tee "$SYS/../$VF_BDF/driver_override" >/dev/null 2>&1 || true
	fi
	echo 1 | sudo tee "$SYS/sriov_drivers_autoprobe" >/dev/null 2>&1 || true
	echo 0 | sudo tee "$SYS/sriov_numvfs"            >/dev/null 2>&1 || true
}
trap cleanup EXIT

# --- provision unbound VFs and pick a clean (unconsumed) one ----------
# A VHCA that has already been through a restore probe once degenerates:
# its SAVE collapses to a tiny record and its migration FSM starts
# rejecting SUSPEND/RESUME, and that survives an sriov_numvfs cycle (only
# an mlx5 driver reload / reboot resets it). Plain SAVE does NOT consume a
# VHCA, so provision several VFs and self-suspend-SAVE each until one
# yields a full-sized blob -- that is a fresh VHCA to restore into. If all
# candidates are degenerate, reboot (or reload mlx5_core) and re-run.
NUMVFS=${NUMVFS:-4}
MIN_BLOB=${MIN_BLOB:-65536}
VF=""

echo "=== provision $NUMVFS unbound VFs on $PF, find a fresh VHCA ==="
echo 0 | sudo tee "$SYS/sriov_numvfs"            >/dev/null
echo 0 | sudo tee "$SYS/sriov_drivers_autoprobe" >/dev/null
echo "$NUMVFS" | sudo tee "$SYS/sriov_numvfs"    >/dev/null
wait_for_path "$SYS/virtfn$((NUMVFS - 1))" || { echo "FAIL: VFs did not appear"; exit 1; }

for cand in $(seq 0 $((NUMVFS - 1))); do
	run_tool enable_migratable "$cand"
	if [ "$TOOL_RC" -ne 0 ]; then
		echo "$TOOL_OUT" | grep -qiE "not supported|EOPNOTSUPP|Operation not supported" &&
			{ echo "SKIP: PF firmware lacks migration caps"; exit 0; }
		continue
	fi
	# Self-suspend SAVE: leaves the VHCA at RUNNING (the destination
	# baseline the restore probe's RUNNING->STOP->LOAD->RUNNING arc
	# expects) and does not consume it.
	run_tool save_vhca_state "$cand" "$BLOB"
	sz=$( [ -f "$BLOB" ] && stat -c%s "$BLOB" || echo 0 )
	if [ "$TOOL_RC" -eq 0 ] && [ "$sz" -ge "$MIN_BLOB" ]; then
		VF=$cand
		VF_BDF=$(basename "$(readlink -f "$SYS/virtfn$cand")")
		echo "    picked vf$VF ($VF_BDF): fresh VHCA, blob $sz bytes"
		break
	fi
	echo "    vf$cand: degenerate/consumed (blob $sz bytes, rc=$TOOL_RC), skipping"
done

if [ -z "$VF" ]; then
	echo "SKIP: no fresh VHCA available (all consumed). Reboot / reload"
	echo "      mlx5_core and re-run to reset firmware migration state."
	exit 0
fi

# --- STAGE the blob via a LOAD session -------------------------------
echo "=== stage LOAD blob for vf$VF (fd close latches restored) ==="
sudo dmesg -C
run_tool load_vhca_state "$VF" "$BLOB"
[ "$TOOL_RC" -eq 0 ] && pass "LOAD session accepted + staged the blob" ||
	fail "LOAD staging failed rc=$TOOL_RC"
sudo dmesg | grep -qE "vfmig: staged .* for vf $VF .* next probe will apply" &&
	pass "kernel logged 'staged ... next probe will apply'" ||
	fail "missing kernel 'staged ... will apply' line"
# QUERY_VF must now report the VF as restored (latch set by LOAD close).
run_tool query_vf "$VF"
echo "$TOOL_OUT" | grep -qiE "restored[ =:]+1|restored: 1|restored=1" &&
	pass "QUERY_VF reports vf$VF restored=1" ||
	echo "  NOTE: QUERY_VF restored flag not surfaced by tool (non-fatal)"

# --- bind the VF: probe applies the staged LOAD ----------------------
# The VF was created with sriov_drivers_autoprobe=0, so a plain
# drivers/mlx5_core/bind write returns -ENODEV; set driver_override to
# force the match. The bind write itself is expected to return an error
# because the probe fails at the native-restore wall (below) -- that is
# reported, not asserted.
echo "=== bind $VF_BDF -> probe applies staged LOAD, skips INIT_HCA ==="
echo mlx5_core | sudo tee "$SYS/../$VF_BDF/driver_override" >/dev/null
sudo dmesg -C
echo "$VF_BDF" | sudo tee "$DRV/bind" >/dev/null 2>&1
sleep 0.5
PROBE_LOG=$(sudo dmesg)

echo "$PROBE_LOG" | grep -qE "vfmig: applied .* bytes of LOAD state to vf $VF .* resumed" &&
	pass "probe applied LOAD_VHCA_STATE ('applied ... resumed')" ||
	fail "probe did not log 'applied ... resumed' (LOAD not applied at probe)"

echo "$PROBE_LOG" | grep -qE "vfmig: VF .* restored; .*INIT_HCA skipped" &&
	pass "probe skipped ENABLE_HCA/SET_ISSI/boot-pages/INIT_HCA" ||
	fail "probe did not log the INIT_HCA-skip line"

# The "applied ... resumed" line above only prints when the whole apply
# arc (SUSPEND -> LOAD_VHCA_STATE -> RESUME) returned 0, so there is no
# separate apply-phase syndrome check here: the expected QUERY_HCA_CAP
# failure at the native-restore wall legitimately emits its own syndrome.

# No kernel splat (WARN/BUG/oops/lockdep) around the restored probe.
echo "$PROBE_LOG" | grep -qE "WARNING:|BUG:|Call Trace:|kernel BUG|general protection|Oops" &&
	fail "kernel splat observed during restored probe" ||
	pass "no kernel splat during restored probe"

# --- report (do not fail on) the expected native-restore-wall result --
echo "=== bind result (expected to fail on native probe) ==="
if [ -e "$DRV/$VF_BDF" ]; then
	echo "  NOTE: $VF_BDF bound successfully (deterministic-IOVA not needed?)"
else
	echo "  NOTE: $VF_BDF bind did not complete -- expected native-restore wall"
	echo "$PROBE_LOG" | grep -qiE "query hca failed" &&
		echo "  NOTE: failure surfaced at QUERY_HCA_CAP, as documented"
fi

# --- summary ----------------------------------------------------------
echo
echo "==== restore_probe build-up summary: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" -eq 0 ] || { echo "RESULT: FAIL"; exit 1; }
echo "RESULT: PASS"
