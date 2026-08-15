/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Subscription pushes for a *strided* write.
 *
 * A push describes its payload as one flat element range
 * [elem_start, elem_start + elem_count), with the bytes contiguous from
 * elem_start. That description is only truthful when the write's own
 * selection is itself one contiguous flat run -- true for a whole-dataset
 * write and for a slab of leading dimensions, but not for a column of a 2-D
 * dataset, whose elements are strided across the extent.
 *
 * Before H5VL__stream_space_flat_runs(), a column write pushed its N packed
 * values labelled [first_element, first_element + N): the subscriber was
 * told they occupied consecutive flat indices when all but the first belong
 * elsewhere entirely. Observed directly on a 4x8 dataset -- a write of
 * column 0 alone was pushed as elem_start=0 elem_count=4, claiming flat
 * elements 0..3, three of which the writer never touched. Wrong values
 * rather than missing ones, which is the worse failure.
 *
 * This test writes ONLY column 0 and then checks every element of the push
 * against where it claims to be: the writer touched flat index f exactly
 * when f % COLS == 0, so any pushed element landing elsewhere is
 * mislabelled. The fix splits such a write into one push per contiguous run
 * (here, one per row), each truthfully describing its own bytes.
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
#define FNAME "t_strided_write.h5"

#define READY_SENTINEL       "t_strided_write.reader_ready"
#define READER_DONE_SENTINEL "t_strided_write.reader_done"

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
    hsize_t  count[2] = {ROWS, COLS}; /* whole dataset */
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
        if (got < 1) {
            printf("  FAIL  nothing arrived\n");
            rc = 1;
        }
        else {
            const int *vals = (const int *)buf;
            int        ok   = 1;

            /* Row 0 holds values 0..COLS-1 (see run_writer()). Whatever the
             * push covers must start at the row's first element. */
            /* Writer wrote ONLY column 0, values 1000+r at flat r*COLS.
             * Whatever range the push claims, flat index 0 must hold 1000. */
            /* Every element of this push claims flat index elem_start+k.
             * The writer wrote ONLY column 0, so flat index f holds a real
             * value exactly when f % COLS == 0, and that value is
             * 1000 + f / COLS. Anything else means the push mislabelled
             * where its bytes belong. */
            for (i = 0; i < (int)got; i++) {
                unsigned long long f = (unsigned long long)elem_start + (unsigned long long)i;

                if (f % COLS != 0) {
                    printf("  FAIL  push claims flat element %llu, which is column %llu -- the writer only "
                           "wrote column 0, so a strided write is being pushed as if contiguous\n",
                           f, f % COLS);
                    ok = 0;
                    break;
                }
                if (vals[i] != (int)(1000 + f / COLS)) {
                    printf("  FAIL  flat element %llu = %d, expected %llu\n", f, vals[i], 1000 + f / COLS);
                    ok = 0;
                    break;
                }
            }
            if (ok)
                printf("  ok    every pushed element lands at a flat index the writer actually wrote\n");
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

    unlink(FNAME ".vsgroup");
    unlink(FNAME);

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
    for (i = 0; i < ROWS; i++)
        vals[i] = 1000 + i; /* one value per row, for column 0 */

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("writer: FAIL begin_step\n");
        return 1;
    }
    if ((ds = H5Dcreate2(fid, "/grid", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        printf("writer: FAIL create /grid\n");
        return 1;
    }
    {
        hsize_t cstart[2] = {0, 0}, ccount[2] = {ROWS, 1}; /* column 0 */
        hsize_t mdim = ROWS;
        hid_t   mspace = H5Screate_simple(1, &mdim, NULL);
        hid_t   fspace = H5Scopy(space);

        H5Sselect_hyperslab(fspace, H5S_SELECT_SET, cstart, NULL, ccount, NULL);
        if (H5Dwrite(ds, H5T_NATIVE_INT, mspace, fspace, H5P_DEFAULT, vals) < 0) {
            printf("writer: FAIL column write\n");
            return 1;
        }
        H5Sclose(mspace);
        H5Sclose(fspace);
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

    printf("vol-stream: non-contiguous (column) write push (na+sm)\n");

    /* A default, not a requirement: the CI matrix runs this suite over more
     * than one Mercury NA plugin, so an externally-set VOL_STREAM_NA wins
     * (overwrite = 0). Shared memory stays the default because it needs no
     * network setup on a bare runner. */
    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(READY_SENTINEL);
    unlink(READER_DONE_SENTINEL);

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
