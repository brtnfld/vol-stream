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
#
set -uo pipefail

MPIRUN=""
BIN=""
PLUGIN_DIR=""
WRITER_RANKS=3
READER_RANKS=2
PER_RANK=4
OUTDIR="./parallel-test-results"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mpirun)       MPIRUN="$2"; shift 2 ;;
        --bin)          BIN="$2"; shift 2 ;;
        --plugin-dir)   PLUGIN_DIR="$2"; shift 2 ;;
        --writer-ranks) WRITER_RANKS="$2"; shift 2 ;;
        --reader-ranks) READER_RANKS="$2"; shift 2 ;;
        --per-rank)     PER_RANK="$2"; shift 2 ;;
        --outdir)       OUTDIR="$2"; shift 2 ;;
        -h|--help)      sed -n '2,20p' "$0"; exit 0 ;;
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
# plain env var is -- Open MPI's "-host localhost:N declares N slots" and
# MPICH's hydra do not reliably agree on that syntax (confirmed in CI: with
# -host localhost:N, Open MPI launches N properly coordinated ranks, but
# MPICH's hydra silently launches N independent single-rank jobs instead --
# each one sees MPI_COMM_WORLD size 1, a correctness bug, not just a
# warning). Detect the implementation from its own --version banner
# ("HYDRA build details" is MPICH's hydra signature) and give each its own
# idiomatic flags rather than guessing at one syntax that works for both.
if "$MPIRUN" --version 2>&1 | grep -qi "HYDRA build details"; then
    MPI_EXTRA_ARGS=(-hosts localhost -launcher fork)
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

echo "== write ($WRITER_RANKS ranks) =="
HDF5_PLUGIN_PATH="$PLUGIN_DIR" "$MPIRUN" "${MPI_EXTRA_ARGS[@]}" -n "$WRITER_RANKS" "$BIN" write \
    t_parallel.h5 "$GLOBAL_SIZE"
write_rc=$?
if [[ $write_rc -ne 0 ]]; then
    echo
    echo "GATE FAILED: write phase rc=$write_rc"
    exit 1
fi

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
