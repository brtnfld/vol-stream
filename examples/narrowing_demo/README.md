# narrowing_demo: one write, narrowed per subscriber

The most direct demonstration of vol-stream's core claim: a single writer
commits one `H5Dwrite()` per step, and each subscriber narrows that same
write to its own precision, filter, or value condition -- **at the writer**,
before a byte ever reaches the network -- without the writer's own code
knowing or caring which subscribers are attached or what they asked for.

Two programs:

- **narrowing_writer** streams `/reading`, a 4000-element `double` array,
  plain and unfiltered (`H5P_DEFAULT`) -- every reduction below is therefore
  the *subscriber's* doing, not something baked into the file. Even steps
  burst 300 elements to a large value ("hot"); odd steps are all zero
  ("quiet") -- a mostly-quiescent-with-occasional-events shape, which is both
  a real class of instrument data and, incidentally, highly compressible.
- **narrow_subscriber** is one binary with three modes -- run it (up to)
  three times against the same writer:

  | mode        | API                                          | what it does |
  |-------------|-----------------------------------------------|--------------|
  | `float`     | `H5Fsubscribe_type(H5T_NATIVE_FLOAT)`          | delivered as 4-byte float instead of the dataset's own 8-byte double |
  | `gzip`      | `H5Fsubscribe()` with a GZIP-configured DCPL   | delivered GZIP-compressed, though the file itself is never compressed |
  | `predicate` | `H5Fsubscribe_predicate(GT, threshold)`        | delivered only elements above the threshold; a step with none sends **nothing at all** |

## Running it

```
cmake --build build   # or whichever build dir has VOL_STREAM_HAVE_MERCURY on
examples/narrowing_demo/run_demo.sh build/examples/narrowing_demo
```

That starts the writer and all three subscriber modes at once and prints
what each one measured. Or run them by hand in four terminals:

```
# terminal 1
VOL_STREAM_NA=ofi+tcp ./narrowing_writer

# terminals 2-4
VOL_STREAM_NA=ofi+tcp ./narrow_subscriber float
VOL_STREAM_NA=ofi+tcp ./narrow_subscriber gzip
VOL_STREAM_NA=ofi+tcp ./narrow_subscriber predicate
```

`narrowing_writer [nsteps] [delay-ms]` -- defaults 8, 500.
`narrow_subscriber <float|gzip|predicate> [max-steps] [step-timeout-ms]`.

The writer calls `H5Fwait_subscribers()` (no ready-sentinel file -- see
`test/t_rendezvous_barrier.c`) and will wait up to 15s for all three modes
to attach, but proceeds anyway if fewer show up: run it with just one or two
subscriber terminals and those still work correctly, just without the
others' numbers to compare against.

## What each mode actually measures

**float**: every push is exactly half the bytes of the same step's double
delivery (measured: 16,000 B where double would be 32,000 B) -- the
conversion happens on the writer via `H5Tconvert()` before marshaling, so
this is a real halving of the wire payload, not a cast the subscriber
applies after receiving the full 8 bytes/element.

**gzip**: `H5Fget_subscribed_data()` always hands the subscriber back
**decoded** values (see `test/t_precision.c`), so this mode's own `size` is
the *original* 32,000 bytes, every time -- proving the round trip is exactly
correct, not that it was small. The actual compressed wire bytes only show
up in **the writer's own terminal**, in its `refilter filter=... raw=...
filtered=...` log line (`VOL_STREAM_DEBUG_REFILTER`, on by default for this
demo). Measured on the mostly-zero/occasional-burst pattern above: raw
32,000 bytes down to 53-65 bytes per step -- roughly 500x, entirely because
one subscriber asked for GZIP on a dataset the file itself never compresses.

**predicate**: only the "hot" steps (every other one) ever produce a push
-- 300 elements / 2,400 bytes each. The "quiet" steps produce **zero
pushes**, not small ones: the writer evaluates the threshold and does not
marshal or send anything at all when nothing matches. Compared to what an
unfiltered whole-object subscription would have cost across all 8 steps
(256,000 bytes), this mode moves under 10,000 -- and half the steps cost
literally nothing.

## Why this is a fair comparison

The writer never knows the file will be read at reduced precision, GZIP'd,
or filtered by value -- it just calls `H5Dwrite()` on a plain, unfiltered
array. All three subscriptions are configured entirely from the reader
side, after the fact, and the writer's code does not change depending on
which subscribers (if any) are attached. That is what "narrows before a
byte leaves the writer, not after" means concretely: the reduction is real
network-wire savings, decided per subscriber, computed by the writer,
without becoming the writer's problem to know about in advance.
