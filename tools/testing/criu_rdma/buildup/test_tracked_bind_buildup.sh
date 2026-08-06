#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build-up test for the deterministic-IOVA tracking milestone: a VF that
# has been SET_TRACKED (an unmanaged paging iommu_domain attached in place
# of its default DMA domain) can still have mlx5_core bound to it and
# complete probe, because every control-plane coherent DMA allocation
# (cmd ring, cmd mailboxes, FW pages, EQ/CQ, WQ rings, doorbells, and the
# DMA_COHERENT catch-all) is routed through the per-VF deterministic IOVA
# allocator.
#
# This is the functional counterpart of the earlier restore-probe test's
# "native-restore wall": there, a tracked domain did not yet exist and a
# restored VF's cmd ring was non-functional; here the allocator routing
# has landed, so a *fresh* (non-restored) tracked VF binds cleanly.
#
# Flow (single host):
#   1. provision one unbound VF (sriov_drivers_autoprobe=0).
#   2. SET_TRACKED enable=1; QUERY_VF must report tracked=1 and the kernel
#      must log the per-VF IOVA window attach at a deterministic base
#      (>= 4 GiB) that fits the IOMMU aperture.
#   3. bind mlx5_core (via driver_override): probe must COMPLETE with the
#      VF bound, the cmd ring live (firmware-version line), and no IOMMU
#      fault / allocator failure / kernel splat.
#   4. unbind; SET_TRACKED enable=0; QUERY_VF must report tracked=0.
#   5. no-regression: rebind the now-untracked VF and confirm it also
#      probes cleanly (the untracked path is unchanged).
#
# Only the control plane is exercised (the link is left down): RX/TX
# packet buffers use streaming DMA that is not routed through the
# allocator until the dma_ops milestone, so bringing the link up on a
# tracked VF would emit expected LOCAL_PROT_ERR datapath CQEs. That is
# out of scope here.
#
# Usage: sudo PF=0000:08:00.0 ./test_tracked_bind_buildup.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PF=${PF:-0000:08:00.0}
TOOL=${TOOL:-$SCRIPT_DIR/mlx5_vfmig_min}
CDEV=/dev/mlx5_vfmig/$PF
SYS=/sys/bus/pci/devices/$PF
DRV=/sys/bus/pci/drivers/mlx5_core
VF=0

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

# Bind mlx5_core to $VF_BDF; the VF was created with autoprobe=0 so a
# plain bind write returns -ENODEV without a driver_override match.
bind_vf() {
	echo mlx5_core | sudo tee "$SYS/../$VF_BDF/driver_override" >/dev/null
	echo "$VF_BDF" | sudo tee "$DRV/bind" >/dev/null 2>&1
	sleep 0.5
}

unbind_vf() {
	echo "$VF_BDF" | sudo tee "$DRV/unbind" >/dev/null 2>&1 || true
	echo "" | sudo tee "$SYS/../$VF_BDF/driver_override" >/dev/null 2>&1 || true
	sleep 0.2
}

# Common assertions on a probe log: the cmd ring came up (firmware
# version) and nothing faulted. @1 is a human label for the phase.
assert_clean_probe() {
	local label=$1 log=$2
	echo "$log" | grep -qE "mlx5_core $VF_BDF: firmware version" &&
		pass "$label: cmd ring live (firmware version read)" ||
		fail "$label: no firmware-version line (cmd ring never came up)"
	echo "$log" | grep -qiE "DMAR:|iommu.*fault|vfmig_iova.*alloc_slot.* -[0-9]|Error cqe" &&
		fail "$label: IOMMU fault / allocator failure in probe log" ||
		pass "$label: no IOMMU fault / allocator failure"
	echo "$log" | grep -qE "WARNING:|BUG:|Call Trace:|kernel BUG|general protection|Oops" &&
		fail "$label: kernel splat during probe" ||
		pass "$label: no kernel splat"
}

cleanup() {
	if [ -n "$VF_BDF" ]; then
		unbind_vf
	fi
	sudo "$TOOL" "$PF" untrack "$VF" >/dev/null 2>&1 || true
	echo 1 | sudo tee "$SYS/sriov_drivers_autoprobe" >/dev/null 2>&1 || true
	echo 0 | sudo tee "$SYS/sriov_numvfs"            >/dev/null 2>&1 || true
}
trap cleanup EXIT

# --- provision one unbound VF ----------------------------------------
echo "=== provision 1 unbound VF on $PF ==="
echo 0 | sudo tee "$SYS/sriov_numvfs"            >/dev/null
echo 0 | sudo tee "$SYS/sriov_drivers_autoprobe" >/dev/null
echo 1 | sudo tee "$SYS/sriov_numvfs"            >/dev/null
wait_for_path "$SYS/virtfn0" || { echo "FAIL: VF did not appear"; exit 1; }
VF_BDF=$(basename "$(readlink -f "$SYS/virtfn0")")
[ -e "$SYS/../$VF_BDF/driver" ] && { echo "FAIL: VF $VF_BDF unexpectedly bound"; exit 1; }
pass "VF $VF_BDF provisioned and unbound"

# --- track the VF: attach the per-VF deterministic IOVA domain -------
echo "=== SET_TRACKED enable=1 on vf$VF ==="
sudo dmesg -C
run_tool track "$VF"
[ "$TOOL_RC" -eq 0 ] && pass "SET_TRACKED enable=1 accepted" ||
	fail "SET_TRACKED enable=1 failed rc=$TOOL_RC"

run_tool query_vf "$VF"
echo "$TOOL_OUT" | grep -qE "tracked 1" &&
	pass "QUERY_VF reports tracked=1" ||
	fail "QUERY_VF did not report tracked=1"

ATTACH_LOG=$(sudo dmesg)
# Deterministic window base >= 4 GiB (0x1_0000_0000), fitting the aperture.
echo "$ATTACH_LOG" | grep -qE "vfmig_iova: vf $VF domain attached, IOVA window \[0x1[0-9a-f]+,.*within IOMMU aperture" &&
	pass "kernel logged deterministic IOVA window attach (>= 4 GiB)" ||
	fail "missing 'domain attached, IOVA window [0x1... within IOMMU aperture' line"

# --- bind mlx5_core: probe must COMPLETE on the tracked VF -----------
echo "=== bind $VF_BDF (tracked) -> probe routes DMA through allocator ==="
sudo dmesg -C
bind_vf
TRACKED_PROBE_LOG=$(sudo dmesg)

if [ -e "$DRV/$VF_BDF" ]; then
	pass "tracked VF $VF_BDF bound (probe completed)"
else
	fail "tracked VF $VF_BDF did not bind (probe did not complete)"
fi
assert_clean_probe "tracked bind" "$TRACKED_PROBE_LOG"

# --- untrack and verify state ----------------------------------------
echo "=== unbind + SET_TRACKED enable=0 on vf$VF ==="
unbind_vf
run_tool untrack "$VF"
[ "$TOOL_RC" -eq 0 ] && pass "SET_TRACKED enable=0 accepted" ||
	fail "SET_TRACKED enable=0 failed rc=$TOOL_RC"
run_tool query_vf "$VF"
echo "$TOOL_OUT" | grep -qE "tracked 0" &&
	pass "QUERY_VF reports tracked=0 after untrack" ||
	fail "QUERY_VF still reports tracked=1 after untrack"

# --- no-regression: the untracked VF still probes cleanly ------------
echo "=== bind $VF_BDF (untracked) -> unchanged legacy DMA path ==="
sudo dmesg -C
bind_vf
UNTRACKED_PROBE_LOG=$(sudo dmesg)
if [ -e "$DRV/$VF_BDF" ]; then
	pass "untracked VF $VF_BDF bound (no regression)"
else
	fail "untracked VF $VF_BDF did not bind (regression!)"
fi
assert_clean_probe "untracked bind" "$UNTRACKED_PROBE_LOG"
unbind_vf

# --- summary ----------------------------------------------------------
echo
echo "==== tracked_bind build-up summary: PASS=$PASS FAIL=$FAIL ===="
[ "$FAIL" -eq 0 ] || { echo "RESULT: FAIL"; exit 1; }
echo "RESULT: PASS"
