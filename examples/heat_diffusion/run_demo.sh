#!/usr/bin/env bash
# Runs heat_writer and heat_monitor together: the writer in the background,
# the monitor in the foreground so its live redraw is what you see.
# Usage: run_demo.sh [build-dir] [grid-n] [nsteps] [substeps] [delay-ms]
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-}"
# Resolve to absolute now -- everything below cd's into a scratch run
# directory, so a relative path passed here would otherwise break.
[ -n "$BUILD_DIR" ] && BUILD_DIR="$(cd "$BUILD_DIR" 2>/dev/null && pwd)"

find_bin() {
    local name="$1"
    for d in "$BUILD_DIR" "$SCRIPT_DIR" "$SCRIPT_DIR/../../build/examples/heat_diffusion" \
             "$SCRIPT_DIR/../../build-nomercury/examples/heat_diffusion"; do
        [ -n "$d" ] && [ -x "$d/$name" ] && { echo "$d/$name"; return 0; }
    done
    return 1
}

WRITER=$(find_bin heat_writer) || { echo "run_demo.sh: can't find heat_writer -- pass its build dir as \$1" >&2; exit 1; }
MONITOR=$(find_bin heat_monitor) || { echo "run_demo.sh: can't find heat_monitor -- pass its build dir as \$1" >&2; exit 1; }

GRID_N="${2:-28}"
NSTEPS="${3:-150}"
SUBSTEPS="${4:-6}"
DELAY_MS="${5:-60}"

RUNDIR="$(mktemp -d)"
cd "$RUNDIR" || exit 1
GPID=""
trap 'kill "$WPID" "$GPID" 2>/dev/null; wait "$WPID" 2>/dev/null; rm -rf "$RUNDIR"' EXIT

# na+sm is vol-stream's usual default, but its zero-copy path needs
# cross-memory attach (process_vm_readv/writev), which some kernels disable
# by default (kernel.yama.ptrace_scope != 0) -- ofi+tcp has no such
# dependency and is what this project's own benchmarks fall back to when
# that's the case. Override with VOL_STREAM_NA=na+sm if your kernel allows it.
export VOL_STREAM_NA="${VOL_STREAM_NA:-ofi+tcp}"

echo "run_demo: workdir $RUNDIR"
"$WRITER" "$GRID_N" "$NSTEPS" "$SUBSTEPS" "$DELAY_MS" &
WPID=$!

# Best-effort: a real graphical heatmap alongside the terminal one, if
# gnuplot and a display are actually available. Never fatal on its own --
# the terminal heatmap from heat_monitor is the demo either way. gnuplot's
# first `plot` errors out (aborting the whole reread loop) if the frame
# file doesn't exist yet, so pre-seed a zeroed placeholder of the right
# shape before starting it -- heat_monitor overwrites it within its first
# received step regardless.
if [ -n "${DISPLAY:-}" ] && command -v gnuplot >/dev/null 2>&1; then
    awk -v n="$GRID_N" 'BEGIN { row = ""; for (j = 0; j < n; j++) row = row (j ? " " : "") "0"; for (i = 0; i < n; i++) print row }' \
        > heat_diffusion_frame.dat
    gnuplot "$SCRIPT_DIR/plot_live.gnuplot" >/dev/null 2>&1 &
    GPID=$!
fi

# Bounding the monitor to the writer's own step count matters, not just
# convenience: the monitor must finish (and stop touching the transport)
# BEFORE the writer's fixed-length run ends and tears down the rendezvous
# group, or the race goes the other way -- see heat_monitor.c's comment on
# why it skips its own graceful close.
"$MONITOR" "$GRID_N" "$NSTEPS"

wait "$WPID"
