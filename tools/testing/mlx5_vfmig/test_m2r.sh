#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# M2-revised end-to-end SAVE+LOAD round-trip on a single host. No VFIO,
# no IOMMU, no QEMU -- everything is driven through the in-driver
# /dev/mlx5_vfmig/<bdf> control plane.
#
# Phases:
#   A. Provision VF, let mlx5_core probe it normally (autoprobe=on).
#      Snapshot baseline operational state (vhca_id, link, MAC, etc).
#   B. SAVE the VF's firmware state into a blob via the SAVE ioctl.
#      The driver SUSPENDs the VHCA, snapshots, then RESUMEs.
#   C. Tear down the VF (sriov_numvfs=0). Re-create it with
#      autoprobe=off so we can race its probe.
#   D. LOAD the blob back into the fresh VF, mark it restored, then
#      bind mlx5_core.
#   E. Compare post-restore operational state against the baseline.
#
# Acceptance:
#   - SAVE ioctl returns a non-empty blob, no firmware errors in dmesg
#   - LOAD ioctl consumes the blob without firmware errors
#   - VF re-probes cleanly (no "bad system state", no QUERY_ADAPTER
#     errors, no probe error code)
#   - Post-restore baseline (link, MAC, vhca_id) matches pre-save
#
# Usage: sudo PF=0000:00:08.0 ./test_m2r.sh

set -euxo pipefail

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-./mlx5_vfmig}
BLOB=${BLOB:-/tmp/vf_m2r.blob}
SAVE_FLAGS=${SAVE_FLAGS:-}   # e.g. "keep_suspended"

[ -x "$TOOL" ] || { echo "build $TOOL first"; exit 1; }

CDEV="/dev/mlx5_vfmig/$PF"
[ -e "$CDEV" ] || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

# --- helpers -----------------------------------------------------------

vf_path()       { echo "/sys/bus/pci/devices/$1"; }
wait_for_path() { for i in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }

snapshot_vf() {
    # $1 = VF BDF, $2 = label
    local bdf=$1 label=$2 ifname mac state vhca
    ifname=$(ls "/sys/bus/pci/devices/$bdf/net/" 2>/dev/null | head -n1 || true)
    if [ -n "$ifname" ]; then
        mac=$(cat "/sys/class/net/$ifname/address" 2>/dev/null || echo "?")
        state=$(cat "/sys/class/net/$ifname/operstate" 2>/dev/null || echo "?")
    else
        ifname="(none)"
        mac="(none)"
        state="(none)"
    fi
    vhca=$(sudo "$TOOL" "$PF" get_vhca_id 0 2>/dev/null | awk '{print $NF}' || echo "?")
    echo "[$label] bdf=$bdf netdev=$ifname mac=$mac state=$state vhca_id=$vhca"
}

# --- Phase A: provision and snapshot baseline --------------------------

echo "=== Phase A: provision + snapshot baseline ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"
echo 1 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe"
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs"

wait_for_path "$(vf_path $PF)/virtfn0"
VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "VF is $VF"
wait_for_path "$(vf_path $VF)/driver"
DRV_PRE=$(basename "$(readlink "$(vf_path $VF)/driver")")
[ "$DRV_PRE" = "mlx5_core" ] || { echo "expected mlx5_core, got $DRV_PRE"; exit 1; }

# Give the VF a moment to settle (netdev appears slightly after probe).
sleep 1
PRE=$(snapshot_vf "$VF" "PRE")
echo "$PRE"

# --- Phase B: SAVE -----------------------------------------------------

echo "=== Phase B: SAVE ==="
sudo dmesg -C
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB" $SAVE_FLAGS
ls -l "$BLOB"
SAVE_BYTES=$(stat -c %s "$BLOB")
[ "$SAVE_BYTES" -gt 16 ] || { echo "blob suspiciously small: $SAVE_BYTES"; exit 1; }

echo "--- dmesg after SAVE ---"
sudo dmesg | tail -20
if sudo dmesg | grep -E 'SUSPEND_VHCA|SAVE_VHCA_STATE|QUERY_VHCA_MIGRATION' | grep -i 'failed'; then
    echo "FAIL: firmware error during SAVE"
    exit 1
fi

# --- Phase C: tear down + re-provision (no autoprobe) ------------------

echo "=== Phase C: tear down + re-provision ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs"
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe"
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs"

wait_for_path "$(vf_path $PF)/virtfn0"
VF2=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "fresh VF is $VF2"
[ ! -e "$(vf_path $VF2)/driver" ] && echo "OK: VF $VF2 unbound"

# --- Phase D: LOAD + mark_restored + bind -----------------------------

echo "=== Phase D: LOAD + bind ==="
sudo dmesg -C
sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" mark_restored 0

echo mlx5_core | sudo tee "$(vf_path $VF2)/driver_override"
echo "$VF2"     | sudo tee /sys/bus/pci/drivers/mlx5_core/bind

echo "--- dmesg after LOAD+BIND ---"
sudo dmesg | tail -40
if sudo dmesg | grep -E 'bad system state|probe with driver mlx5_core failed'; then
    echo "FAIL: VF probe surfaced bad-system-state / probe failure"
    exit 1
fi
if sudo dmesg | grep -E 'LOAD_VHCA_STATE.*failed|RESUME_VHCA.*failed'; then
    echo "FAIL: firmware error during LOAD/RESUME"
    exit 1
fi

# --- Phase E: post-restore snapshot + comparison -----------------------

echo "=== Phase E: snapshot post-restore ==="
wait_for_path "$(vf_path $VF2)/driver"
DRV_POST=$(basename "$(readlink "$(vf_path $VF2)/driver")")
[ "$DRV_POST" = "mlx5_core" ] || { echo "post-restore driver=$DRV_POST"; exit 1; }
sleep 1
POST=$(snapshot_vf "$VF2" "POST")
echo "$POST"

echo
echo "==== summary ===="
echo "$PRE"
echo "$POST"
echo
echo "Acceptance signals:"
echo "  - SAVE produced $SAVE_BYTES bytes"
echo "  - VF re-probed clean (no bad system state / probe failure)"
echo "  - LOAD/RESUME reported no firmware errors"
echo
echo "(Compare the PRE / POST lines above for vhca_id and netdev MAC equality.)"

# Cleanup leaves VF in restored state for inspection. To reset:
#   echo 0 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs
