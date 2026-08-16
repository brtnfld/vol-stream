#!/bin/sh
# Copyright by The HDF Group.  All rights reserved.
# This file is part of vol-stream.  See the LICENSE file at the root of the
# source distribution, or https://www.hdfgroup.org/licenses.
#
# Runs the precision path through HDF5_VOL_CONNECTOR rather than explicit
# registration -- the one configuration no other test covers.
#
# Why that configuration matters. H5VL__stream_refilter_for_subscriber() and
# H5VL__stream_unfilter_pushed_data() build a throwaway in-memory dataset to
# run a filter pipeline. H5Pset_fapl_core() sets the VFD, not the VOL, so a
# fresh H5P_FILE_ACCESS carries whatever HDF5 installed as the process default
# -- which is vol-stream itself under HDF5_VOL_CONNECTOR, the plugin usage
# README.md documents. Those scratch files then recursed straight back into the
# connector from inside a replay already in progress. Confirmed by direct
# instrumentation: a scratch file reached H5VL_stream_file_create() and
# triggered a vs_tr_start() attempt of its own.
#
# Every C test registers the connector explicitly (H5VL_stream_register() +
# H5Pset_vol()), which leaves the process default native and hides this
# entirely. Hence a separate run rather than another case inside an existing
# test.
#
# What this asserts, precisely -- and what it does not. The recursion is not
# directly observable from outside the process: with na+sm the recursive
# vs_tr_start() fails (a second margo instance on the same NA plugin does not
# come up), so no second transport and no stray sidecar result, and the run
# still passes. What is observable, and what is checked here, is that no
# scratch file escapes to disk and that the run is clean under the plugin
# loader. That catches the escape case directly -- which is what a transport
# whose second margo_init() DOES succeed would produce -- and catches the
# fixed-scratch-filename collision. It does not, on its own, prove the scratch
# FAPL is pinned to native; H5VL__stream_scratch_fapl()'s H5Pset_vol() call is
# what does that, and this run exercises the path that depends on it.

set -e

BIN="${1:?usage: $0 <path to t_precision> <plugin dir>}"
PLUGIN_DIR="${2:?usage: $0 <path to t_precision> <plugin dir>}"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

HDF5_PLUGIN_PATH="$PLUGIN_DIR" \
HDF5_VOL_CONNECTOR="vol-stream" \
VOL_STREAM_NA="${VOL_STREAM_NA:-na+sm}" \
VOL_STREAM_DEBUG_REFILTER=1 \
    "$BIN" > out.txt 2>&1 || {
        echo "FAIL: t_precision did not succeed under HDF5_VOL_CONNECTOR=vol-stream"
        cat out.txt
        exit 1
    }

# The guard is only meaningful if the filter path actually ran, so that a
# future change quietly skipping it cannot make this pass vacuously.
if ! grep -q "refilter" out.txt; then
    echo "FAIL: no re-filtering happened, so this run proves nothing"
    cat out.txt
    exit 1
fi

leaked=$(ls -1 | grep -E 'vol_stream_(re|un)filter_tmp' || true)
if [ -n "$leaked" ]; then
    echo "FAIL: connector scratch files escaped to disk. Either the scratch FAPL"
    echo "      stopped being pinned to native (H5VL__stream_scratch_fapl), or"
    echo "      backing_store is no longer false:"
    echo "$leaked" | sed 's/^/        /'
    exit 1
fi

echo "plugin-scratch: filter path ran under HDF5_VOL_CONNECTOR, no scratch artifacts"
exit 0
