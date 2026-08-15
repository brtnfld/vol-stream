/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * A subscription whose selection is not contiguous in flat element order.
 *
 * test/t_subvolume_nd.c covers a *row* of a 2-D dataset, which is one
 * contiguous flat run, so bounding-box routing happens to be exact for it.
 * A *column* is the case that separates a bounding box from the selection
 * itself: column 0 of a ROWS x COLS dataset is ROWS single elements at flat
 * indices 0, COLS, 2*COLS, ..., and its bounding box spans the entire
 * dataset. Routing on the box therefore delivers every element of the
 * dataset -- correct, since it is a superset, but the subscriber asked for
 * ROWS elements out of ROWS*COLS.
 *
 * dev-plan.md's M8.5 section names this as the remaining routing gap:
 * "Subscription routing linearizes the selection's bounding box, so a
 * non-contiguous subscription receives a superset of what it asked for --
 * correct, but coarser than it could be."
 *
 * The fix threads the subscription's H5Sencode2 selection through to the
 * writer (decision #3's "a subscription is a list of (object path,
 * H5Sencode2 blob, ...)", which until now travelled as bounds only) and
 * intersects it with each write, pushing one RPC per contiguous run of the
 * real intersection.
 *
 * What this test asserts, in order of what would break first:
 *   1. every element the subscriber asked for arrives, at the right flat
 *      index with the right value -- under-sending is data loss, and is the
 *      failure mode a "fewer bytes" optimization risks introducing;
 *   2. nothing else arrives -- which is the actual improvement, and is
 *      measured in bytes rather than asserted.
 *
 * Two processes, na+sm, same shape as test/t_subvolume_nd.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define ROWS 6
#define COLS 8
#define COL  3 /* the subscribed column -- not 0, so a wrong stride shows up */

#define FNAME "t_subvolume_strided.h5"

#define READY_SENTINEL       "t_subvolume_strided.reader_ready"
#define WRITES_DONE_SENTINEL "t_subvolume_strided.writes_done"
#define READER_DONE_SENTINEL "t_subvolume_strided.reader_done"

/* Value at (r, c). Distinct per element so a misrouted push is obvious. */
static int
val_at(int r, int c)
{
    return r * 100 + c;
}

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
    hid_t   vol_id, fapl, fid, space;
    hsize_t dims[2]   = {ROWS, COLS};
    hsize_t start[2]  = {0, COL};
    hsize_t count[2]  = {ROWS, 1}; /* one column: ROWS separate flat runs */
    int     seen[ROWS];
    size_t  total_elems = 0, total_bytes = 0;
    int     i, rc = 0;

    memset(seen, 0, sizeof(seen));

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
        printf("reader: FAIL build column selection\n");
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

    /* Drain everything the writer sends, then judge the whole delivery.
     * A column arrives as several pushes (one per row), so a single
     * H5Fget_subscribed_data() call is not the unit of correctness here. */
    if (wait_for_sentinel(WRITES_DONE_SENTINEL, 200) < 0) {
        printf("reader: FAIL writer never signalled writes done\n");
        rc = 1;
    }

    for (;;) {
        uint64_t phys = 0, elem_start = 0, elem_count = 0;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;
        uint64_t k;

        if (H5Fget_subscribed_data(fid, 500, &phys, &path, &buf, &size, &elem_start, &elem_count) < 0)
            break;

        total_elems += (size_t)elem_count;
        total_bytes += size;

        for (k = 0; k < elem_count; k++) {
            uint64_t   flat = elem_start + k;
            int        r    = (int)(flat / COLS);
            int        c    = (int)(flat % COLS);
            const int *vals = (const int *)buf;

            if (c != COL) {
                /* Only reachable while routing still delivers the bounding
                 * box; the point of the fix is that it stops happening. */
                continue;
            }
            if (r < 0 || r >= ROWS) {
                printf("  FAIL  push covers flat index %llu, outside the dataset\n",
                       (unsigned long long)flat);
                rc = 1;
                continue;
            }
            if (vals[k] != val_at(r, c)) {
                printf("  FAIL  element (%d,%d) = %d, expected %d\n", r, c, vals[k], val_at(r, c));
                rc = 1;
            }
            seen[r] = 1;
        }

        free(path);
        free(buf);
    }

    /* 1. Nothing the subscriber asked for may be missing. */
    for (i = 0; i < ROWS; i++)
        if (!seen[i]) {
            printf("  FAIL  column element (%d,%d) never arrived -- under-sending is data loss\n", i, COL);
            rc = 1;
        }
    if (!rc)
        printf("  ok    all %d subscribed column elements arrived with correct values\n", ROWS);

    /* 2. And the improvement, measured. */
    {
        size_t asked = (size_t)ROWS;                 /* elements actually subscribed */
        size_t box   = (size_t)((ROWS - 1) * COLS + 1); /* bounding box of the column */

        printf("  info  received %zu element(s) / %zu bytes; subscription covers %zu, its bounding box "
               "%zu, whole dataset %d\n",
               total_elems, total_bytes, asked, box, ROWS * COLS);

        if (total_elems > asked) {
            printf("  FAIL  received %zu elements for a %zu-element subscription -- routing is still "
                   "using the bounding box, not the selection\n",
                   total_elems, asked);
            rc = 1;
        }
        else
            printf("  ok    exactly the subscribed elements crossed the wire (%zu of %zu in the bounding "
                   "box)\n",
                   total_elems, box);
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
    int     r, c;

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

    if (wait_for_sentinel(READY_SENTINEL, 200) < 0) {
        printf("writer: FAIL reader never subscribed\n");
        return 1;
    }

    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++)
            vals[r * COLS + c] = val_at(r, c);

    /* One whole-dataset write: the narrowing has to come from the
     * subscription, not from how the writer decomposed its own I/O. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || (space = H5Screate_simple(2, dims, NULL)) < 0 ||
        (ds = H5Dcreate2(fid, "/grid", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
        printf("writer: FAIL write step\n");
        return 1;
    }
    H5Dclose(ds);
    H5Sclose(space);

    touch_sentinel(WRITES_DONE_SENTINEL);

    wait_for_sentinel(READER_DONE_SENTINEL, 200);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status, nerrors = 0;

    printf("vol-stream M8.5: routing a non-contiguous subscription (na+sm)\n");

    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(READY_SENTINEL);
    unlink(WRITES_DONE_SENTINEL);
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
    unlink(WRITES_DONE_SENTINEL);
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
