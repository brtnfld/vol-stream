/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Writer for the "one write, narrowed per subscriber" demo (companion to
 * narrow_subscriber, which does the actual narrowing).
 *
 * A single double-precision dataset, /reading, written plain (H5P_DEFAULT,
 * no compression) once per step -- every reduction narrow_subscriber shows
 * is therefore the SUBSCRIBER's doing, not something baked into the file.
 * See narrowing_common.h's narrowing_step_is_hot(): even steps burst
 * NARROWING_HOT_COUNT elements to NARROWING_HOT_VALUE, odd steps are all
 * zero -- mostly-quiescent readings with occasional events, a real shape
 * for instrument data and, incidentally, one pattern that honestly serves
 * both the GZIP subscriber (highly compressible) and the predicate
 * subscriber (a real above/below-threshold split) without being tuned
 * separately for either.
 *
 * Synchronizes with its subscribers via H5Fwait_subscribers() rather than a
 * ready-sentinel file -- see test/t_rendezvous_barrier.c, whose whole point
 * is that this removes the shared-filesystem dependency a sentinel file
 * reintroduces. Not treated as fatal if fewer than N_SUBSCRIBERS attach in
 * time: this demo is meant to be run with 1, 2, or 3 narrow_subscriber
 * terminals, not strictly all three.
 *
 * VOL_STREAM_DEBUG_REFILTER is enabled unconditionally: the gzip
 * subscriber's actual wire bytes are only observable here, in the writer's
 * own "refilter filter=... raw=... filtered=..." log line, because
 * H5Fget_subscribed_data() on the subscriber side always hands back
 * decoded values (see test/t_precision.c's comment) -- the subscriber
 * cannot see its own compressed size.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"
#include "narrowing_common.h"

#define N_SUBSCRIBERS 3

int
main(int argc, char **argv)
{
    int nsteps   = argc > 1 ? atoi(argv[1]) : NARROWING_NSTEPS;
    int delay_ms = argc > 2 ? atoi(argv[2]) : 500;

    hid_t   vol_id, fapl, fid;
    hid_t   space = H5I_INVALID_HID, ds = H5I_INVALID_HID;
    double *vals;
    hsize_t dims = NARROWING_NELEM;
    int     s, i;

    setenv("VOL_STREAM_NA", "na+sm", 0);
    setenv("VOL_STREAM_DEBUG_REFILTER", "1", 0);
    unlink(NARROWING_FNAME);
    unlink(NARROWING_FNAME ".vsgroup");

    if (NULL == (vals = (double *)calloc((size_t)NARROWING_NELEM, sizeof(double)))) {
        fprintf(stderr, "writer: FAIL allocate\n");
        return 1;
    }

    if ((vol_id = H5VL_stream_register()) < 0) {
        fprintf(stderr, "writer: FAIL register vol-stream\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        fprintf(stderr, "writer: FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(NARROWING_FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        fprintf(stderr, "writer: FAIL create %s (transport up? VOL_STREAM_NA set?)\n", NARROWING_FNAME);
        return 1;
    }

    printf("writer: %d step(s) of %d-element /reading (double, unfiltered) -- %s\n", nsteps,
           NARROWING_NELEM, NARROWING_FNAME);
    printf("writer: waiting up to %ds for subscribers (run narrow_subscriber float|gzip|predicate "
           "in other terminals)...\n",
           NARROWING_BARRIER_TIMEOUT_MS / 1000);
    if (H5Fwait_subscribers(fid, N_SUBSCRIBERS, NARROWING_BARRIER_TIMEOUT_MS) < 0)
        printf("writer: proceeding without all %d subscribers attached (fewer is fine for this demo)\n",
               N_SUBSCRIBERS);
    else
        printf("writer: all %d subscribers attached, starting\n", N_SUBSCRIBERS);

    if ((space = H5Screate_simple(1, &dims, NULL)) < 0) {
        fprintf(stderr, "writer: FAIL create dataspace\n");
        return 1;
    }

    for (s = 0; s < nsteps; s++) {
        int hot       = narrowing_step_is_hot(s);
        int hot_start = (s * 137) % (NARROWING_NELEM - NARROWING_HOT_COUNT); /* rotates, for variety */

        for (i = 0; i < NARROWING_NELEM; i++)
            vals[i] = 0.0;
        if (hot)
            for (i = hot_start; i < hot_start + NARROWING_HOT_COUNT; i++)
                vals[i] = NARROWING_HOT_VALUE;

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            fprintf(stderr, "writer: FAIL begin_step %d\n", s);
            return 1;
        }
        /* Created once, rewritten through the same handle every step after
         * -- how a real simulation writes a per-iteration variable; see
         * test/t_predicate.c's comment on why recreating it every step is
         * not the pattern to follow. */
        if (s == 0 && (ds = H5Dcreate2(fid, NARROWING_DATASET, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT,
                                        H5P_DEFAULT, H5P_DEFAULT)) < 0) {
            fprintf(stderr, "writer: FAIL create %s\n", NARROWING_DATASET);
            return 1;
        }
        if (H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0) {
            fprintf(stderr, "writer: FAIL write step %d\n", s);
            return 1;
        }
        if (H5Fend_step(fid) < 0) {
            fprintf(stderr, "writer: FAIL end_step %d\n", s);
            return 1;
        }

        printf("writer: step %d/%d  %s  (raw %zu bytes)\n", s + 1, nsteps, hot ? "HOT " : "quiet",
               (size_t)NARROWING_NELEM * sizeof(double));
        fflush(stdout);
        if (delay_ms > 0)
            usleep((useconds_t)delay_ms * 1000);
    }

    printf("writer: done stepping, settling before close...\n");
    fflush(stdout);
    usleep(1500000); /* outlive subscribers still draining -- same settle window every demo here uses */

    H5Dclose(ds);
    H5Sclose(space);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    free(vals);

    printf("writer: done, %d step(s) written to %s\n", nsteps, NARROWING_FNAME);
    return 0;
}
