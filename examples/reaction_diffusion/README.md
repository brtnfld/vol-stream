# reaction_diffusion: a livelier real use case for vol-stream

The companion to `heat_diffusion`, for the same reason but a different
problem: `heat_diffusion` relaxes to a static steady state and then stops
being interesting to watch. This one doesn't. Two independent programs:

- **rd_writer** solves the Gray-Scott reaction-diffusion system (the
  "mitosis" parameter preset -- see `rd_common.h`) on a periodic 2D grid
  and streams both fields, `/U` and `/V`, through vol-stream once per
  timestep -- a real multi-object step, unlike `heat_diffusion`'s single
  dataset.
- **rd_monitor** subscribes to `/V` only (the field that actually shows the
  pattern) and redraws an ASCII heatmap, contrast-stretched to each frame's
  own min/max, every time new data arrives -- while the simulation is still
  running.

Same point as `heat_diffusion`: `rd_monitor` cannot open the file and read
a snapshot, because a plain HDF5 reader gets **NOT FOUND** against a file
the writer still has open. The only way to see the fields as they evolve is
`H5Fsubscribe()` / `H5Fget_subscribed_data()` over the live transport.

## Why this one instead of heat_diffusion

Gray-Scott's "mitosis" regime produces blobs that continually divide and
morph rather than smoothing out -- there's no steady state to reach within
any reasonable step count, so there's always something new in the next
push. It's still a real, well-known PDE system (not a toy animation), just
one whose dynamics stay interesting far longer than pure diffusion's.

## Running it

Build normally (built alongside `heat_diffusion`, same
`VOL_STREAM_BUILD_EXAMPLES` / `VOL_STREAM_HAVE_MERCURY` gating):

```
cmake --build build
```

Then either:

```
examples/reaction_diffusion/run_demo.sh build
```

or run the two programs yourself in separate terminals, passing
`rd_monitor` the same step count as `rd_writer`'s `nsteps` (its own
`max-steps` argument) so it finishes observing and closes gracefully
*before* the writer's fixed-length run ends and tears down the rendezvous
group -- same ordering requirement as `heat_diffusion`, see
`rd_monitor.c`'s closing comment:

```
# terminal 1
VOL_STREAM_NA=ofi+tcp ./examples/reaction_diffusion/rd_writer 80 150

# terminal 2
VOL_STREAM_NA=ofi+tcp ./examples/reaction_diffusion/rd_monitor 80 150
```

`rd_writer [grid-n] [nsteps] [substeps] [delay-ms]` -- defaults 80, 150,
25, 40. `rd_monitor [grid-n] [max-steps] [step-timeout-ms]` -- defaults 80,
0 (watch until the writer stops), 20000. The grid size must match on both
sides.

`run_demo.sh` also launches a live `gnuplot` window automatically,
best-effort, if `gnuplot` and a display are available -- see
`heat_diffusion/README.md`'s equivalent section for the manual `gnuplot`
invocation and the placeholder-frame caveat, identical here except the
file is `reaction_diffusion_frame.dat` and the palette is grayscale
(`set palette gray` in `plot_live.gnuplot`) rather than heat's warm ramp,
and there's no fixed `cbrange` -- V has no fixed physical bound the way a
temperature does, so it autoscales every frame.

## What to watch for

The seeded square in the center should grow, buckle, and split into
multiple blobs, which each keep splitting and drifting -- unlike
`heat_diffusion`, `step-to-step change` never meaningfully approaches zero
within a normal run's step count, because the system genuinely hasn't
converged. That is the correctness check here: if the pattern stays a
static square instead of dividing, something's wrong with the periodic
boundary wrap-around or the parameter preset, not just "hasn't run long
enough."
