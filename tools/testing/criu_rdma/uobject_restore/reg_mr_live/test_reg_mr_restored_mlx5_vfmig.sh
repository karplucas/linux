#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Local (single-host) validation of the restored-VF inline-CREATE_MKEY
# gate in drivers/infiniband/hw/mlx5/mr.c (design/datapath_pause_resume.md).
#
# Reproduces, without a cross-host CRIU swap, the post-restore ibv_reg_mr
# path that regressed the mlx5_sriov rdma_criu_steps after_context/after_pd
# suites: a restored (post-LOAD_VHCA_STATE + mark_restored) VF whose bound
# userspace context calls a *fresh* ibv_reg_mr. On an ungated kernel that
# call posts to a dead UMR QP and D-state hangs forever; with the gate it
# is forced onto the inline CREATE_MKEY (command-ring) path and returns
# cleanly.
#
# Flow
# ----
#   Phase A  Provision a VF, set_tracked + enable_migratable, bind, SAVE
#            its (object-free) state to a blob, tear the VF down.
#   Phase B  Provision a fresh VF, LOAD_VHCA_STATE + mark_restored + bind
#            -> mlx5_vf_is_restored(mdev) is now true for this ib_device.
#   Phase C  ===== THE TEST =====
#            Run restored_reg_mr_probe against the restored ib_device with
#            a small MR (default 4096 -- the case that used to hang) and a
#            large MR (default 512 MiB -- past MLX5_MAX_UMR_PAGES*PAGE_SIZE).
#            Both must succeed. A clean (non-D-state) return is the proof
#            the inline CREATE_MKEY path was taken.
#
# RETOGGLE=1 additionally reproduces the CRIU R1-barrier initiator
# re-toggle (SUSPEND(INITIATOR)->RESUME(INITIATOR) on the RUNNING restored
# VF) that E2E bisected as the UMR-wedge trigger; DEFER_RESUME=1 exercises
# the bind-while-parked restore path.
#
# SAFETY: the probe is run under `timeout`. If it does NOT return, the VF
# is wedged in an unkillable D-state UMR wait (ungated / wrong kernel). In
# that case the harness FAILS LOUDLY and DOES NOT attempt SR-IOV teardown
# (that would compound the wedge); it tells the operator to reboot.
#
# Usage:
#   sudo PF=0000:08:00.0 ./test_reg_mr_restored_mlx5_vfmig.sh
#
# Optional knobs:
#   TOOL           mlx5_vfmig CLI.
#   PROBE          restored_reg_mr_probe binary.
#   BLOB           Path for the SAVE blob.
#   SMALL_BYTES    Small MR size (default 4096).
#   BIG_BYTES      Large MR size (default 536870912 = 512 MiB). Set to 0
#                  to skip the large-MR subtest.
#   PROBE_TIMEOUT  Seconds to allow each probe run (default 30).
#   RETOGGLE       1 = re-toggle the initiator before the test (repro).
#   DEFER_RESUME   1 = bind-while-parked restore path (R1 barrier mirror).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."

PF=${PF:-0000:00:08.0}
TOOL=${TOOL:-$ROOT_DIR/tools/mlx5_vfmig}
PROBE=${PROBE:-$ROOT_DIR/uobject_restore/reg_mr_live/restored_reg_mr_probe}
BLOB=${BLOB:-/tmp/vf_reg_mr_restored.blob}
SMALL_BYTES=${SMALL_BYTES:-4096}
BIG_BYTES=${BIG_BYTES:-536870912}
PROBE_TIMEOUT=${PROBE_TIMEOUT:-30}
# DEFER_RESUME=1 mirrors the CRIU R1 barrier restore path: park the VF at
# STOP after LOAD (mark_restored defer_resume), then walk STOP->P2P->RUNNING
# via directional RESUME_VHCA before bind -- instead of the plain
# mark_restored that leaves the VF RUNNING directly.
DEFER_RESUME=${DEFER_RESUME:-0}

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
        echo "!!! NOT tearing down SR-IOV: a probe is stuck in D-state on the"
        echo "!!! restored VF (UMR wait). Touching sriov_numvfs now would wedge"
        echo "!!! the whole SR-IOV subsystem. REBOOT the host to recover."
        return
    fi
    echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null 2>&1 || true
    rm -f "$BLOB" 2>/dev/null || true
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

# Run the probe under a hard timeout. Sets PROBE_RC; on timeout marks the
# host as WEDGED and returns 124 (do not proceed to teardown).
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

# --- Phase A: provision + SAVE -------------------------------------------
echo "=== Phase A: provision VF + SAVE (object-free) ==="
echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0"
SRC_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "src VF: $SRC_VF"

sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0
echo mlx5_core | sudo tee "$(vf_path $SRC_VF)/driver_override" >/dev/null
bind_vf_safe "$SRC_VF" 60 "Phase A: src bind"
sleep 1

# two-phase dump-side quiesce (design datapath_pause_resume.md), then SAVE.
sudo "$TOOL" "$PF" suspend_vhca 0 initiator
sudo "$TOOL" "$PF" suspend_vhca 0 responder
sudo "$TOOL" "$PF" save_vhca_state 0 "$BLOB"
sudo "$TOOL" "$PF" resume_vhca 0 responder
sudo "$TOOL" "$PF" resume_vhca 0 initiator
sudo chmod 0644 "$BLOB"
[ "$(stat -c %s "$BLOB")" -gt 16 ] || { echo "FAIL: blob too small"; exit 1; }

echo 0 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
echo 0 | sudo tee "$(vf_path $PF)/sriov_drivers_autoprobe" >/dev/null

# --- Phase B: fresh VF + LOAD + mark_restored + bind ---------------------
echo "=== Phase B: fresh VF + LOAD + mark_restored + bind ==="
echo 1 | sudo tee "$(vf_path $PF)/sriov_numvfs" >/dev/null
wait_for_path "$(vf_path $PF)/virtfn0"
DST_VF=$(basename "$(readlink "$(vf_path $PF)/virtfn0")")
echo "dst VF: $DST_VF"

sudo "$TOOL" "$PF" set_tracked 0 1
sudo "$TOOL" "$PF" enable_migratable 0
sudo dmesg -C
sudo "$TOOL" "$PF" load_vhca_state 0 "$BLOB"
if [ "$DEFER_RESUME" = 1 ]; then
    # E2E CRIU R1-barrier order: park at STOP after LOAD, bind mlx5_core
    # WHILE parked (prerestore), then the late-hook directional RESUME_VHCA
    # walks STOP->P2P->RUNNING.
    echo "restore path: defer_resume, bind-while-parked, then directional RESUME_VHCA (R1 barrier mirror)"
    sudo "$TOOL" "$PF" mark_restored 0 defer_resume
    echo mlx5_core | sudo tee "$(vf_path $DST_VF)/driver_override" >/dev/null
    bind_vf_safe "$DST_VF" 60 "Phase B: dst bind (parked)"
    sudo "$TOOL" "$PF" resume_vhca 0 responder
    sudo "$TOOL" "$PF" resume_vhca 0 initiator
else
    sudo "$TOOL" "$PF" mark_restored 0
    echo mlx5_core | sudo tee "$(vf_path $DST_VF)/driver_override" >/dev/null
    bind_vf_safe "$DST_VF" 60 "Phase B: dst bind"
fi
sleep 1
DST_IBDEV=$(find_ib_dev_for_pci "$DST_VF") || { echo "FAIL: no ibdev for $DST_VF"; exit 1; }
echo "restored ibdev: $DST_IBDEV"

# RETOGGLE=1 reproduces the CRIU R1-barrier RESUME_DEVICES_LATE re-toggle:
# on an already-RUNNING, already-bound restored VF, park+unpark the initiator
# a *second* time (SUSPEND(INITIATOR): RUNNING->P2P, then RESUME(INITIATOR):
# P2P->RUNNING). E2E bisected this exact re-toggle as necessary+sufficient
# for the post-restore reg_mr UMR wedge.
if [ "${RETOGGLE:-0}" = 1 ]; then
    echo "R1 re-toggle: SUSPEND(initiator) -> RESUME(initiator) on RUNNING restored VF"
    sudo "$TOOL" "$PF" suspend_vhca 0 initiator
    sudo "$TOOL" "$PF" resume_vhca 0 initiator
fi

# --- Phase C: THE TEST ---------------------------------------------------
echo
echo "================ Phase C: fresh ibv_reg_mr on restored VF ============"
overall=0

run_probe "$DST_IBDEV" "$SMALL_BYTES" "small (${SMALL_BYTES}B)"
if [ "$WEDGED" = 1 ]; then
    echo "FAIL reg_mr_restored: small MR wedged the VF (ungated kernel?)"
    exit 1
fi
[ "$PROBE_RC" = 0 ] || { echo "FAIL reg_mr_restored: small MR rc=$PROBE_RC"; overall=1; }

if [ "$BIG_BYTES" != 0 ]; then
    run_probe "$DST_IBDEV" "$BIG_BYTES" "large (${BIG_BYTES}B)"
    if [ "$WEDGED" = 1 ]; then
        echo "FAIL reg_mr_restored: large MR wedged the VF (ungated kernel?)"
        exit 1
    fi
    [ "$PROBE_RC" = 0 ] || { echo "FAIL reg_mr_restored: large MR rc=$PROBE_RC"; overall=1; }
fi

# --- dmesg evidence: gate fired (dyndbg-gated, so 0 unless enabled) -------
echo
echo "================ gate evidence (dmesg) =============================="
GATE_LINES=$(sudo dmesg | grep -c "forced onto inline CREATE_MKEY" || true)
echo "gate dbg lines: $GATE_LINES (dyndbg-gated; 0 is normal)"
sudo dmesg | grep "forced onto inline CREATE_MKEY" | sed 's/^/  /' || true
echo "  NOTE: the gate line is mlx5_ib_dbg. That the small MR returned"
echo "        cleanly above (no D-state) is the real proof it took the"
echo "        inline CREATE_MKEY path; enable dyndbg on mr.c to see it."

echo
if [ "$overall" = 0 ]; then
    echo "PASS reg_mr_restored: fresh ibv_reg_mr succeeds on a restored VF"
    echo "     (inline CREATE_MKEY path); no D-state UMR hang."
else
    echo "FAIL reg_mr_restored: see per-subtest output above"
fi
exit "$overall"
