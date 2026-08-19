/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Subscription routing on a multi-dimensional dataset.
 *
 * M8.5 routes a subscription by a 1-D element range. That range is derived
 * from the subscriber's dataspace by H5VL__stream_space_1d_bounds(), which
 * reads H5Sget_select_bounds()'s *dimension-0* low/high and uses them
 * directly as element indices. For a genuinely 1-D dataset those coincide
 * and everything is fine -- every existing subscription test is 1-D.
 *
 * For rank >= 2 they do not coincide at all: dimension 0 indexes rows, not
 * elements. A subscriber asking for row 0 of a ROWS x COLS dataset yields
 * the element range [0, 1) -- one element -- when the row it asked for is
 * COLS elements wide. The subscriber is then silently served a fraction of
 * what it subscribed to, which is data loss rather than merely coarse
 * routing.
 *
 * This test pins the N-D case: subscribe to the first row of a 2-D dataset
 * and require that the whole row arrives. It deliberately checks the
 * *count* of elements received, since that is what the bug truncates --
 * the values that do arrive are correct, so a value-only check would pass
 * while losing most of the data.
 *
 * Two processes, na+sm, same shape as test/t_subscribe.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define ROWS 4
#define COLS 8
#define FNAME "t_subvolume_nd.h5"

#define READY_SENTINEL       "t_subvolume_nd.reader_ready"
#define READER_DONE_SENTINEL "t_subvolume_nd.reader_done"

static int
wait_for_sentinel(const char *path, int max_iters)
{
    int i;

    for (i = 0; i < max_iters; i++) {
        FILE *f = fopen(path, "r");

        if (f) {
            fclose(f);
            return 0;
        }
        usleep(100000);
    }
    return -1;
}

static void
touch_sentinel(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f);
}

static int
run_reader(void)
{
    hid_t    vol_id, fapl, fid, space;
    hsize_t  dims[2]  = {ROWS, COLS};
    hsize_t  start[2] = {0, 0};
    hsize_t  count[2] = {1, COLS}; /* exactly the first row */
    uint64_t phys = (uint64_t)-1, elem_start = 0, elem_count = 0;
    char    *path = NULL;
    void    *buf  = NULL;
    size_t   size = 0;
    int      i, rc = 0;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("reader: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL fapl\n");
        return 1;
    }
    for (i = 0; i < 100; i++) {
        FILE *f = fopen(FNAME ".vsgroup", "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000);
    }
    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open/join\n");
        return 1;
    }

    if ((space = H5Screate_simple(2, dims, NULL)) < 0 ||
        H5Sselect_hyperslab(space, H5S_SELECT_SET, start, NULL, count, NULL) < 0) {
        printf("reader: FAIL build 2-D row selection\n");
        return 1;
    }
    {
        const char *paths[1]  = {"/grid"};
        const hid_t spaces[1] = {space};

        if (H5Fsubscribe(fid, 1, paths, spaces, NULL) < 0) {
            printf("reader: FAIL subscribe\n");
            return 1;
        }
    }
    H5Sclose(space);

    touch_sentinel(READY_SENTINEL);

    if (H5Fget_subscribed_data(fid, 10000, &phys, &path, &buf, &size, &elem_start, &elem_count) < 0) {
        printf("  FAIL  never received pushed data for /grid\n");
        rc = 1;
    }
    else {
        size_t got = size / sizeof(int);

        printf("  info  received %zu element(s), elem_start=%llu elem_count=%llu\n", got,
               (unsigned long long)elem_start, (unsigned long long)elem_count);

        /* The subscription covered one full row. Anything less is data the
         * subscriber asked for and did not get. */
        if (got < (size_t)COLS) {
            printf("  FAIL  subscribed to one row of %d elements but only %zu arrived -- an N-D "
                   "selection is being routed as if dimension 0 were an element index\n",
                   COLS, got);
            rc = 1;
        }
        else {
            const int *vals = (const int *)buf;
            int        ok   = 1;

            /* Row 0 holds values 0..COLS-1 (see run_writer()). Whatever the
             * push covers must start at the row's first element. */
            for (i = 0; i < COLS; i++)
                if (vals[i] != i) {
                    printf("  FAIL  /grid row 0 element %d = %d, expected %d\n", i, vals[i], i);
                    ok = 0;
                    break;
                }
            if (ok)
                printf("  ok    the whole subscribed row (%d elements) arrived intact\n", COLS);
            else
                rc = 1;
        }
        free(path);
        free(buf);
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    touch_sentinel(READER_DONE_SENTINEL);
    return rc;
}

static int
run_writer(void)
{
    hid_t   vol_id, fapl, fid, space, ds;
    hsize_t dims[2] = {ROWS, COLS};
    int     vals[ROWS * COLS];
    int     i;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("writer: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("writer: FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("writer: FAIL create (transport up? VOL_STREAM_NA set?)\n");
        return 1;
    }

    if (wait_for_sentinel(READY_SENTINEL, 100) < 0) {
        printf("writer: FAIL reader never subscribed\n");
        return 1;
    }

    if ((space = H5Screate_simple(2, dims, NULL)) < 0) {
        printf("writer: FAIL create dataspace\n");
        return 1;
    }
    /* Row-major: element (r,c) holds r*COLS + c, so row 0 is 0..COLS-1. */
    for (i = 0; i < ROWS * COLS; i++)
        vals[i] = i;

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("writer: FAIL begin_step\n");
        return 1;
    }
    if ((ds = H5Dcreate2(fid, "/grid", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0) {
        printf("writer: FAIL write /grid\n");
        return 1;
    }
    H5Dclose(ds);
    if (H5Fend_step(fid) < 0) {
        printf("writer: FAIL end_step\n");
        return 1;
    }

    H5Sclose(space);

    if (wait_for_sentinel(READER_DONE_SENTINEL, 100) < 0)
        printf("writer: reader never signalled done (proceeding to close anyway)\n");

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    return 0;
}

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status;
    int   nerrors = 0;

    printf("vol-stream: N-D subvolume subscription routing (na+sm)\n");

    /* A default, not a requirement: the CI matrix runs this suite over more
     * than one Mercury NA plugin, so an externally-set VOL_STREAM_NA wins
     * (overwrite = 0). Shared memory stays the default because it needs no
     * network setup on a bare runner. */
    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(READY_SENTINEL);
    unlink(READER_DONE_SENTINEL);

    /* Cleared before the fork, not inside run_writer(): a reader polls for
     * this sidecar to know the writer is up, so one left behind by a previous
     * run in the same directory sends it to join a group that died with that
     * run -- and then to open a file the current writer is about to unlink
     * ("can't retrieve stat info for file"). Same fix, and the same reason,
     * as t_precision_dual.c's own pre-fork cleanup. */
    unlink(FNAME ".vsgroup");
    unlink(FNAME);

    fflush(NULL);
    if ((pid = fork()) < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        int rc = run_reader();

        fflush(NULL);
        _exit(rc);
    }

    writer_status = run_writer();

    if (waitpid(pid, &reader_status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    unlink(READY_SENTINEL);
    unlink(READER_DONE_SENTINEL);

    if (writer_status != 0) {
        printf("\nwriter process reported failure\n");
        nerrors++;
    }
    if (!(WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 0)) {
        printf("\nreader process reported failure (status=%d)\n", reader_status);
        nerrors++;
    }

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
