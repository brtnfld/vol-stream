#!/usr/bin/env bash
#
# M6 exit gate (first increment -- see docs/dev-plan.md's M6 section).
#
# Runs test/t_parallel.c's write phase with WRITER_RANKS MPI ranks, then its
# read phase with READER_RANKS ranks (coprime, deliberately a different
# decomposition of the same global dataset), over a file written through
# vol-stream. t_parallel itself verifies every element read back against a
# closed-form expected value -- this script's job is just launching both
# phases with the right rank counts and propagating failure.
#
# Usage:
#   run_parallel_test.sh --mpirun <path> --bin <path/to/t_parallel> [options]
#
#   --mpirun        mpirun/mpiexec binary
#   --bin           t_parallel executable
#   --plugin-dir    Directory holding libvol_stream.so (default: --bin's directory)
#   --writer-ranks  Number of writer ranks (default: 3)
#   --reader-ranks  Number of reader ranks (default: 2; must be coprime with
#                   --writer-ranks for this to actually exercise a
#                   differently-shaped decomposition, per the exit gate)
#   --per-rank      Elements per writer rank (default: 4)
#   --outdir        Working directory for the test file (default: ./parallel-test-results)
#   --concentration Writer ranks per I/O concentrator (default: 0/unset, meaning
#                   VOL_STREAM_CONCENTRATION is left unset -- every rank does its
#                   own raw-data I/O directly, M6.5's original scope). > 1 opts
#                   into the concentrator topology for the write phase only; the
#                   read phase is unaffected either way (see docs/dev-plan.md's
#                   M6.5 section and H5VL__stream_replay_concentrated_writes()'s
#                   comment in src/H5VLstream.c).
#
set -uo pipefail

MPIRUN=""
BIN=""
PLUGIN_DIR=""
WRITER_RANKS=3
READER_RANKS=2
PER_RANK=4
OUTDIR="./parallel-test-results"
CONCENTRATION=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mpirun)       MPIRUN="$2"; shift 2 ;;
        --bin)          BIN="$2"; shift 2 ;;
        --plugin-dir)   PLUGIN_DIR="$2"; shift 2 ;;
        --writer-ranks) WRITER_RANKS="$2"; shift 2 ;;
        --reader-ranks) READER_RANKS="$2"; shift 2 ;;
        --per-rank)     PER_RANK="$2"; shift 2 ;;
        --outdir)       OUTDIR="$2"; shift 2 ;;
        --concentration) CONCENTRATION="$2"; shift 2 ;;
        -h|--help)      sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$MPIRUN" ]] || { echo "error: --mpirun is required" >&2; exit 2; }
[[ -n "$BIN" ]]    || { echo "error: --bin is required" >&2; exit 2; }
[[ -x "$BIN" ]]    || { echo "error: --bin '$BIN' is not executable" >&2; exit 2; }

if [[ -z "$PLUGIN_DIR" ]]; then
    PLUGIN_DIR="$(cd "$(dirname "$BIN")" && pwd)"
fi
[[ -f "$PLUGIN_DIR/libvol_stream.so" ]] || {
    echo "error: no libvol_stream.so in '$PLUGIN_DIR'" >&2
    exit 2
}

mkdir -p "$OUTDIR"
cd "$OUTDIR" || exit 2
rm -f t_parallel.h5

# MPICH's default UCX transport probes for real InfiniBand hardware
# (ibv_create_srq() etc.) and aborts outright on a plain cloud VM that has
# none -- confirmed in CI ("ibv_create_srq() failed: Operation not
# supported", MPI_Init() itself failing before a single rank starts).
# Restricting to tcp/shared-memory/self transports is the standard fix and
# is exactly what na+sm-over-a-single-node needs anyway; harmless for Open
# MPI (a plain env var UCX-unaware MPI simply never reads).
export UCX_TLS=tcp,self,sm

echo "mpirun: $MPIRUN"
"$MPIRUN" --version 2>&1 | head -3
echo

# Slot/host declaration is not portable between implementations the way a
# plain env var is. Three earlier attempts here all tried different hydra
# host/launcher flags ("-host localhost:N", "-hosts localhost -launcher
# fork", then bare "-n N" matching HDF5's own CI recipe) to fix the same
# CI-only symptom (every rank independently reports MPI_COMM_WORLD size 1 --
# a silent per-process singleton-init fallback, not an error) -- none of
# them actually fixed it. The bare-flags attempt's own -verbose output
# finally showed the real cause: hydra's proxy reports "--proxy-core-count
# 1" / "(1 cores)" on this runner -- it believes there is exactly one core
# available, fewer than the ranks requested -- and, unlike Open MPI's clear
# "not enough slots" error, MPICH's hydra does not error on that; something
# in its default process-core binding for the excess ranks silently breaks
# their PMI rendezvous instead. HDF5's own CI never hits this because it
# caps MPICH tests at MPIEXEC_MAX_NUMPROCS=2, avoiding oversubscription
# rather than solving it. "-bind-to none" (disable hydra's process-core
# binding, letting the OS scheduler timeslice normally) is the standard fix
# for exactly this -- Open MPI's own --oversubscribe below already does the
# equivalent for its side. -verbose stays on for MPICH so a repeat failure
# keeps showing hydra's own trace instead of nothing.
if "$MPIRUN" --version 2>&1 | grep -qi "HYDRA build details"; then
    MPI_EXTRA_ARGS=(-bind-to none -verbose)
    echo "detected MPICH hydra -- using: ${MPI_EXTRA_ARGS[*]}"
else
    MPI_EXTRA_ARGS=(-host localhost --oversubscribe)
    echo "detected Open MPI (or unrecognized -- assuming Open MPI-compatible flags) -- using: ${MPI_EXTRA_ARGS[*]}"
fi
echo

GLOBAL_SIZE=$((WRITER_RANKS * PER_RANK))

echo "vol-stream M6 exit gate (first increment): ${WRITER_RANKS} writers -> ${READER_RANKS} readers"
echo "  global size: $GLOBAL_SIZE elements ($PER_RANK per writer rank)"
echo

if [[ "$CONCENTRATION" -gt 1 ]]; then
    echo "concentrator topology: VOL_STREAM_CONCENTRATION=$CONCENTRATION (every $CONCENTRATION writer ranks share one concentrator)"
    echo
fi

echo "== write ($WRITER_RANKS ranks) =="
write_log="$(mktemp)"
if [[ "$CONCENTRATION" -gt 1 ]]; then
    HDF5_PLUGIN_PATH="$PLUGIN_DIR" VOL_STREAM_CONCENTRATION="$CONCENTRATION" \
        "$MPIRUN" "${MPI_EXTRA_ARGS[@]}" -n "$WRITER_RANKS" "$BIN" write \
        t_parallel.h5 "$GLOBAL_SIZE" 2>&1 | tee "$write_log"
    write_rc=${PIPESTATUS[0]}
else
    HDF5_PLUGIN_PATH="$PLUGIN_DIR" "$MPIRUN" "${MPI_EXTRA_ARGS[@]}" -n "$WRITER_RANKS" "$BIN" write \
        t_parallel.h5 "$GLOBAL_SIZE" 2>&1 | tee "$write_log"
    write_rc=${PIPESTATUS[0]}
fi
if [[ $write_rc -ne 0 ]]; then
    echo
    rm -f "$write_log"
    echo "GATE FAILED: write phase rc=$write_rc"
    exit 1
fi
if [[ "$CONCENTRATION" -gt 1 ]] && ! grep -q "concentrator rank .* wrote .* DsetWrite entries on behalf of" "$write_log"; then
    echo
    rm -f "$write_log"
    echo "GATE FAILED: --concentration $CONCENTRATION requested but no concentrator aggregation was observed"
    exit 1
fi
rm -f "$write_log"

echo
echo "== read ($READER_RANKS ranks, decomposition independent of the writers) =="
HDF5_PLUGIN_PATH="$PLUGIN_DIR" "$MPIRUN" "${MPI_EXTRA_ARGS[@]}" -n "$READER_RANKS" "$BIN" read \
    t_parallel.h5 "$GLOBAL_SIZE" "$WRITER_RANKS"
read_rc=$?
if [[ $read_rc -ne 0 ]]; then
    echo
    echo "GATE FAILED: read phase rc=$read_rc"
    exit 1
fi

echo
echo "GATE PASSED: byte-exact data with coprime rank counts ($WRITER_RANKS -> $READER_RANKS)."
exit 0
