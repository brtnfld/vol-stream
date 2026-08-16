/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef HEAT_COMMON_H
#define HEAT_COMMON_H

#include <stdio.h>
#include <unistd.h>

#define HEAT_FNAME "heat_diffusion.h5"
#define HEAT_DATASET "/temperature"
#define HEAT_DEFAULT_N 28
#define HEAT_HOT_EDGE 100.0
#define HEAT_COLD 0.0
/* A subscription is not retroactive -- a step the writer commits before a
 * monitor's H5Fsubscribe() call reaches it is simply never pushed to that
 * monitor. heat_monitor touches this the moment its subscription is up;
 * heat_writer waits for it before its first step, so it doesn't win that
 * race against its own first commit (see test/b_stream_grow_tail.c's
 * READY_SENTINEL for the same pattern already established in this repo). */
#define HEAT_READY_SENTINEL "heat_diffusion.ready"

/* Poll for a file's existence, up to timeout_ms. 0 on success, -1 on timeout.
 * Used to wait for the writer's SSG rendezvous sidecar (FNAME ".vsgroup"),
 * the same handshake every multi-process test in test/ relies on. */
static int
heat_wait_for_file(const char *path, int timeout_ms)
{
    int waited = 0;

    for (;;) {
        FILE *f = fopen(path, "r");

        if (f) {
            fclose(f);
            return 0;
        }
        if (waited >= timeout_ms)
            return -1;
        usleep(20000);
        waited += 20;
    }
}

static void
heat_touch(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f);
}

#endif /* HEAT_COMMON_H */
