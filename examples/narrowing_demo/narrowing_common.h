/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef NARROWING_COMMON_H
#define NARROWING_COMMON_H

#include <stdio.h>
#include <unistd.h>

#define NARROWING_FNAME   "narrowing_demo.h5"
#define NARROWING_DATASET "/reading"

/* 4000 doubles/step -- a round 32,000 raw bytes, big enough that the
 * reductions each subscriber demonstrates read as real numbers, not
 * rounding noise. */
#define NARROWING_NELEM  4000
#define NARROWING_NSTEPS 8

/* Baseline is 0.0 everywhere; a "hot" step sets one contiguous run to this
 * value. A single repeated value keeps the hot run maximally compressible,
 * so the same data pattern honestly supports both the GZIP story and the
 * predicate story -- it isn't tuned separately for each. Also a realistic
 * shape: mostly-quiescent instrument readings with occasional events. */
#define NARROWING_HOT_VALUE 5000.0
#define NARROWING_THRESHOLD 1.0
#define NARROWING_HOT_COUNT 300

/* How long the writer's H5Fwait_subscribers() will wait for attendees before
 * proceeding anyway (see narrowing_writer.c). A subscriber's own polling
 * loop must tolerate at least this much silence before its first push --
 * see narrow_subscriber.c's idle budget, which is deliberately longer than
 * this, not just its own per-call step timeout. */
#define NARROWING_BARRIER_TIMEOUT_MS 15000

static int
narrowing_wait_for_file(const char *path, int timeout_ms)
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

/* Even steps are "hot" (a burst above threshold), odd steps are "quiet"
 * (all-zero baseline) -- so a predicate subscriber sees both a real push
 * and real silence repeatedly over one run, not just once. */
static int
narrowing_step_is_hot(int s)
{
    return (s % 2) == 0;
}

#endif /* NARROWING_COMMON_H */
