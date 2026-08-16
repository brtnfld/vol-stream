#!/bin/bash
# Launches the ADIOS2 SST benchmark's writer and reader as separate mpirun
# jobs (SST's normal usage -- not two ranks of one MPI_COMM_WORLD), waits
# for both, then reports. Mirrors test/b_stream_grow.c's own launch shape
# (a real writer and reader in separate OS processes) as closely as SST's
# own model allows.
set -u
cd "$(dirname "$0")"

MPIRUN=/home/brtnfld/packages/mpich/bin/mpirun

rm -f adios2_bench_series.sst adios2_bench_writer_times.txt adios2_bench_reader_times.txt \
      adios2_bench_writer_done.txt adios2_bench_reader_done.txt
rm -rf adios2_bench_series*

"$MPIRUN" -n 1 ./adios2_bench reader > reader.log 2>&1 &
READER_PID=$!

"$MPIRUN" -n 1 ./adios2_bench writer > writer.log 2>&1 &
WRITER_PID=$!

wait $WRITER_PID
WRITER_RC=$?
wait $READER_PID
READER_RC=$?

echo "writer rc=$WRITER_RC reader rc=$READER_RC"
echo "--- writer.log (tail) ---"
tail -20 writer.log
echo "--- reader.log (tail) ---"
tail -20 reader.log

exit $(( WRITER_RC != 0 || READER_RC != 0 ))
