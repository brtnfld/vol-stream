#!/usr/bin/env bash
# Runs detector_writer plus all five pipeline_consumer roles against it, then
# prints what each one measured -- plus a standalone "discover" run first,
# which is the part RFC section A.2 is actually about. See README.md.
# Usage: run_pipeline.sh [build-dir] [nframes] [delay-ms]
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-}"
[ -n "$BUILD_DIR" ] && BUILD_DIR="$(cd "$BUILD_DIR" 2>/dev/null && pwd)"

find_bin() {
    local name="$1"
    for d in "$BUILD_DIR" "$SCRIPT_DIR" "$SCRIPT_DIR/../../build/examples/detector_pipeline" \
             "$SCRIPT_DIR/../../build-hg/examples/detector_pipeline"; do
        [ -n "$d" ] && [ -x "$d/$name" ] && { echo "$d/$name"; return 0; }
    done
    return 1
}

WRITER=$(find_bin detector_writer) || { echo "run_pipeline.sh: can't find detector_writer -- pass its build dir as \$1" >&2; exit 1; }
CONSUMER=$(find_bin pipeline_consumer) || { echo "run_pipeline.sh: can't find pipeline_consumer -- pass its build dir as \$1" >&2; exit 1; }

NFRAMES="${2:-8}"
DELAY_MS="${3:-500}"
STEP_TIMEOUT_MS=3000
NMODULES=4

RUNDIR="$(mktemp -d)"
cd "$RUNDIR" || exit 1
trap 'kill $WPID $APID $VPID $NPID $HPID $MPID 2>/dev/null; wait 2>/dev/null; rm -rf "$RUNDIR"' EXIT

# Same rationale as narrowing_demo/run_demo.sh: na+sm's zero-copy path needs
# cross-memory attach, which some kernels disable (kernel.yama.ptrace_scope).
export VOL_STREAM_NA="${VOL_STREAM_NA:-ofi+tcp}"
# The viewer's real wire bytes are only observable in the writer's log.
export VOL_STREAM_DEBUG_REFILTER="${VOL_STREAM_DEBUG_REFILTER:-1}"

echo "run_pipeline: workdir $RUNDIR"
echo "run_pipeline: starting detector_writer ($NFRAMES frames, ${DELAY_MS}ms apart)"
"$WRITER" "$NFRAMES" "$DELAY_MS" > writer.log 2>&1 &
WPID=$!

"$CONSUMER" archive   0 "$NMODULES" "$NFRAMES" "$STEP_TIMEOUT_MS" > archive.log   2>&1 &
APID=$!
"$CONSUMER" viewer    0 "$NMODULES" "$NFRAMES" "$STEP_TIMEOUT_MS" > viewer.log    2>&1 &
VPID=$!
"$CONSUMER" analysis  2 "$NMODULES" "$NFRAMES" "$STEP_TIMEOUT_MS" > analysis.log  2>&1 &
NPID=$!
"$CONSUMER" hitfinder 0 "$NMODULES" "$NFRAMES" "$STEP_TIMEOUT_MS" > hitfinder.log 2>&1 &
HPID=$!
"$CONSUMER" monitor   0 "$NMODULES" "$NFRAMES" "$STEP_TIMEOUT_MS" > monitor.log   2>&1 &
MPID=$!

wait "$WPID"
wait "$APID" "$VPID" "$NPID" "$HPID" "$MPID" 2>/dev/null

show() {
    echo
    echo "=============================================================="
    echo "  $1"
    echo "=============================================================="
    sed 's/^/  /' "$2"
}

show "writer (detector_writer) -- NeXus layout, structure never announced" writer.log
show "archive   -- full fidelity (the NeXus writer's role)"                archive.log
show "viewer    -- int16 + deflate (the live view)"                        viewer.log
show "analysis  -- one panel by row band (the stitcher's input)"           analysis.log
show "hitfinder -- predicate GT (the veto role)"                           hitfinder.log
show "monitor   -- status + decision (the automated-feedback role)"        monitor.log

echo
echo "Every consumer above printed a schema it DISCOVERED -- no shape constant"
echo "is shared with the writer, and there is no second metadata channel. That"
echo "is the claim in RFC-VOLSTREAM-2026-001 section A.2. Run"
echo "  $CONSUMER discover"
echo "against a live writer to see it on its own."
