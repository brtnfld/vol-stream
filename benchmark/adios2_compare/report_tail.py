#!/usr/bin/env python3
# report.py's counterpart for the adios2_bench_tail variant -- total wall
# time, not aggregate throughput, is the number that matters here (see
# adios2_bench_tail.cpp's own comment): total bytes differs by design
# between the O(N) and O(N^2) patterns, so bytes/sec compares the wrong
# thing.
import sys

w = {}
with open("adios2_bench_tail_writer_times.txt") as f:
    for line in f:
        s, t0, t1 = map(int, line.split())
        w[s] = (t0, t1)

r = {}
with open("adios2_bench_tail_reader_times.txt") as f:
    for line in f:
        s, t1, bytes_ = map(int, line.split())
        r[s] = (t1, bytes_)

nsteps = len(w)
assert nsteps == len(r), f"step count mismatch: writer={len(w)} reader={len(r)}"

total_bytes = sum(r[s][1] for s in range(nsteps))

lat = [r[s][0] - w[s][0] for s in range(nsteps)]
commit = [w[s][1] - w[s][0] for s in range(nsteps)]

print("vol-stream benchmark counterpart: growing time-series array, TAIL-ONLY, over ADIOS2 SST")
print(f"  {nsteps} steps, each push is the new tail only\n")

print("step  pushed size   start-to-receipt latency   writer commit latency")
stride = max(1, nsteps // 10)
for s in range(0, nsteps, stride):
    print(f"{s:4d}  {r[s][1]:10d} B   {lat[s]/1e6:20.3f} ms   {commit[s]/1e6:14.3f} ms")

print(f"\ntotal bytes streamed (sum of every step's OWN new tail): {total_bytes/1024/1024:.2f} MiB")
print(f"start-to-receipt latency (BeginStep -> reader's data in hand), {nsteps} steps: "
      f"min {min(lat)/1e6:.3f} ms, max {max(lat)/1e6:.3f} ms, mean {sum(lat)/nsteps/1e6:.3f} ms")
print(f"writer-side commit latency (BeginStep..EndStep), {nsteps} steps: "
      f"min {min(commit)/1e6:.3f} ms, max {max(commit)/1e6:.3f} ms, mean {sum(commit)/nsteps/1e6:.3f} ms")

span_ns = r[nsteps - 1][0] - w[0][0]
if span_ns > 0:
    print(f"total wall time for the whole {nsteps}-step run: {span_ns/1e6:.2f} ms")
