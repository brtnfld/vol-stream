#!/bin/bash
# Same shape as run_adios2_bench.sh, for the tail-only (adios2_bench_tail)
# variant -- kept as a separate script rather than a flag so both variants'
# own artifacts (times files, .sst rendezvous) never collide if run back to
# back.
set -u
cd "$(dirname "$0")"

MPIRUN=/home/brtnfld/packages/mpich/bin/mpirun

rm -f adios2_bench_tail_writer_times.txt adios2_bench_tail_reader_times.txt \
      adios2_bench_tail_writer_done.txt adios2_bench_tail_reader_done.txt
rm -rf adios2_bench_tail_series*

"$MPIRUN" -n 1 ./adios2_bench_tail reader > reader_tail.log 2>&1 &
READER_PID=$!

"$MPIRUN" -n 1 ./adios2_bench_tail writer > writer_tail.log 2>&1 &
WRITER_PID=$!

wait $WRITER_PID
WRITER_RC=$?
wait $READER_PID
READER_RC=$?

echo "writer rc=$WRITER_RC reader rc=$READER_RC"
echo "--- writer_tail.log (tail) ---"
tail -20 writer_tail.log
echo "--- reader_tail.log (tail) ---"
tail -20 reader_tail.log

exit $(( WRITER_RC != 0 || READER_RC != 0 ))
