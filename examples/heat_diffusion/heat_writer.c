/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * A real use case for vol-stream, not a test: a 2D heat-diffusion solver
 * that streams its temperature field to a live subscriber every timestep,
 * rather than writing checkpoints to disk for something else to poll.
 * Run alongside heat_monitor (see README.md / run_demo.sh) to watch a
 * running simulation converge in real time over the transport -- the thing
 * a plain HDF5 file cannot do while the writer still has it open (see
 * tools/h5stream.c's own comment on why `tail` needs the transport).
 *
 * The problem: a square plate, one edge held at a fixed hot temperature,
 * the other three held cold, interior starting cold -- transient conduction
 * toward a steady state, solved by explicit finite differences (FTCS).
 * Nothing here is vol-stream-specific except the per-step
 * H5Fbegin_step()/H5Dwrite()/H5Fend_step() bracket.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"
#include "heat_common.h"

static double *
heat_alloc_field(int n)
{
    return (double *)calloc((size_t)n * (size_t)n, sizeof(double));
}

/* Left edge hot, the other three cold; interior is left to the solver. */
static void
heat_init(double *t, int n)
{
    int i, j;

    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            t[i * n + j] = (j == 0) ? HEAT_HOT_EDGE : HEAT_COLD;
}

/* One explicit FTCS step. Boundary values are held fixed (Dirichlet). */
static void
heat_step(const double *t, double *tnew, int n, double r)
{
    int i, j;

    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            if (i == 0 || i == n - 1 || j == 0 || j == n - 1) {
                tnew[i * n + j] = t[i * n + j];
                continue;
            }
            tnew[i * n + j] = t[i * n + j] + r * (t[(i + 1) * n + j] + t[(i - 1) * n + j] +
                                                   t[i * n + j + 1] + t[i * n + j - 1] - 4.0 * t[i * n + j]);
        }
    }
}

static double
heat_mean(const double *t, int n)
{
    double sum = 0.0;
    int    i, total = n * n;

    for (i = 0; i < total; i++)
        sum += t[i];
    return sum / total;
}

int
main(int argc, char **argv)
{
    int    n        = argc > 1 ? atoi(argv[1]) : HEAT_DEFAULT_N;
    int    nsteps   = argc > 2 ? atoi(argv[2]) : 150;
    int    substeps = argc > 3 ? atoi(argv[3]) : 6;
    int    delay_ms = argc > 4 ? atoi(argv[4]) : 60;
    double r        = 0.24; /* alpha*dt/dx^2, must stay < 0.25 for 2D FTCS stability */

    hid_t   vol_id, fapl, fid;
    double *t, *tnew, *tmp;
    hsize_t dims[2];
    int     s, k;

    if (n < 3) {
        fprintf(stderr, "writer: grid size must be >= 3\n");
        return 1;
    }

    t    = heat_alloc_field(n);
    tnew = heat_alloc_field(n);
    if (!t || !tnew) {
        fprintf(stderr, "writer: FAIL allocate %dx%d field\n", n, n);
        return 1;
    }
    heat_init(t, n);

    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(HEAT_FNAME);
    unlink(HEAT_FNAME ".vsgroup");
    unlink(HEAT_READY_SENTINEL);

    if ((vol_id = H5VL_stream_register()) < 0) {
        fprintf(stderr, "writer: FAIL register vol-stream\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        fprintf(stderr, "writer: FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(HEAT_FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        fprintf(stderr, "writer: FAIL create %s (transport up? VOL_STREAM_NA set?)\n", HEAT_FNAME);
        return 1;
    }

    printf("writer: %dx%d grid, %d step(s) x %d solver iteration(s), r=%.3f -- %s\n", n, n, nsteps, substeps,
           r, HEAT_FNAME);

    /* A subscription isn't retroactive, so racing straight into step 1
     * risks committing before a monitor's H5Fsubscribe() call arrives --
     * see heat_common.h's comment on HEAT_READY_SENTINEL. Wait a bit for
     * one to show up; proceed solo if none does (heat_writer is still a
     * complete program on its own, just without a live observer). */
    printf("writer: waiting up to 8s for a subscriber...\n");
    if (heat_wait_for_file(HEAT_READY_SENTINEL, 8000) == 0)
        printf("writer: subscriber attached, starting\n");
    else
        printf("writer: no subscriber attached in time, proceeding solo\n");

    dims[0] = (hsize_t)n;
    dims[1] = (hsize_t)n;

    for (s = 0; s < nsteps; s++) {
        hid_t space, ds;

        for (k = 0; k < substeps; k++) {
            heat_step(t, tnew, n, r);
            tmp  = t;
            t    = tnew;
            tnew = tmp;
        }

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            fprintf(stderr, "writer: FAIL begin_step %d\n", s);
            return 1;
        }
        if ((space = H5Screate_simple(2, dims, NULL)) < 0 ||
            (ds = H5Dcreate2(fid, HEAT_DATASET, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT)) < 0 ||
            H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, t) < 0) {
            fprintf(stderr, "writer: FAIL write step %d\n", s);
            return 1;
        }
        H5Dclose(ds);
        H5Sclose(space);
        if (H5Fend_step(fid) < 0) {
            fprintf(stderr, "writer: FAIL end_step %d\n", s);
            return 1;
        }

        printf("writer: step %4d/%-4d  mean=%.4f\n", s + 1, nsteps, heat_mean(t, n));
        fflush(stdout);

        if (delay_ms > 0)
            usleep(delay_ms * 1000);
    }

    /* Settle window: closing the file immediately after the last step tears
     * down the transport's rendezvous group while a subscriber may still be
     * blocked waiting on the final push, which races the group-leave RPC
     * against that wait rather than letting it resolve first. This project
     * hit the same class of bug once before at transport stop (see
     * dev-plan.md's M8 status); the fix there was the same shape: give any
     * in-flight subscriber wait time to finish before tearing down. */
    printf("writer: done stepping, settling before close...\n");
    fflush(stdout);
    usleep(1500000);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    free(t);
    free(tnew);

    printf("writer: done, %d step(s) written to %s\n", nsteps, HEAT_FNAME);
    return 0;
}
