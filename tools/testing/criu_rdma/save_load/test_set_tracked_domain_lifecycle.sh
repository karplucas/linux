#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# SET_TRACKED per-VF IOVA-domain lifecycle harness.
#
# Gates the domain-lifecycle surface that MLX5_VFMIG_IOC_SET_TRACKED
# lands on its own -- i.e. everything up to and including the per-VF
# unmanaged iommu_domain attach/detach, WITHOUT the deterministic-IOVA
# allocator or any SAVE/LOAD. This is the cheap gate to run on a kernel
# that has the tracking commit but not yet the DMA-routing work;
# test_iova_tracked_save_load.sh is the full end-to-end roundtrip and
# needs the allocator, so it is the wrong gate for the tracking commit
# in isolation.
#
# Subtests:
#
#   attach_detach        set_tracked 1 attaches a per-VF unmanaged
#                        domain: QUERY_VF reports tracked=1 and dmesg
#                        carries the "iova domain attached" breadcrumb
#                        with the aperture-fit window. set_tracked 0
#                        detaches: tracked=0, no iommu WARN (the VF
#                        still exists, so detach restores the default
#                        DMA domain cleanly).
#
#   idempotent_toggle    A set_tracked to the state the VF is already
#                        in is a silent no-op ("already N, no-op",
#                        rc=0, no iommu traffic), enable and disable.
#
#   multi_vf_windows     Two VFs tracked at once land on distinct,
#                        non-overlapping deterministic windows
#                        (vf N at VFMIG_IOVA_BASE + N*4GiB) and report
#                        tracked=1 independently -- catches a shared /
#                        off-by-one window bug.
#
#   reject_when_bound    set_tracked on a driver-bound VF is rejected
#                        -EBUSY (a bound driver's DMA mappings live in
#                        the domain we would replace).
#
#   clear_on_teardown    A tracked VF dropped via sriov_numvfs=0 tears
#                        its domain down with NO iommu WARN, and the
#                        tracked state is back to 0 after re-enable.
#                        This is the regression gate for detaching the
#                        unmanaged domain BEFORE pci_disable_sriov():
#                        get the ordering wrong and the iommu core
#                        WARNs at __iommu_group_free_device() when the
#                        VF's group is freed with our domain still in
#                        place of its default.
#
# This script drives sriov_numvfs + sriov_drivers_autoprobe (root-only,
# sysfs) and binds/unbinds mlx5_core on a VF. It assumes the PF is in
# the test-harness configuration where bouncing SR-IOV is safe (no live
# workload on the VFs). It backs up + restores sriov_numvfs and
# sriov_drivers_autoprobe, and unbinds any VF it bound, on exit.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_set_tracked_domain_lifecycle.sh [N]
#
#   PF   PF BDF, defaults to 0000:00:08.0.
#   N    VF count to provision, defaults to 2 (>=2 for multi_vf_windows).
#
# Exit codes:
#   0  all subtests PASSed
#   1  setup error (missing tool/cdev, sysfs not writable, no IOMMU)
#   2  at least one subtest FAILed; details on stderr

set -euo pipefail

PF=${PF:-0000:00:08.0}
N=${1:-2}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
CDEV="/dev/mlx5_vfmig/$PF"
PFDIR="/sys/bus/pci/devices/$PF"
SYSFS_NUMVFS="$PFDIR/sriov_numvfs"
SYSFS_AUTOPROBE="$PFDIR/sriov_drivers_autoprobe"
DRV=/sys/bus/pci/drivers/mlx5_core

# Deterministic window geometry (mirrors vfmig_iova.h): base + vf*4GiB.
IOVA_BASE=0x100000000
window_of() { printf '0x%x' $(( IOVA_BASE + $1 * (4 << 30) )); }

[ -x "$TOOL" ] || { echo "build $TOOL first (make -C tools/testing/criu_rdma)" >&2; exit 1; }
[ -e "$CDEV" ] || { echo "missing $CDEV (vfmig cdev not present)" >&2; exit 1; }
[ -w "$SYSFS_NUMVFS" ] || { echo "ERROR: $SYSFS_NUMVFS not writable; need root + a vfmig-eligible PF" >&2; exit 1; }
[ -e /sys/kernel/iommu_groups/0 ] || { echo "ERROR: no IOMMU groups; SET_TRACKED needs an IOMMU" >&2; exit 1; }
[ "$N" -ge 2 ] || { echo "ERROR: N must be >= 2 (multi_vf_windows)" >&2; exit 1; }

vfmig() { "$TOOL" "$PF" "$@"; }
tracked_of() { vfmig query_vf "$1" 2>/dev/null | sed -n 's/.*tracked=\([0-9]\).*/\1/p'; }

# Non-destructive dmesg delta: B=$(dcount); ...op...; dnew "$B"
dcount() { dmesg | wc -l; }
dnew()   { dmesg | tail -n +$(( $1 + 1 )); }
has_iommu_warn() { grep -qiE "WARNING|BUG:|Call Trace|iommu\.c:[0-9]"; }

read_numvfs()  { cat "$SYSFS_NUMVFS"; }
write_numvfs() { echo "$1" > "$SYSFS_NUMVFS"; }
ensure_numvfs() {
	local desired=$1 cur; cur=$(read_numvfs)
	[ "$cur" = "$desired" ] && return 0
	[ "$cur" != "0" ] && write_numvfs 0
	[ "$desired" != "0" ] && write_numvfs "$desired"
	sleep 1
}

vf_bdf() { basename "$(readlink -f "$PFDIR/virtfn$1")"; }

ORIG_NUMVFS=$(read_numvfs)
ORIG_AUTOPROBE=$(cat "$SYSFS_AUTOPROBE")
BOUND_VF=""
cleanup() {
	echo "[cleanup] restoring autoprobe=$ORIG_AUTOPROBE numvfs=$ORIG_NUMVFS"
	[ -n "$BOUND_VF" ] && [ -e "$DRV/$BOUND_VF" ] && { echo "$BOUND_VF" > "$DRV/unbind" 2>/dev/null || true; }
	[ -e "$PFDIR/virtfn0/driver_override" ] && { echo "" > "$PFDIR/virtfn0/driver_override" 2>/dev/null || true; }
	ensure_numvfs "$ORIG_NUMVFS" || true
	echo "$ORIG_AUTOPROBE" > "$SYSFS_AUTOPROBE" 2>/dev/null || true
}
trap cleanup EXIT

# Provision N unbound VFs.
echo 0 > "$SYSFS_AUTOPROBE"
ensure_numvfs 0
ensure_numvfs "$N"

PASS=0; FAIL=0
pass() { echo "PASS $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL $1: $2" >&2; FAIL=$((FAIL + 1)); }

#
# Test 1: attach_detach
#
echo "--- attach_detach ---"
B=$(dcount)
if vfmig set_tracked 0 1 >/dev/null && [ "$(tracked_of 0)" = "1" ]; then
	NEW=$(dnew "$B")
	if echo "$NEW" | grep -q "vf 0 tracked=1, iova domain attached" &&
	   echo "$NEW" | grep -Fq "domain attached, IOVA window [$(window_of 0), $(window_of 1))"; then
		B=$(dcount)
		if vfmig set_tracked 0 0 >/dev/null && [ "$(tracked_of 0)" = "0" ]; then
			if dnew "$B" | has_iommu_warn; then
				fail attach_detach "iommu WARN on detach"
			else
				pass attach_detach
			fi
		else
			fail attach_detach "detach did not clear tracked"
		fi
	else
		fail attach_detach "missing attach breadcrumb / window [$(window_of 0), $(window_of 1))"
	fi
else
	fail attach_detach "set_tracked 0 1 did not set tracked=1"
fi

#
# Test 2: idempotent_toggle
#
echo "--- idempotent_toggle ---"
ok=1
vfmig set_tracked 0 1 >/dev/null
B=$(dcount)
vfmig set_tracked 0 1 >/dev/null || ok=0     # already 1 -> no-op, rc=0
dnew "$B" | grep -qi "already 1, no-op" || { fail idempotent_toggle "enable no-op breadcrumb missing"; ok=0; }
vfmig set_tracked 0 0 >/dev/null
B=$(dcount)
vfmig set_tracked 0 0 >/dev/null || ok=0     # already 0 -> no-op, rc=0
dnew "$B" | grep -qi "already 0, no-op" || { fail idempotent_toggle "disable no-op breadcrumb missing"; ok=0; }
[ "$ok" = 1 ] && pass idempotent_toggle

#
# Test 3: multi_vf_windows
#
echo "--- multi_vf_windows ---"
B=$(dcount)
vfmig set_tracked 0 1 >/dev/null
vfmig set_tracked 1 1 >/dev/null
NEW=$(dnew "$B")
ok=1
[ "$(tracked_of 0)" = "1" ] || { fail multi_vf_windows "vf0 not tracked"; ok=0; }
[ "$(tracked_of 1)" = "1" ] || { fail multi_vf_windows "vf1 not tracked"; ok=0; }
echo "$NEW" | grep -Fq "vf 0 domain attached, IOVA window [$(window_of 0), $(window_of 1))" \
	|| { fail multi_vf_windows "vf0 window != [$(window_of 0), $(window_of 1))"; ok=0; }
echo "$NEW" | grep -Fq "vf 1 domain attached, IOVA window [$(window_of 1), $(window_of 2))" \
	|| { fail multi_vf_windows "vf1 window != [$(window_of 1), $(window_of 2))"; ok=0; }
[ "$ok" = 1 ] && pass multi_vf_windows
# leave both untracked for the next test
vfmig set_tracked 0 0 >/dev/null
vfmig set_tracked 1 0 >/dev/null

#
# Test 4: reject_when_bound
#
echo "--- reject_when_bound ---"
vf0=$(vf_bdf 0)
echo mlx5_core > "$PFDIR/virtfn0/driver_override" 2>/dev/null || true
echo "$vf0" > "$DRV/bind" 2>/dev/null || true
sleep 1
if [ -e "$DRV/$vf0" ]; then
	BOUND_VF="$vf0"
	set +e
	out=$(vfmig set_tracked 0 1 2>&1); rc=$?
	set -e
	if [ $rc -ne 0 ] && echo "$out" | grep -qiE "busy|bound"; then
		pass reject_when_bound
	else
		fail reject_when_bound "expected EBUSY on bound VF, rc=$rc out='$out'"
	fi
	echo "$vf0" > "$DRV/unbind" 2>/dev/null || true
	sleep 1
	BOUND_VF=""
else
	fail reject_when_bound "could not bind mlx5_core to $vf0 to test the guard"
fi
echo "" > "$PFDIR/virtfn0/driver_override" 2>/dev/null || true

#
# Test 5: clear_on_teardown  (regression gate for detach-before-pci_disable_sriov)
#
echo "--- clear_on_teardown ---"
vfmig set_tracked 1 1 >/dev/null
if [ "$(tracked_of 1)" = "1" ]; then
	B=$(dcount)
	ensure_numvfs 0
	NEW=$(dnew "$B")
	ensure_numvfs "$N"
	if echo "$NEW" | has_iommu_warn; then
		echo "$NEW" | grep -iE "WARNING|iommu\.c:[0-9]|Call Trace" >&2 || true
		fail clear_on_teardown "iommu WARN during SR-IOV teardown of a tracked VF"
	elif [ "$(tracked_of 1)" = "0" ]; then
		pass clear_on_teardown
	else
		fail clear_on_teardown "tracked state leaked across sriov cycle"
	fi
else
	fail clear_on_teardown "could not re-track vf1 for teardown test"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
