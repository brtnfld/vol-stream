/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * How the archival case scales with step count.
 *
 * Every step is retained, each under its own /step/<n>/ group with its own
 * copies of the objects it wrote. Earlier measurements stopped at 50 steps.
 * This writes N steps of a small, realistic payload -- two 64-double
 * datasets and a scalar attribute, rewritten every step through handles kept
 * open -- and reports, for each N:
 *
 *   - H5Fend_step() time over the first and the last 100 steps, which shows
 *     whether a step gets more expensive as the file fills up;
 *   - file bytes per step;
 *   - how long a reader's H5Fopen() takes (it builds the step index);
 *   - how long reaching the last step and reading one dataset there takes.
 *
 * Measures rather than asserts; the one check is that the last step reads
 * back the right value. No transport needed.
 *
 * usage: b_step_scale [N ...]    (default: 100 1000 10000)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NVAL  64
#define FNAME "b_step_scale.h5"
#define EDGE  100 /* steps averaged at each end of the run */

static double
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int
run(hid_t vol_id, long n)
{
    hid_t    fapl, fid, space, scalar, t_ds = H5I_INVALID_HID, p_ds = H5I_INVALID_HID, attr = H5I_INVALID_HID;
    hsize_t  dims = NVAL;
    double   vals[NVAL], first_ms = 0, last_ms = 0, t0, open_ms, seek_ms;
    long     s;
    int      i, rc = 0;
    size_t   n_logical = 0;
    struct stat st;

    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0 ||
        (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (space = H5Screate_simple(1, &dims, NULL)) < 0 || (scalar = H5Screate(H5S_SCALAR)) < 0) {
        printf("FAIL setup\n");
        return 1;
    }

    for (s = 0; s < n; s++) {
        uint64_t logical = (uint64_t)s;
        double   scale   = (double)s;

        for (i = 0; i < NVAL; i++)
            vals[i] = (double)s + i * 1e-3;
        if (H5Fbegin_step(fid, 1, &logical, (uint64_t)s) < 0)
            goto fail;
        if (s == 0 &&
            ((t_ds = H5Dcreate2(fid, "/T", H5T_NATIVE_DOUBLE, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
             (p_ds = H5Dcreate2(fid, "/P", H5T_NATIVE_DOUBLE, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
             (attr = H5Acreate2(t_ds, "scale", H5T_NATIVE_DOUBLE, scalar, H5P_DEFAULT, H5P_DEFAULT)) < 0))
            goto fail;
        if (H5Dwrite(t_ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 ||
            H5Dwrite(p_ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 ||
            H5Awrite(attr, H5T_NATIVE_DOUBLE, &scale) < 0)
            goto fail;

        t0 = now_ms();
        if (H5Fend_step(fid) < 0)
            goto fail;
        t0 = now_ms() - t0;
        if (s < EDGE)
            first_ms += t0;
        if (s >= n - EDGE)
            last_ms += t0;
    }
    H5Aclose(attr);
    H5Dclose(t_ds);
    H5Dclose(p_ds);
    H5Sclose(space);
    H5Sclose(scalar);
    H5Fclose(fid);

    if (stat(FNAME, &st) < 0) {
        printf("FAIL stat\n");
        return 1;
    }

    /* Reader: open (which builds the step index), then go to the last step. */
    t0 = now_ms();
    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("FAIL reader open at N=%ld\n", n);
        return 1;
    }
    open_ms = now_ms() - t0;
    H5Fget_logical_steps(fid, &n_logical, NULL);

    t0 = now_ms();
    {
        hid_t  ds;
        double got[NVAL];

        if (H5Fbegin_logical_step(fid, (uint64_t)(n - 1)) < 0 || (ds = H5Dopen2(fid, "/T", H5P_DEFAULT)) < 0 ||
            H5Dread(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) < 0) {
            printf("FAIL reader could not read the last step at N=%ld\n", n);
            H5Fclose(fid);
            return 1;
        }
        seek_ms = now_ms() - t0;
        if (got[0] != (double)(n - 1)) {
            printf("FAIL last step read %g, expected %ld\n", got[0], n - 1);
            rc = 1;
        }
        H5Dclose(ds);
    }
    H5Fclose(fid);
    H5Pclose(fapl);

    {
        long edge = n < EDGE ? n : EDGE;

        printf("%8ld  %9.3f  %9.3f  %10.0f  %10.1f  %9.2f  %8zu\n", n, first_ms / (double)edge,
               last_ms / (double)edge, (double)st.st_size / (double)n, open_ms, seek_ms, n_logical);
    }
    fflush(stdout);
    remove(FNAME);
    return rc;

fail:
    printf("FAIL writer at step %ld\n", s);
    return 1;
}

int
main(int argc, char **argv)
{
    hid_t vol_id;
    long  defaults[] = {100, 1000, 10000};
    int   i, n = argc > 1 ? argc - 1 : 3, rc = 0;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("FAIL register\n");
        return 1;
    }
    printf("vol-stream: archival step-count scaling (payload staging %s)\n",
           getenv("VOL_STREAM_STAGE_PAYLOAD") ? getenv("VOL_STREAM_STAGE_PAYLOAD") : "default");
    printf("%8s  %9s  %9s  %10s  %10s  %9s  %8s\n", "steps", "end_ms@0", "end_ms@N", "bytes/step", "open_ms",
           "seek_ms", "logical");
    for (i = 0; i < n; i++)
        rc |= run(vol_id, argc > 1 ? atol(argv[i + 1]) : defaults[i]);
    H5VLclose(vol_id);
    return rc;
}
