/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M8.5 follow-up: a subscriber's requested chunk shape is now honored, not
 * just the pipeline -- see vs_tr_refilter_shape_fn's comment in
 * tr_mercury.h and H5VL__stream_refilter_shape_for_subscriber()'s in
 * src/H5VLstream.c.
 *
 * Every other precision test (t_precision.c, t_chunk_zerocopy.c) requests a
 * chunk shape equal to the whole run/extent -- exactly the case that must
 * NOT split, so none of them would notice a regression here either way.
 * This test is the one that actually asks for something smaller: NELEM=64,
 * requested chunk_dims=16, so a correct implementation pushes this as 4
 * separate RPCs (one push per chunk, elem_count=16 each) instead of 1
 * (elem_count=64). The core assertion is exactly that count -- receiving
 * fewer, larger pushes than requested is the residual this closes.
 *
 * GZIP is layered on top (not load-bearing for the split-count assertion,
 * but real correctness evidence): each chunk is independently filtered and
 * must independently decode back to the right values at the right
 * positions, proving the split doesn't corrupt data at the chunk
 * boundaries.
 *
 * Two OS processes, na+sm, same shape as test/t_precision.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NELEM 64
#define REQ_CHUNK 16 /* NELEM must be an exact multiple, so every push is the same size */
#define FNAME "t_chunk_shape_split.h5"

#define READY_SENTINEL "t_chunk_shape_split.reader_ready"
#define READER_DONE_SENTINEL "t_chunk_shape_split.reader_done"

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
    hid_t    vol_id, fapl, fid;
    int      rc = 0;
    hid_t    space, precise_dcpl;
    hsize_t  dims = NELEM, chunk_dims = REQ_CHUNK;
    int      got[NELEM];
    int      have[NELEM];
    int      i, n_pushes = 0, total_elems = 0;

    memset(have, 0, sizeof(have));

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("reader: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL fapl\n");
        return 1;
    }
    if (wait_for_sentinel(FNAME ".vsgroup", 100) < 0) {
        printf("reader: FAIL writer's group sidecar never appeared\n");
        return 1;
    }
    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open/join\n");
        return 1;
    }

    if ((space = H5Screate_simple(1, &dims, NULL)) < 0 || (precise_dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0) {
        printf("reader: FAIL create dataspace/dcpl\n");
        return 1;
    }
    if (H5Pset_chunk(precise_dcpl, 1, &chunk_dims) < 0 || H5Pset_deflate(precise_dcpl, 6) < 0) {
        printf("reader: FAIL configure chunked/GZIP dcpl\n");
        return 1;
    }
    {
        const char *paths[1]  = {"/precise"};
        const hid_t spaces[1] = {space};
        const hid_t plists[1] = {precise_dcpl};

        if (H5Fsubscribe(fid, 1, paths, spaces, plists) < 0) {
            printf("reader: FAIL subscribe with chunk-shape dcpl\n");
            return 1;
        }
    }
    H5Sclose(space);
    H5Pclose(precise_dcpl);

    touch_sentinel(READY_SENTINEL);

    /* Drain until every element has been seen once, or bail after twice as
     * many calls as the exact split would need -- if a bug merges chunks
     * back into fewer, larger pushes, this loop still terminates instead of
     * spinning, and n_pushes/total_elems below report what actually
     * happened either way. */
    while (total_elems < NELEM && n_pushes < 2 * (NELEM / REQ_CHUNK)) {
        uint64_t phys = (uint64_t)-1;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;
        uint64_t elem_start = 0, elem_count = 0;

        if (H5Fget_subscribed_data(fid, 10000, &phys, &path, &buf, &size, &elem_start, &elem_count) < 0) {
            printf("  FAIL  timed out after %d push(es), %d/%d element(s) received\n", n_pushes,
                   total_elems, NELEM);
            rc = 1;
            break;
        }
        n_pushes++;

        if (!path || strcmp(path, "/precise") != 0) {
            printf("  FAIL  push %d: path is '%s', expected '/precise'\n", n_pushes, path ? path : "(null)");
            rc = 1;
        }
        else if (elem_start + elem_count > NELEM) {
            printf("  FAIL  push %d: [%llu, %llu) runs past NELEM=%d\n", n_pushes,
                   (unsigned long long)elem_start, (unsigned long long)(elem_start + elem_count), NELEM);
            rc = 1;
        }
        else if (size != elem_count * sizeof(int)) {
            printf("  FAIL  push %d: decoded size %zu does not match elem_count %llu\n", n_pushes, size,
                   (unsigned long long)elem_count);
            rc = 1;
        }
        else {
            const int *vals = (const int *)buf;

            for (i = 0; i < (int)elem_count; i++) {
                int idx = (int)elem_start + i;

                got[idx]  = vals[i];
                have[idx] = 1;
            }
            total_elems += (int)elem_count;
            printf("  info  push %d: [%llu, %llu) -- %llu element(s)\n", n_pushes,
                   (unsigned long long)elem_start, (unsigned long long)(elem_start + elem_count),
                   (unsigned long long)elem_count);
        }

        free(path);
        free(buf);
        if (rc)
            break;
    }

    if (!rc) {
        if (n_pushes != NELEM / REQ_CHUNK) {
            printf("  FAIL  received %d push(es) for %d elements at chunk shape %d, expected exactly %d "
                   "(the subscriber's requested chunk shape was not honored)\n",
                   n_pushes, NELEM, REQ_CHUNK, NELEM / REQ_CHUNK);
            rc = 1;
        }
        else
            printf("  ok    received exactly %d push(es) of %d element(s) each -- requested chunk shape "
                   "honored, not forced to a single chunk spanning the whole run\n",
                   n_pushes, REQ_CHUNK);

        for (i = 0; i < NELEM; i++) {
            if (!have[i]) {
                printf("  FAIL  element %d was never received across any push\n", i);
                rc = 1;
                break;
            }
            if (got[i] != i) {
                printf("  FAIL  element %d decoded as %d, expected %d (chunk-boundary corruption)\n", i,
                       got[i], i);
                rc = 1;
                break;
            }
        }
        if (!rc)
            printf("  ok    all %d values decode correctly across chunk boundaries\n", NELEM);
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
    hid_t   vol_id, fapl, fid;
    hid_t   space, ds;
    hsize_t dims = NELEM;
    int     vals[NELEM];
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

    if ((space = H5Screate_simple(1, &dims, NULL)) < 0) {
        printf("writer: FAIL create dataspace\n");
        return 1;
    }
    for (i = 0; i < NELEM; i++)
        vals[i] = i;

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("writer: FAIL begin_step\n");
        return 1;
    }
    /* Plain H5P_DEFAULT, unchunked -- "/precise" itself carries no chunk
     * shape of its own; only the subscription's own dcpl (run_reader())
     * requests one, exercising the split purely as a subscriber-side
     * request against an ordinary contiguous dataset. */
    if ((ds = H5Dcreate2(fid, "/precise", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0) {
        printf("writer: FAIL write /precise\n");
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

    printf("vol-stream M8.5 follow-up: subscriber chunk shape is honored (na+sm)\n");

    if (H5Zfilter_avail(H5Z_FILTER_DEFLATE) <= 0) {
        printf("  FAIL  this HDF5 has no deflate filter -- rebuild it with "
               "-DHDF5_ENABLE_ZLIB_SUPPORT=ON (it defaults to OFF)\n");
        return 1;
    }

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
