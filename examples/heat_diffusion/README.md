# heat_diffusion: a real use case for vol-stream

Two independent programs, not a test harness:

- **heat_writer** solves 2D transient heat conduction on a square plate (one
  edge held hot, the other three cold, explicit finite differences) and
  streams its temperature field through vol-stream once per timestep.
- **heat_monitor** is a live in-situ subscriber: it connects to the writer
  over the transport, subscribes to `/temperature`, and redraws an ASCII
  heatmap plus running stats (mean, max, min, step-to-step change) every
  time a new field arrives -- while the simulation is still running.

This is the point of vol-stream's subscription protocol: `heat_monitor`
never opens the file and reads a snapshot, because it can't -- a plain HDF5
reader gets **NOT FOUND** against a file the writer still has open (see
`tools/h5stream.c`'s own comment on this). The only way to see the field as
it evolves is `H5Fsubscribe()` / `H5Fget_subscribed_data()` over the live
transport, exactly as an in-situ analysis or visualization tool coupled to
a running simulation would use it.

## Running it

Build normally (`VOL_STREAM_BUILD_EXAMPLES` defaults ON, gated on
`VOL_STREAM_HAVE_MERCURY` since subscription push requires the transport):

```
cmake --build build
```

Then either use the convenience script:

```
examples/heat_diffusion/run_demo.sh build
```

or run the two programs yourself in separate terminals from the build
directory, so you can see each side's own output:

```
# terminal 1
VOL_STREAM_NA=ofi+tcp ./examples/heat_diffusion/heat_writer 28 150

# terminal 2
VOL_STREAM_NA=ofi+tcp ./examples/heat_diffusion/heat_monitor 28 150
```

Pass `heat_monitor` the same step count as `heat_writer`'s second argument
(`nsteps`, default 150) as its own `max-steps`. This isn't just tidiness:
the monitor must observe its last step and finish *before* the writer's
fixed-length run ends and tears down the rendezvous group it's subscribed
through, or group teardown races the monitor's still-open wait -- see
`heat_monitor.c`'s closing comment. Left at its own default (`0`, watch
forever), the monitor will hang on that race once the writer finishes.

`ofi+tcp` is used here rather than vol-stream's usual `na+sm` default because
`na+sm`'s zero-copy path needs cross-memory attach
(`process_vm_readv`/`writev`), which some kernels disable by default
(`kernel.yama.ptrace_scope != 0`) -- on such a machine `na+sm` doesn't just
run slower, group rendezvous itself can hang. If your kernel allows it
(`sudo sysctl kernel.yama.ptrace_scope=0`), `na+sm` works too and needs no
network setup.

`heat_writer [grid-n] [nsteps] [substeps] [delay-ms]` -- defaults 28, 150,
6, 60. `heat_monitor [grid-n] [max-steps] [step-timeout-ms]` -- defaults
28, 0 (watch until the writer stops), 20000.

The grid size no longer has to match by hand: the monitor calls
`H5Fget_stream_schema()` and takes `/temperature`'s extent from the writer's
own dataspace, printing a line if that disagrees with the number given on the
command line. That argument is now a fallback for a writer that publishes no
schema, and the discovered dataspace is what `H5Fsubscribe()` receives -- an
in-situ tool cannot ask a user to retype the simulation's mesh.

## Watching it as a real graphical heatmap

`run_demo.sh` also launches a live `gnuplot` window automatically, best-effort,
if `gnuplot` and a display are available -- no flag needed, it just checks
and skips itself otherwise. Running the two programs by hand instead, add a
third terminal (same working directory as `heat_monitor`, since it's where
`heat_monitor` writes `heat_diffusion_frame.dat` -- a whitespace-matrix of
the current field, rewritten atomically every step):

```
# terminal 3, after heat_monitor has printed its "watching live" banner
gnuplot examples/heat_diffusion/plot_live.gnuplot
```

`plot_live.gnuplot` just polls that file on a `pause`/`reread` loop -- an
ordinary poll, not a gnuplot animation feature -- and needs the file to
exist before its first `plot`, so either wait for `heat_monitor`'s first
frame or pre-seed a zeroed placeholder of the same `grid-n x grid-n` shape
first (see `run_demo.sh`'s `awk` line for the one-liner).

## What to watch for

The monitor's heatmap should visibly sharpen from uniform cold into a
smooth gradient rising toward the hot edge, and `step-to-step change`
should shrink toward zero as the plate approaches steady state -- a
correctness check you get for free just by watching it run. If the ASCII
map stays blank or the monitor times out waiting for `heat_diffusion.h5` to
appear, check that both sides agree on `VOL_STREAM_NA` and that the writer
is actually still running (it exits after `nsteps`; restart it if the
monitor was started too late).
