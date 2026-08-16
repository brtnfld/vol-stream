# vol-stream + VFD SWMR: a proof of concept

**The question this answers:** a live vol-stream file cannot be read. While the
writer holds it open, a second process gets `**NOT FOUND**` from `h5ls` against
a file that is plainly on disk with content in it. That is why the connector
carries a Mercury transport at all — it is the only channel by which another
process learns anything about a running stream.

**Unless the file sits on VFD SWMR.** This directory demonstrates that it can,
and that the result is usable.

## What is demonstrated

Three claims, each checked:

1. **The two stack, with no connector changes.** vol-stream is a VOL connector;
   VFD SWMR is a virtual file driver. `H5VL_stream_file_create()` copies the
   application's FAPL and overrides only the VOL, so a VFD SWMR configuration
   set by the application reaches the native connector untouched.
2. **A second process can read a live stream's file** — the `/step/<n>/` groups
   appearing as the writer commits.
3. **And can read the data**, not merely see the structure. Step *content* is
   raw data, which is a different question from whether the group exists.

> [!NOTE]
> **There is no transport here.** `vs_swmr_writer` is built without Mercury and
> unsets `VOL_STREAM_NA`. Everything the reader sees travelled through the
> file. That is the point — this is the one path that normally does not work.

## The two programs

| | |
|---|---|
| `vs_swmr_writer` | vol-stream over native over VFD SWMR. Commits 12 steps of a growing `/series` array, calling `H5Fvfd_swmr_end_tick()` after each `H5Fend_step()` |
| `vs_swmr_reader` | A **separate process** polling the same file while the writer holds it open. Counts visible steps and reads each new step's data |

`vs_swmr_reader` deliberately uses the **plain native VOL, not vol-stream**.
The claim under test is about the *persistence* path; vol-stream's own reader
index is built by scanning at open time and does not grow live, so involving it
would confound the measurement.

Both take one argument: `1` enables VFD SWMR (default), `0` is the control arm
without it. **The control arm is what makes this decisive** rather than merely
positive — it shows the difference is VFD SWMR and not something incidental.

## Running it

```
examples/vfd_swmr/run_poc.sh build/examples/vfd_swmr
```

or by hand, in two terminals from the build directory:

```
# terminal 1
./vs_swmr_writer 1
# terminal 2
./vs_swmr_reader 1
```

## Measured result

Against `feature/vfd-swmr-port`, Debug build, serial:

| Arm | Result |
|---|---|
| **Control, no VFD SWMR** | Reader opens the file but `/step` **never becomes visible**; step count stays at "not found" for the whole run |
| **VFD SWMR on** | Step count grows **2 → 12 live**; **10/10 content reads correct** |

```
reader: opened the live file (VFD SWMR ON)
reader: first observation: 2 step(s) visible
reader: 2 -> 3  read /step/2/series: 768 elems, first=0 last=767  [DATA OK]
...
reader: 11 -> 12 read /step/11/series: 3072 elems, first=0 last=3071  [DATA OK]
reader: live content reads: 10 ok, bad=0
reader: RESULT=live-growth-and-data-verified
```

The element counts also confirm vol-stream's carry-forward survives underneath
VFD SWMR: step 11 holds 3072 = 12 × 256 elements — a complete snapshot — even
though the writer only ever wrote the 256-element tail.

## What this does *not* show

- **Nothing about the reader-driven features.** Subscription, predicate
  pushdown, per-subscriber precision and queue-policy backpressure all need a
  reader → writer back-channel. A shared-filesystem mechanism has none. VFD
  SWMR can deliver the whole file, eventually visible — never "send me only
  the elements above this threshold". For that, see `examples/heat_diffusion`,
  which is the transport-based counterpart to this demo.
- **Nothing about parallel writers, filtered or chunked workloads, long runs,
  crash consistency, or Release builds.** One workload, once.

## Requirements and traps

VFD SWMR is not in mainline HDF5 2.x; it lives on the `feature/vfd-swmr-port`
branch, which also carries the `H5Tdecode2()`/`H5Dread_chunk2()` APIs
vol-stream needs — so it is the one build where both can coexist. This
directory is skipped entirely when the HDF5 in use lacks VFD SWMR
(`VOL_STREAM_HAVE_VFD_SWMR`).

> [!CAUTION]
> **`md_file_path` is a directory; `md_file_name` is the shadow file's name.**
> They are separate fields. Passing a full path as `md_file_path` and leaving
> `md_file_name` empty makes `H5Fcreate()` fail, and in a Debug HDF5 that
> surfaces as an assertion *inside the library*:
>
> ```
> H5Fint.c: H5F__dest: Assertion `H5AC_cache_is_clean(f, H5AC_RING_MDFSM)' failed.
> ```
>
> This looks like a library bug and is not one. It reproduces with plain native
> HDF5 and no vol-stream in the stack, which is the quickest way to confirm the
> connector is not implicated. Getting the two fields right fixes it.

> [!NOTE]
> `flush_raw_data` is set true here, and clearing it (`VS_SWMR_NO_FLUSH_RAW=1`)
> did **not** break the data reads when measured — probably because
> `H5Fvfd_swmr_end_tick()` pushes everything out anyway. The knob is left
> exposed so the next person can re-check rather than trust the claim.

Full write-up, including the interactions to watch when combining the two
lag windows: `docs/user-guide.md`, "Composing with VFD SWMR".
