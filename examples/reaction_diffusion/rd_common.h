/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef RD_COMMON_H
#define RD_COMMON_H

#include <stdio.h>
#include <unistd.h>

#define RD_FNAME "reaction_diffusion.h5"
#define RD_DATASET_U "/U"
#define RD_DATASET_V "/V"
#define RD_DEFAULT_N 80

/* Gray-Scott "mitosis" preset (Robert Munafo's classification): blobs that
 * continually divide rather than settling into a static pattern, unlike
 * heat_diffusion's steady-state relaxation -- this keeps visibly changing
 * for thousands of solver iterations. */
#define RD_DU 0.16
#define RD_DV 0.08
#define RD_FEED 0.0367
#define RD_KILL 0.0649

/* Same rendezvous handshake as heat_diffusion, for the same reason: a
 * subscription isn't retroactive, so the writer waits for a reader to be
 * ready before its first step (see heat_common.h's comment). */
#define RD_READY_SENTINEL "reaction_diffusion.ready"

static int
rd_wait_for_file(const char *path, int timeout_ms)
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
rd_touch(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f);
}

#endif /* RD_COMMON_H */
