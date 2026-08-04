#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build-up smoke for the vfmig-rebase-0 char device + introspection
# ioctls (GET_VHCA_ID / QUERY_VF / MARK_RESTORED). This is the trimmed,
# always-green ancestor of save_load/test_pf_cdev_smoke.sh: it covers
# only the surface the build-up branch actually implements. Steps 4-5 of
# the full smoke (bind -> "skipping INIT_HCA" -> "bad system state 0x4")
# arrive once the restore-probe milestone lands and are intentionally
# absent here.
#
# Acceptance criteria:
#   1. /dev/mlx5_vfmig/<PF> exists after mlx5_core probes the PF.
#   2. QUERY_VF enumerates the PF's VFs.
#   3. GET_VHCA_ID resolves an unbound VF's vhca_id.
#   4. MARK_RESTORED succeeds; a second MARK_RESTORED returns EALREADY.
#   5. QUERY_VF then reports restored=1 for that VF.
#
# Usage: sudo PF=0000:08:00.0 ./test_pf_cdev_smoke_buildup.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PF=${PF:-0000:08:00.0}
TOOL=${TOOL:-$SCRIPT_DIR/mlx5_vfmig_min}
CDEV=/dev/mlx5_vfmig/$PF
SYS=/sys/bus/pci/devices/$PF

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$TOOL" ] || fail "build $TOOL first: make -C $SCRIPT_DIR/.. buildup/mlx5_vfmig_min"

# 1. the PF cdev must exist (CONFIG_MLX5_VFMIG kernel loaded)
[ -e "$CDEV" ] || fail "cdev $CDEV missing (is a build-up-branch kernel loaded?)"
pass "cdev $CDEV present"

# clean slate, then close the autoprobe window so the VF stays unbound
echo 0 | sudo tee "$SYS/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$SYS/sriov_drivers_autoprobe" >/dev/null

cleanup() {
	echo 1 | sudo tee "$SYS/sriov_drivers_autoprobe" >/dev/null 2>&1 || true
	echo 0 | sudo tee "$SYS/sriov_numvfs" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# spawn a single VF
echo 1 | sudo tee "$SYS/sriov_numvfs" >/dev/null
for _ in $(seq 1 20); do
	[ -e "$SYS/virtfn0" ] && break
	sleep 0.1
done
[ -e "$SYS/virtfn0" ] || fail "VF did not appear"
VF=$(basename "$(readlink "$SYS/virtfn0")")
[ ! -e "$SYS/../$VF/driver" ] && pass "VF $VF is unbound"

# 2. QUERY_VF enumeration
sudo "$TOOL" "$PF" list
pass "QUERY_VF list ran"

# 3. GET_VHCA_ID on the unbound VF
sudo "$TOOL" "$PF" get_vhca_id 0
pass "GET_VHCA_ID resolved vf 0"

# 4. MARK_RESTORED, then EALREADY on the second call
sudo "$TOOL" "$PF" mark_restored 0
pass "MARK_RESTORED vf 0"
if sudo "$TOOL" "$PF" mark_restored 0; then
	fail "second MARK_RESTORED should have failed with EALREADY"
fi
pass "second MARK_RESTORED returned an error (expected EALREADY)"

# 5. QUERY_VF must now show restored=1
out=$(sudo "$TOOL" "$PF" query_vf 0)
echo "$out"
echo "$out" | grep -q "restored 1" || fail "QUERY_VF did not report restored=1"
pass "QUERY_VF reports restored=1"

echo "ALL PASS"
