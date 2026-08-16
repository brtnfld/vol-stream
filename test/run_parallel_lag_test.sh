#!/usr/bin/env bash
# Copyright by The HDF Group.  All rights reserved.
# This file is part of vol-stream.  See the LICENSE file at the root of the
# source distribution, or https://www.hdfgroup.org/licenses.
#
# Runs t_parallel_lag: an MPI writer with a Block queue policy, and a separate
# single-process reader that deliberately lags. See t_parallel_lag.c for what
# is being proved and why it needs a real reader rather than the no-pressure
# parallel_queue_policy run.
#
# Writer and reader must overlap in time, which is why this cannot reuse
# run_parallel_test.sh -- that runs its write and read phases in sequence.

set -u

MPIRUN=""
BIN=""
PLUGIN_DIR=""
WRITER_RANKS=3

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mpirun)       MPIRUN="$2"; shift 2 ;;
        --bin)          BIN="$2"; shift 2 ;;
        --plugin-dir)   PLUGIN_DIR="$2"; shift 2 ;;
        --writer-ranks) WRITER_RANKS="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$MPIRUN" || -z "$BIN" || -z "$PLUGIN_DIR" ]]; then
    echo "usage: $0 --mpirun <mpiexec> --bin <t_parallel_lag> --plugin-dir <dir>" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

export HDF5_PLUGIN_PATH="$PLUGIN_DIR"
# ofi+tcp, not the na+sm the other transport tests default to. na+sm moves data
# with process_vm_readv/writev, which Linux's Yama LSM permits only between a
# process and its descendants (kernel.yama.ptrace_scope=1, the common default).
# Every other transport test fork()s its peer, so it stays inside that rule.
# Here the reader is a SIBLING of the mpiexec-launched writer ranks -- which is
# the whole point, since that is what makes it ack to exactly one rank -- and
# na+sm then fails with "Kernel Yama configuration does not allow cross-memory
# attach". A TCP provider has no such constraint.
export VOL_STREAM_NA="${VOL_STREAM_NA:-ofi+tcp}"
# Same MPICH-singleton workaround the sibling script documents at length.
export UCX_TLS=tcp,self,sm

MPI_EXTRA_ARGS=()
if "$MPIRUN" --version 2>&1 | grep -qi hydra; then
    MPI_EXTRA_ARGS+=(-localhost localhost)
fi

echo "== writer ($WRITER_RANKS ranks, Block policy) + lagging reader =="

"$MPIRUN" "${MPI_EXTRA_ARGS[@]}" -n "$WRITER_RANKS" "$BIN" write t_parallel_lag.h5 > writer.log 2>&1 &
writer_pid=$!

# The reader is deliberately NOT under mpiexec: it is an ordinary single
# process, which is what makes it ack to exactly one writer rank and produce
# the asymmetric lag view this test exists for.
"$BIN" read t_parallel_lag.h5 > reader.log 2>&1 &
reader_pid=$!

wait $writer_pid; writer_rc=$?
wait $reader_pid; reader_rc=$?

cat writer.log
cat reader.log

if [[ $reader_rc -ne 0 ]]; then
    echo "GATE FAILED: reader rc=$reader_rc"
    exit 1
fi
if [[ $writer_rc -ne 0 ]]; then
    echo "GATE FAILED: writer rc=$writer_rc"
    exit 1
fi
if ! grep -q "step 2 discarded unanimously" writer.log; then
    echo "GATE FAILED: writer never confirmed a unanimous discard"
    exit 1
fi

echo
echo "GATE PASSED: a parallel writer applied Discard collectively under real reader lag."
