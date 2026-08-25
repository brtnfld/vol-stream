/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Selection-exact routing on a dataset whose extent GROWS, and on one whose
 * selection needs more runs than the push budget.
 *
 * test/t_subvolume_strided.c already pins the column case -- a subscription
 * to a non-contiguous selection must receive that selection and not its
 * bounding span. It does so against a dataset created once at its final
 * size, which is the one shape a streaming connector rarely has. Two
 * separate gates made the same routing decline on the shape it usually does
 * have, each silently serving the whole bounding span, and neither was
 * observable from that test:
 *
 *   1. H5VL__stream_selection_runs() required the subscriber's dataspace
 *      extent to EQUAL the write's. A subscription is made once, against
 *      the shape the object will end up with; the object's dimension-0
 *      extent grows at every step. The two therefore agreed at no step but
 *      the last, so the refinement was disabled for the entire life of
 *      every growing dataset. The guard is now extent COMPATIBILITY:
 *      dimension 0 may differ freely (it does not enter any other
 *      dimension's stride), every trailing dimension must still match.
 *
 *   2. H5VL__stream_space_flat_runs() decomposed the WHOLE selection
 *      bounded by H5VL_STREAM_MAX_PUSH_RUNS and declined above it, before
 *      the result was clipped to the range being pushed. A column band's
 *      run count therefore scaled with the height of the OBJECT rather
 *      than with the size of the PUSH -- so a selection that is one run in
 *      every range it is ever evaluated against was rejected for being
 *      complex globally. Clipping now happens first, inside the
 *      decomposition, so out-of-range runs cost no budget.
 *
 * COLS is deliberately larger than H5VL_STREAM_MAX_PUSH_RUNS (256) rows'
 * worth of runs: the dataset grows to GROW_STEPS * ROWS_PER_STEP = 384
 * rows, and a column selection over it decomposes to 384 runs globally
 * while being exactly one run per pushed row-slab. Before gate 2's fix that
 * is a decline; after it, an exact answer.
 *
 * What this asserts, in the order it would break:
 *   1. every element the subscriber asked for arrives, at the right flat
 *      index with the right value -- under-sending is data loss;
 *   2. NOTHING else arrives. This is the actual regression guard, and it
 *      must be an equality on the element count, not a "fewer bytes than
 *      before" check: both gates fail by over-sending a correct superset,
 *      so a value-only test passes while the routing does nothing at all.
 *
 * Two processes, same shape as test/t_subvolume_strided.c; see main() for
 * why this one pins ofi+tcp rather than the suite's usual na+sm.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define ROWS_PER_STEP 32
#define GROW_STEPS    12
#define TOT_ROWS      (ROWS_PER_STEP * GROW_STEPS) /* 384 > the 256 run cap */
#define COLS          64

/* The column band: [BAND_LO, BAND_HI) of every row. Non-contiguous in flat
 * order, so its bounding span is very nearly the whole dataset. */
#define BAND_LO 24
#define BAND_HI 32
#define BAND_W  (BAND_HI - BAND_LO)

#define WANT_ELEMS ((size_t)TOT_ROWS * BAND_W)

#define FNAME "t_subvolume_grow.h5"

#define READY_SENTINEL       "t_subvolume_grow.reader_ready"
#define READER_DONE_SENTINEL "t_subvolume_grow.reader_done"

/* Value at (row, col). Distinct per element so a misrouted push cannot
 * coincidentally verify. */
static int
cell(int row, int col)
{
    return row * COLS + col;
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
run_writer(void)
{
    hid_t   vol_id, fapl, fid, space, dcpl, ds = -1;
    hsize_t dims[2]    = {ROWS_PER_STEP, COLS};
    hsize_t maxdims[2] = {H5S_UNLIMITED, COLS};
    hsize_t chunk[2]   = {ROWS_PER_STEP, COLS};
    int    *rows;
    int     s, r, c;

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

    /* A subscription only affects writes issued after it reaches the
     * writer, so the reader must be subscribed before step 0. */
    if (wait_for_sentinel(READY_SENTINEL, 300) < 0) {
        printf("writer: FAIL reader never signaled ready\n");
        return 1;
    }

    if (NULL == (rows = (int *)malloc((size_t)ROWS_PER_STEP * COLS * sizeof(int)))) {
        printf("writer: FAIL alloc\n");
        return 1;
    }
    if ((space = H5Screate_simple(2, dims, maxdims)) < 0 || (dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
        H5Pset_chunk(dcpl, 2, chunk) < 0) {
        printf("writer: FAIL space/dcpl\n");
        free(rows);
        return 1;
    }

    for (s = 0; s < GROW_STEPS; s++) {
        hsize_t newdims[2] = {(hsize_t)(s + 1) * ROWS_PER_STEP, COLS};
        hsize_t start[2]   = {(hsize_t)s * ROWS_PER_STEP, 0};
        hsize_t count[2]   = {ROWS_PER_STEP, COLS};
        hid_t   fspace, mspace;

        for (r = 0; r < ROWS_PER_STEP; r++)
            for (c = 0; c < COLS; c++)
                rows[r * COLS + c] = cell(s * ROWS_PER_STEP + r, c);

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            printf("writer: FAIL begin step %d\n", s);
            free(rows);
            return 1;
        }
        if (s == 0) {
            if ((ds = H5Dcreate2(fid, "/grid", H5T_NATIVE_INT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0) {
                printf("writer: FAIL create dataset\n");
                free(rows);
                return 1;
            }
        }
        else if (H5Dset_extent(ds, newdims) < 0) {
            printf("writer: FAIL set_extent step %d\n", s);
            free(rows);
            return 1;
        }

        if ((fspace = H5Dget_space(ds)) < 0 ||
            H5Sselect_hyperslab(fspace, H5S_SELECT_SET, start, NULL, count, NULL) < 0 ||
            (mspace = H5Screate_simple(2, count, NULL)) < 0) {
            printf("writer: FAIL spaces step %d\n", s);
            free(rows);
            return 1;
        }
        if (H5Dwrite(ds, H5T_NATIVE_INT, mspace, fspace, H5P_DEFAULT, rows) < 0 || H5Fend_step(fid) < 0) {
            printf("writer: FAIL write/end step %d\n", s);
            free(rows);
            return 1;
        }
        H5Sclose(mspace);
        H5Sclose(fspace);
    }

    H5Dclose(ds);
    H5Sclose(space);
    H5Pclose(dcpl);
    free(rows);

    /* The reader must close while this writer's group is still alive. */
    if (wait_for_sentinel(READER_DONE_SENTINEL, 600) < 0)
        printf("writer: WARNING reader never signaled done\n");

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}

static int
run_reader(void)
{
    hid_t    vol_id, fapl, fid, space;
    hsize_t  dims[2]  = {TOT_ROWS, COLS};
    hsize_t  start[2] = {0, BAND_LO};
    hsize_t  count[2] = {TOT_ROWS, BAND_W};
    char    *seen;
    size_t   got   = 0;
    int      rc    = 0, drained = 0;
    uint64_t bytes = 0, pushes = 0;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("reader: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL fapl\n");
        return 1;
    }
    if (wait_for_sentinel(FNAME ".vsgroup", 300) < 0) {
        printf("reader: FAIL writer's group sidecar never appeared\n");
        return 1;
    }
    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open\n");
        return 1;
    }

    /* Subscribe against the shape the object will END UP with -- the only
     * shape a subscriber can name up front, and the one the extent-equality
     * guard used to reject at every step but the last. */
    if ((space = H5Screate_simple(2, dims, NULL)) < 0 ||
        H5Sselect_hyperslab(space, H5S_SELECT_SET, start, NULL, count, NULL) < 0) {
        printf("reader: FAIL build column-band selection\n");
        return 1;
    }
    {
        const char *paths[1]  = {"/grid"};
        const hid_t spaces[1] = {space};

        if (H5Fsubscribe(fid, 1, paths, spaces, NULL) < 0) {
            printf("reader: FAIL subscribe\n");
            H5Sclose(space);
            return 1;
        }
    }
    H5Sclose(space);

    if (NULL == (seen = (char *)calloc(WANT_ELEMS, 1))) {
        printf("reader: FAIL alloc\n");
        return 1;
    }

    touch_sentinel(READY_SENTINEL);

    /* Pushes arrive one per contiguous run, and how many runs a step
     * produces is exactly what this test is about -- so drain until two
     * consecutive polls time out rather than looping a known count. */
    for (;;) {
        uint64_t phys = (uint64_t)-1, elem_start = 0, elem_count = 0;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;
        size_t   i, n;

        if (H5Fget_subscribed_data(fid, 2000, &phys, &path, &buf, &size, &elem_start, &elem_count) < 0) {
            if (++drained >= 2)
                break;
            continue;
        }
        drained = 0;
        pushes++;
        bytes += (uint64_t)size;

        n = size / sizeof(int);
        for (i = 0; i < n; i++) {
            uint64_t flat = elem_start + i;
            int      row  = (int)(flat / COLS);
            int      col  = (int)(flat % COLS);
            int      val  = ((const int *)buf)[i];
            size_t   slot;

            /* 1. Nothing outside the band may arrive. This is the assertion
             *    both gates fail: they over-send a correct superset, so
             *    checking values alone would pass while the routing did
             *    nothing whatsoever. */
            if (col < BAND_LO || col >= BAND_HI) {
                printf("  FAIL  received element outside the subscribed band: "
                       "flat %llu is (row %d, col %d)\n",
                       (unsigned long long)flat, row, col);
                rc = 1;
                break;
            }
            if (row < 0 || row >= TOT_ROWS) {
                printf("  FAIL  received element outside the extent: flat %llu\n",
                       (unsigned long long)flat);
                rc = 1;
                break;
            }
            /* 2. And what does arrive must be right, at the right index. */
            if (val != cell(row, col)) {
                printf("  FAIL  wrong value at (row %d, col %d): got %d, expected %d\n", row, col, val,
                       cell(row, col));
                rc = 1;
                break;
            }

            slot = (size_t)row * BAND_W + (size_t)(col - BAND_LO);
            if (!seen[slot]) {
                seen[slot] = 1;
                got++;
            }
        }
        free(path);
        free(buf);
        if (rc)
            break;
    }

    /* 3. And all of it must arrive -- under-sending is data loss, and is
     *    the failure mode a "fewer bytes" optimization risks introducing. */
    if (!rc && got != WANT_ELEMS) {
        printf("  FAIL  received %zu of %zu subscribed elements\n", got, WANT_ELEMS);
        rc = 1;
    }

    if (!rc) {
        printf("  ok    column band over a GROWING extent routed exactly: %zu elements, "
               "%llu bytes in %llu pushes\n",
               got, (unsigned long long)bytes, (unsigned long long)pushes);
        printf("  info  bounding span would have been %zu elements (%.1fx more)\n",
               (size_t)TOT_ROWS * COLS - (BAND_LO + (COLS - BAND_HI)),
               (double)((size_t)TOT_ROWS * COLS) / (double)WANT_ELEMS);
    }

    free(seen);
    touch_sentinel(READER_DONE_SENTINEL);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return rc;
}

int
main(void)
{
    pid_t reader_pid;
    int   status, rc = 0;

    /* ofi+tcp rather than the suite's usual na+sm default, for a legible
     * failure mode rather than a faster one. Both regressions this guards
     * fail by over-sending the bounding span -- roughly 8x the payload --
     * and na+sm crosses Mercury's eager threshold there and falls back to
     * process_vm_readv(), which Yama's default ptrace_scope blocks on many
     * machines. The test then dies with a cross-memory-attach error instead
     * of the assertion it exists for, which sends the next person chasing
     * the transport. Verified: with either gate reintroduced this reports
     * "received element outside the subscribed band" over ofi+tcp, and an
     * unrelated Mercury fatal over na+sm. */
    setenv("VOL_STREAM_NA", "ofi+tcp", 0);

    printf("vol-stream: selection-exact routing on a GROWING extent (%s)\n", getenv("VOL_STREAM_NA"));
    printf("  /grid grows to %d x %d in %d steps; subscription is columns [%d,%d) of every row\n", TOT_ROWS,
           COLS, GROW_STEPS, BAND_LO, BAND_HI);
    printf("  %zu subscribed elements; the selection is %d runs globally, 1 run per pushed slab\n",
           WANT_ELEMS, TOT_ROWS);

    unlink(FNAME);
    unlink(FNAME ".vsgroup");
    unlink(READY_SENTINEL);
    unlink(READER_DONE_SENTINEL);

    fflush(NULL);

    if ((reader_pid = fork()) < 0) {
        perror("fork");
        return 1;
    }
    if (reader_pid == 0) {
        int r = run_reader();

        fflush(NULL);
        _exit(r);
    }

    rc = run_writer();

    if (waitpid(reader_pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        rc = 1;

    unlink(FNAME);
    unlink(FNAME ".vsgroup");
    unlink(READY_SENTINEL);
    unlink(READER_DONE_SENTINEL);

    if (rc) {
        printf("\nFAILED\n");
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
