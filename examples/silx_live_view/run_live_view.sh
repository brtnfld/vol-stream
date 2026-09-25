#!/usr/bin/env bash
# Runs detector_writer and the silx live view against it. See README.md.
# Usage: run_live_view.sh [build-dir] [nframes] [delay-ms] [viewer options...]
# e.g.   run_live_view.sh build 200 100            # the silx window
#        run_live_view.sh build 8 500 --no-gui     # frames printed instead
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-}"
[ -n "$BUILD_DIR" ] && BUILD_DIR="$(cd "$BUILD_DIR" 2>/dev/null && pwd)"
NFRAMES="${2:-200}"
DELAY_MS="${3:-100}"
shift $(( $# < 3 ? $# : 3 ))

WRITER="${DETECTOR_WRITER:-}"
if [ -z "$WRITER" ]; then
    for d in "$BUILD_DIR/examples/detector_pipeline" "$BUILD_DIR" "$SCRIPT_DIR/../../build/examples/detector_pipeline"; do
        [ -x "$d/detector_writer" ] && { WRITER="$d/detector_writer"; break; }
    done
fi
[ -x "$WRITER" ] || { echo "run_live_view.sh: can't find detector_writer -- pass the build dir as \$1" >&2; exit 1; }
if [ -n "$BUILD_DIR" ] && [ -d "$BUILD_DIR/python" ]; then
    export PYTHONPATH="$BUILD_DIR/python${PYTHONPATH:+:$PYTHONPATH}"
    export HDF5_PLUGIN_PATH="${HDF5_PLUGIN_PATH:-$BUILD_DIR}"
fi
export VOL_STREAM_NA="${VOL_STREAM_NA:-na+sm}"
PYTHON="${PYTHON:-python3}"

RUNDIR="$(mktemp -d)"
cd "$RUNDIR" || exit 1
trap 'kill $WPID 2>/dev/null; wait 2>/dev/null; rm -rf "$RUNDIR"' EXIT

# nconsumers 0: the writer does not wait for anyone; the viewer joins late
# and --from-step (if given) backfills what it missed.
"$WRITER" "$NFRAMES" "$DELAY_MS" 0 > writer.log 2>&1 &
WPID=$!
for _ in $(seq 100); do [ -e detector_pipeline.h5 ] && break; sleep 0.1; done

"$PYTHON" "$SCRIPT_DIR/silx_live_view.py" detector_pipeline.h5 "$@"
rc=$?
wait "$WPID"
wrc=$?
if [ $rc -ne 0 ] || [ $wrc -ne 0 ]; then
    echo "--- writer.log"; cat writer.log
fi
[ $rc -ne 0 ] && exit $rc
exit $wrc
