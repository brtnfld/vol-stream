#!/bin/sh
# Copyright by The HDF Group.  All rights reserved.
# This file is part of vol-stream.  See the LICENSE file at the root of the
# source distribution, or https://www.hdfgroup.org/licenses.
#
# Runs both arms of the VFD SWMR proof of concept and reports the contrast.
# See README.md for what is being demonstrated.
#
#   usage: run_poc.sh [build-dir]

set -e

BIN="${1:-.}"
[ -x "$BIN/vs_swmr_writer" ] || BIN="$(dirname "$0")"
if [ ! -x "$BIN/vs_swmr_writer" ]; then
    echo "run_poc.sh: cannot find vs_swmr_writer; pass the build directory"
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

run_arm() {
    arm=$1
    label=$2

    echo
    echo "=============================================================="
    echo "  $label"
    echo "=============================================================="
    rm -f vfd_swmr_poc.h5 vfd_swmr_poc_shadow

    "$BIN/vs_swmr_writer" "$arm" > writer.log 2>&1 &
    wpid=$!
    "$BIN/vs_swmr_reader" "$arm" 2>&1 | sed 's/^/  /' || true
    wait $wpid 2>/dev/null || true

    echo "  --- writer said ---"
    tail -2 writer.log | sed 's/^/  /'
}

run_arm 0 "CONTROL: no VFD SWMR (expect: reader sees nothing)"
run_arm 1 "VFD SWMR enabled (expect: live growth + data verified)"

echo
echo "Contrast is the result: same writer, same reader, one flag."
