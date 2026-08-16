#!/usr/bin/env python3
import sys

w = {}
with open("adios2_bench_writer_times.txt") as f:
    for line in f:
        s, t0, t1 = map(int, line.split())
        w[s] = (t0, t1)

r = {}
with open("adios2_bench_reader_times.txt") as f:
    for line in f:
        s, t1, bytes_ = map(int, line.split())
        r[s] = (t1, bytes_)

nsteps = len(w)
assert nsteps == len(r), f"step count mismatch: writer={len(w)} reader={len(r)}"

total_bytes = sum(r[s][1] for s in range(nsteps))
final_bytes = r[nsteps - 1][1]

lat = [r[s][0] - w[s][0] for s in range(nsteps)]          # start-to-receipt
commit = [w[s][1] - w[s][0] for s in range(nsteps)]        # writer commit latency

print("vol-stream benchmark: growing time-series array streamed over ADIOS2 SST")
print(f"  {nsteps} steps, /series reaches {final_bytes // 4} elements ({final_bytes/1024/1024:.1f} MiB) at the final step\n")

print("step  /series size   start-to-receipt latency   writer commit latency")
stride = max(1, nsteps // 10)
for s in range(0, nsteps, stride):
    print(f"{s:4d}  {r[s][1]:11d} B   {lat[s]/1e6:20.3f} ms   {commit[s]/1e6:14.3f} ms")

print(f"\ntotal bytes streamed (sum of every step's full snapshot): {total_bytes/1024/1024:.2f} MiB")
print(f"final step alone: {final_bytes/1024/1024:.2f} MiB ({100.0*final_bytes/total_bytes:.1f}% of the total)")
print(f"start-to-receipt latency (BeginStep -> reader's data in hand), {nsteps} steps: "
      f"min {min(lat)/1e6:.3f} ms, max {max(lat)/1e6:.3f} ms, mean {sum(lat)/nsteps/1e6:.3f} ms")
print(f"writer-side commit latency (BeginStep..EndStep), {nsteps} steps: "
      f"min {min(commit)/1e6:.3f} ms, max {max(commit)/1e6:.3f} ms, mean {sum(commit)/nsteps/1e6:.3f} ms")

span_ns = r[nsteps - 1][0] - w[0][0]
if span_ns > 0:
    print(f"aggregate throughput over the whole run: {total_bytes/1024/1024/(span_ns/1e9):.2f} MiB/s")
