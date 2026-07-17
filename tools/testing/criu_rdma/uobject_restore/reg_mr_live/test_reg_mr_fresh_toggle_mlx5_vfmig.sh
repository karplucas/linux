#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Control for the restored-VF UMR wedge (scratch/kernel_answers_retoggle_umr_wedge.md,
# instrumentation ask 5.3).
#
# The wedge repro (test_reg_mr_restored_mlx5_vfmig.sh RETOGGLE=1) is: on a
# *restored* (post-LOAD_VHCA_STATE) VF, a second SUSPEND(INITIATOR)->
# RESUME(INITIATOR) toggle leaves the kernel UMR QP non-executing, so the
# next ibv_reg_mr D-state hangs. This control runs the *identical* toggle on
# a FRESH, never-restored VF and asserts reg_mr still succeeds -- isolating
# "restored" as the sole differentiator (per Q-park-depth / the en_tx.c
# restored-VF TX gate hypothesis).
#
# Flow (single host, no SAVE/LOAD, no restore):
#   1. Provision one VF, set_tracked + enable_migratable (so the migration
#      SUSPEND/RESUME_VHCA ioctls are permitted), bind mlx5_core.
#      -> mlx5_vf_is_restored(mdev) is FALSE for this ib_device.
#   2. Re-toggle the initiator BEFORE any reg_mr (so the UMR QP is created
#      lazily *after* the toggle, mirroring the restored timeline):
#         suspend_vhca 0 initiator   (RUNNING -> RUNNING_P2P)
#         resume_vhca  0 initiator   (RUNNING_P2P -> RUNNING)
#   3. ===== THE TEST ===== run restored_reg_mr_probe; on a fresh VF this
#      must SUCCEED (normal UMR path; the mr.c inline gate does not fire).
#
# Expected: PASS. A hang here would mean the toggle itself is broken
# independent of restore (not what E2E observed).
#
# SAFETY: probe runs under `timeout`; a non-returning probe marks WEDGED and
# skips SR-IOV teardown (reboot to recover), same contract as the restored
# harness.
#
# Usage:  sudo PF=0000:08:00.0 ./test_reg_mr_fresh_toggle_mlx5_vfmig.sh
#
# Optional knobs: TOOL, PROBE, SMALL_BYTES (default 4096),
#   BIG_BYTES (default 0 = skip), PROBE_TIMEOUT (default 30).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$ROOT_DIR/uobject_restore/reg_mr_live/restored_reg_mr_probe}
SMALL_BYTES=${SMALL_BYTES:-4096}
BIG_BYTES=${BIG_BYTES:-0}
PROBE_TIMEOUT=${PROBE_TIMEOUT:-30}

CDEV="/dev/mlx5_vfmig/$PF"
[ -x "$TOOL" ]  || { echo "build $TOOL first";  exit 1; }
[ -x "$PROBE" ] || { echo "build $PROBE first"; exit 1; }
[ -e "$CDEV" ]  || { echo "missing $CDEV (mlx5_core not loaded?)"; exit 1; }

vf_path()       { echo "/sys/bus/pci/devices/$1"; }
wait_for_path() { for i in $(seq 1 50); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }

WEDGED=0
cleanup() {
    if [ "$WEDGED" = 1 ]; then
        echo
        echo "!!! NOT tearing down SR-IOV: the probe is stuck in D-state on the"
        echo "!!! VF (UMR wait). Touching sriov_numvfs now would wedge the whole"
        echo "!!! SR-IOV subsystem. REBOOT the host to recover."
        return
    fi
    echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

bind_vf_safe() {
    local vf=$1 timeout=${2:-60} label=${3:-bind} started bg
    started=$(date +%s)
    ( echo "$vf" | sudo tee /sys/bus/pci/drivers/mlx5_core/bind ) >/dev/null 2>&1 &
    bg=$!
    while :; do
        if [ -e "$(vf_path $vf)/driver" ]; then
            wait "$bg" 2>/dev/null || true
            return 0
        fi
        if ! kill -0 "$bg" 2>/dev/null; then
            sleep 0.2
            [ -e "$(vf_path $vf)/driver" ] && return 0
            return 1
        fi
        if [ $(( $(date +%s) - started )) -ge "$timeout" ]; then
            echo "ERROR: $label pending ${timeout}s; probe wedged." >&2
            return 2
        fi
        sleep 0.5
    done
}

find_ib_dev_for_pci() {
    local bdf=$1 d pci
    for d in /sys/class/infiniband/*; do
        [ -e "$d/device" ] || continue
        pci=$(basename "$(readlink "$d/device")")
        [ "$pci" = "$bdf" ] && { basename "$d"; return 0; }
    done
    return 1
}

run_probe() {
    local ibdev=$1 bytes=$2 label=$3
    echo "=== probe ($label): $PROBE $ibdev $bytes ==="
    if sudo timeout --signal=KILL "$PROBE_TIMEOUT" "$PROBE" "$ibdev" "$bytes"; then
        PROBE_RC=0
    else
        PROBE_RC=$?
    fi
    if [ "$PROBE_RC" = 124 ] || [ "$PROBE_RC" = 137 ]; then
        echo "  probe ($label) did not return within ${PROBE_TIMEOUT}s"
        WEDGED=1
    fi
    return 0
}

# --- Phase A: provision a fresh, migration-enabled VF (no restore) --------
echo "=== Phase A: provision fresh VF (no SAVE/LOAD) ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0"
VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "fresh VF: $VF"

sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0
sudo dmesg -C
echo mlx5_core | sudo tee "$(vf_path $VF)/driver_override" >/dev/null
bind_vf_safe "$VF" 60 "Phase A: fresh bind"
sleep 1

# --- Phase B: the re-toggle on the fresh VF (before any reg_mr) -----------
echo "=== Phase B: SUSPEND(initiator) -> RESUME(initiator) on fresh RUNNING VF ==="
sudo "$TOOL" "$PF" suspend_vhca 0 initiator
sudo "$TOOL" "$PF" resume_vhca 0 initiator

IBDEV=$(find_ib_dev_for_pci "$VF") || { echo "FAIL: no ibdev for $VF"; exit 1; }
echo "fresh ibdev: $IBDEV"

# --- Phase C: THE TEST ---------------------------------------------------
echo
echo "================ Phase C: fresh ibv_reg_mr after re-toggle =========="
overall=0

run_probe "$IBDEV" "$SMALL_BYTES" "small (${SMALL_BYTES}B)"
if [ "$WEDGED" = 1 ]; then
    echo "FAIL fresh_toggle: small MR wedged a FRESH VF after re-toggle"
    echo "  (unexpected: implies the toggle itself breaks UMR, not 'restored')"
    exit 1
fi
[ "$PROBE_RC" = 0 ] || { echo "FAIL fresh_toggle: small MR rc=$PROBE_RC"; overall=1; }

if [ "$BIG_BYTES" != 0 ]; then
    run_probe "$IBDEV" "$BIG_BYTES" "large (${BIG_BYTES}B)"
    if [ "$WEDGED" = 1 ]; then
        echo "FAIL fresh_toggle: large MR wedged a FRESH VF after re-toggle"
        exit 1
    fi
    [ "$PROBE_RC" = 0 ] || { echo "FAIL fresh_toggle: large MR rc=$PROBE_RC"; overall=1; }
fi

echo
echo "================ gate evidence (dmesg) =============================="
GATE_LINES=$(sudo dmesg | grep -c "vfmig reg_mr on restored VF" || true)
echo "restored-gate warn lines: $GATE_LINES (expect 0 on a fresh VF)"

echo
if [ "$overall" = 0 ]; then
    echo "PASS fresh_toggle: SUSPEND(I)->RESUME(I) on a FRESH VF leaves UMR"
    echo "     functional -- reg_mr succeeds. 'restored' is the differentiator."
else
    echo "FAIL fresh_toggle: see per-subtest output above"
fi
exit "$overall"
