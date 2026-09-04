#!/usr/bin/env bash
# Runs narrowing_writer plus all three narrow_subscriber modes at once
# against it, and prints what each one measured. See README.md.
# Usage: run_demo.sh [build-dir] [nsteps] [delay-ms]
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-}"
[ -n "$BUILD_DIR" ] && BUILD_DIR="$(cd "$BUILD_DIR" 2>/dev/null && pwd)"

find_bin() {
    local name="$1"
    for d in "$BUILD_DIR" "$SCRIPT_DIR" "$SCRIPT_DIR/../../build/examples/narrowing_demo" \
             "$SCRIPT_DIR/../../build-nomercury/examples/narrowing_demo"; do
        [ -n "$d" ] && [ -x "$d/$name" ] && { echo "$d/$name"; return 0; }
    done
    return 1
}

WRITER=$(find_bin narrowing_writer) || { echo "run_demo.sh: can't find narrowing_writer -- pass its build dir as \$1" >&2; exit 1; }
SUB=$(find_bin narrow_subscriber) || { echo "run_demo.sh: can't find narrow_subscriber -- pass its build dir as \$1" >&2; exit 1; }

NSTEPS="${2:-8}"
DELAY_MS="${3:-500}"
STEP_TIMEOUT_MS=3000

RUNDIR="$(mktemp -d)"
cd "$RUNDIR" || exit 1
trap 'kill $WPID $FPID $GPID $PREDPID 2>/dev/null; wait 2>/dev/null; rm -rf "$RUNDIR"' EXIT

# na+sm is vol-stream's usual default, but its zero-copy path needs
# cross-memory attach (process_vm_readv/writev), which some kernels disable
# by default (kernel.yama.ptrace_scope != 0) -- ofi+tcp has no such
# dependency. Override with VOL_STREAM_NA=na+sm if your kernel allows it.
export VOL_STREAM_NA="${VOL_STREAM_NA:-ofi+tcp}"

echo "run_demo: workdir $RUNDIR"
echo "run_demo: starting narrowing_writer ($NSTEPS steps, ${DELAY_MS}ms apart)"
"$WRITER" "$NSTEPS" "$DELAY_MS" > writer.log 2>&1 &
WPID=$!

"$SUB" float "$NSTEPS" "$STEP_TIMEOUT_MS" > float.log 2>&1 &
FPID=$!
"$SUB" gzip "$NSTEPS" "$STEP_TIMEOUT_MS" > gzip.log 2>&1 &
GPID=$!
"$SUB" predicate "$NSTEPS" "$STEP_TIMEOUT_MS" > predicate.log 2>&1 &
PREDPID=$!

wait "$WPID"
wait "$FPID" "$GPID" "$PREDPID" 2>/dev/null

echo
echo "=============================================================="
echo "  writer (narrowing_writer) -- unfiltered, full double precision"
echo "=============================================================="
sed 's/^/  /' writer.log

echo
echo "=============================================================="
echo "  float subscriber -- H5Fsubscribe_type(H5T_NATIVE_FLOAT)"
echo "=============================================================="
sed 's/^/  /' float.log

echo
echo "=============================================================="
echo "  gzip subscriber -- H5Fsubscribe() + a GZIP DCPL"
echo "=============================================================="
sed 's/^/  /' gzip.log

echo
echo "=============================================================="
echo "  predicate subscriber -- H5Fsubscribe_predicate(GT, threshold)"
echo "=============================================================="
sed 's/^/  /' predicate.log

echo
echo "Same H5Dwrite() call, three subscribers, three different reductions."
echo "See the 'refilter' line under the writer above for the gzip"
echo "subscriber's real wire bytes -- H5Fget_subscribed_data() always"
echo "hands the subscriber back decoded values, so only the writer's own"
echo "log shows what actually crossed the wire."
