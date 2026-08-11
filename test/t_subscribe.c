/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M8 exit gate, first increment, plus M8.5's subvolume routing on top: the
 * writer marshals only what is subscribed, down to the requested subrange,
 * not just the requested object. Full milestone text also calls for
 * per-subscriber precision (re-filtering) and chunk-storage-granularity
 * (H5Sselect_intersect_block against the FilteredChunks payload form) --
 * neither is built; this proves 1-D element-range intersection instead,
 * which already satisfies the exit gate's core numeric claim ("wire bytes
 * scale with subscribed volume") without either. See tr_mercury.c's header
 * comment for the full scope note.
 *
 * Two OS processes, na+sm, same shape as test/t_queue_policy.c.
 *
 *   1. The reader opens, subscribes to "/sub" bounded to
 *      [SUB_RANGE_START, SUB_RANGE_START+SUB_RANGE_COUNT) -- a strict
 *      subrange of the NSUB-element object, not the whole thing -- and
 *      waits.
 *   2. The writer commits one step creating and writing BOTH "/sub" (small,
 *      NSUB ints, written as one whole-object H5Dwrite -- the connector's
 *      own push logic is what narrows this down to the subscribed
 *      subrange, no special-casing on the write side) and "/unsub" (much
 *      larger, NUNSUB doubles) -- "/unsub" is deliberately the bigger of
 *      the two, so a bug that pushed everything regardless of subscription
 *      would be obvious rather than accidentally passing.
 *   3. The reader retrieves exactly one pushed item via
 *      H5Fget_subscribed_data(), checks it covers exactly the requested
 *      subrange (not the whole object) with the right content, then --
 *      after the writer signals it is completely done -- confirms nothing
 *      else ever arrives (proving "/unsub" was never pushed, not just "not
 *      yet").
 *
 * Only compiled/run when VOL_STREAM_HAVE_MERCURY is on; see
 * test/CMakeLists.txt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NSUB   4    /* ints in /sub -- NSUB * sizeof(int) bytes on the wire */
#define NUNSUB 2000 /* doubles in /unsub -- never subscribed, never pushed */
#define FNAME  "t_subscribe.h5"

/* M8.5: the reader subscribes to this strict subrange of /sub, not the
 * whole NSUB-element object -- proves subvolume intersection routing. */
#define SUB_RANGE_START 1
#define SUB_RANGE_COUNT 2

#define READY_SENTINEL       "t_subscribe.reader_ready"
#define WRITES_DONE_SENTINEL "t_subscribe.writes_done"
#define READER_DONE_SENTINEL "t_subscribe.reader_done"

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
    int      i, rc = 0;
    uint64_t phys       = (uint64_t)-1;
    char    *path        = NULL;
    void    *buf         = NULL;
    size_t   size        = 0;
    uint64_t elem_start = 0, elem_count = 0;
    hid_t    sub_space;
    int      sub_dims_i  = NSUB;
    hsize_t  sub_dims    = (hsize_t)sub_dims_i;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("reader: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL fapl\n");
        return 1;
    }
    /* Wait for the group sidecar, not the raw file -- the writer only
     * publishes it after an explicit flush (H5VL__stream_transport_start_
     * writer()), so its existence is what actually guarantees a durable
     * superblock to open, matching t_rendezvous.c's/t_transport.c's own
     * reader wait. */
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

    /* M8.5: subscribe to a strict SUBRANGE of /sub -- elements [SUB_RANGE_START,
     * SUB_RANGE_START+SUB_RANGE_COUNT), not the whole NSUB-element object --
     * so this test proves subvolume intersection routing, not just whole-
     * object routing (already covered by the fact that a whole-object
     * subscription is exactly sel_start=0/sel_count=extent, the same
     * mechanism with the bound relaxed to cover everything). */
    if ((sub_space = H5Screate_simple(1, &sub_dims, NULL)) < 0) {
        printf("reader: FAIL create subscription dataspace\n");
        return 1;
    }
    {
        hsize_t h_start = SUB_RANGE_START, h_count = SUB_RANGE_COUNT;

        if (H5Sselect_hyperslab(sub_space, H5S_SELECT_SET, &h_start, NULL, &h_count, NULL) < 0) {
            printf("reader: FAIL select subscription range\n");
            return 1;
        }
    }
    /* M8.5 closing a silent gap: a subscription to an attribute path (the
     * "@"-joined internal form H5VL__stream_attr_path() builds) now also
     * gets pushed, not just datasets. Whole-object (scalar, nothing to
     * bound a subrange of). */
    {
        hid_t attr_space;

        if ((attr_space = H5Screate(H5S_SCALAR)) < 0) {
            printf("reader: FAIL create attr subscription dataspace\n");
            return 1;
        }
        {
            const char *paths[2]  = {"/sub", "/sub@meta"};
            const hid_t spaces[2] = {sub_space, attr_space};

            if (H5Fsubscribe(fid, 2, paths, spaces, NULL) < 0) {
                printf("reader: FAIL subscribe\n");
                return 1;
            }
        }
        H5Sclose(attr_space);
    }
    H5Sclose(sub_space);

    touch_sentinel(READY_SENTINEL);

    if (H5Fget_subscribed_data(fid, 10000, &phys, &path, &buf, &size, &elem_start, &elem_count) < 0) {
        printf("  FAIL  never received pushed data for /sub\n");
        rc = 1;
    }
    else {
        int ok = 1;

        /* M8.5: exactly the requested [SUB_RANGE_START, SUB_RANGE_START+
         * SUB_RANGE_COUNT) subrange -- not the whole NSUB-element object --
         * is what proves subvolume intersection routing rather than just
         * whole-object routing. */
        if (elem_start != SUB_RANGE_START || elem_count != SUB_RANGE_COUNT) {
            printf("  FAIL  pushed range is [%llu, %llu), expected [%d, %d)\n",
                   (unsigned long long)elem_start, (unsigned long long)(elem_start + elem_count),
                   SUB_RANGE_START, SUB_RANGE_START + SUB_RANGE_COUNT);
            ok = 0;
        }

        if (!path || strcmp(path, "/sub") != 0) {
            printf("  FAIL  pushed path is '%s', expected '/sub'\n", path ? path : "(null)");
            ok = 0;
        }
        if (size != SUB_RANGE_COUNT * sizeof(int)) {
            printf("  FAIL  pushed size is %zu, expected %zu\n", size, SUB_RANGE_COUNT * sizeof(int));
            ok = 0;
        }
        else {
            const int *vals = (const int *)buf;

            for (i = 0; i < SUB_RANGE_COUNT; i++) {
                int expected = (SUB_RANGE_START + i) * 10;

                if (vals[i] != expected) {
                    printf("  FAIL  pushed /sub[%d] = %d, expected %d\n", SUB_RANGE_START + i, vals[i],
                           expected);
                    ok = 0;
                }
            }
        }
        if (ok)
            printf("  ok    received exactly the subscribed subrange (%zu of %zu bytes), content correct\n",
                   size, NSUB * sizeof(int));
        free(path);
        free(buf);
    }

    /* Attribute push -- closes the silent gap noted above. */
    {
        uint64_t phys3 = (uint64_t)-1, es3 = 0, ec3 = 0;
        char    *path3  = NULL;
        void    *buf3   = NULL;
        size_t   size3  = 0;

        if (H5Fget_subscribed_data(fid, 10000, &phys3, &path3, &buf3, &size3, &es3, &ec3) < 0) {
            printf("  FAIL  never received pushed data for /sub@meta\n");
            rc = 1;
        }
        else {
            int ok = 1;

            if (!path3 || strcmp(path3, "/sub@meta") != 0) {
                printf("  FAIL  pushed path is '%s', expected '/sub@meta'\n", path3 ? path3 : "(null)");
                ok = 0;
            }
            if (size3 != sizeof(int) || (buf3 && *(const int *)buf3 != 777)) {
                printf("  FAIL  attribute value wrong (size=%zu)\n", size3);
                ok = 0;
            }
            if (ok)
                printf("  ok    attribute subscription pushed too -- /sub@meta = 777\n");
            free(path3);
            free(buf3);
        }
    }

    /* The core thesis, made concrete: /unsub is NUSUB*8 = %zu bytes -- far
     * larger than what was actually pushed -- and was never subscribed.
     * Wait for the writer to fully finish committing, then confirm nothing
     * more ever arrives (a short poll here would only prove "not yet"). */
    if (wait_for_sentinel(WRITES_DONE_SENTINEL, 100) < 0) {
        printf("reader: FAIL writer never signalled writes done\n");
        rc = 1;
    }
    {
        uint64_t phys2 = (uint64_t)-1;
        char    *path2  = NULL;
        void    *buf2   = NULL;
        size_t   size2  = 0;
        uint64_t es2 = 0, ec2 = 0;

        if (H5Fget_subscribed_data(fid, 500, &phys2, &path2, &buf2, &size2, &es2, &ec2) == 0) {
            printf("  FAIL  received unexpected extra data for '%s' (%zu bytes) -- /unsub leaked\n",
                   path2 ? path2 : "(null)", size2);
            free(path2);
            free(buf2);
            rc = 1;
        }
        else
            printf("  ok    /unsub (%zu bytes, %dx bigger than /sub) never crossed the wire\n",
                   (size_t)NUNSUB * sizeof(double), (int)((NUNSUB * sizeof(double)) / (NSUB * sizeof(int))));
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    /* Close/leave before the writer does -- avoids the writer's
     * H5Fclose() (destroys the SSG group) racing this reader's own SSG
     * background threads, the same "harmless-but-noisy" shutdown race
     * test/t_transport.c's DONE_SENTINEL already works around. */
    touch_sentinel(READER_DONE_SENTINEL);
    return rc;
}

static int
run_writer(void)
{
    hid_t   vol_id, fapl, fid;
    hid_t   sub_space, unsub_space, ds;
    hsize_t sub_dims   = NSUB;
    hsize_t unsub_dims = NUNSUB;
    int     sub_vals[NSUB];
    double *unsub_vals;
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

    if ((sub_space = H5Screate_simple(1, &sub_dims, NULL)) < 0 ||
        (unsub_space = H5Screate_simple(1, &unsub_dims, NULL)) < 0) {
        printf("writer: FAIL create dataspaces\n");
        return 1;
    }
    if (NULL == (unsub_vals = (double *)malloc(NUNSUB * sizeof(double)))) {
        printf("writer: FAIL alloc\n");
        return 1;
    }
    for (i = 0; i < NSUB; i++)
        sub_vals[i] = i * 10;
    for (i = 0; i < NUNSUB; i++)
        unsub_vals[i] = (double)i;

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("writer: FAIL begin_step\n");
        return 1;
    }
    if ((ds = H5Dcreate2(fid, "/sub", H5T_NATIVE_INT, sub_space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, sub_vals) < 0) {
        printf("writer: FAIL write /sub\n");
        return 1;
    }
    /* An attribute subscription (M8.5) should get pushed too, closing a gap
     * M8's first increment left silent -- see run_reader()'s matching
     * block. */
    {
        hid_t   attr_space, attr;
        int     meta_val = 777;

        if ((attr_space = H5Screate(H5S_SCALAR)) < 0 ||
            (attr = H5Acreate2(ds, "meta", H5T_NATIVE_INT, attr_space, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
            H5Awrite(attr, H5T_NATIVE_INT, &meta_val) < 0) {
            printf("writer: FAIL create/write /sub@meta\n");
            return 1;
        }
        H5Aclose(attr);
        H5Sclose(attr_space);
    }
    H5Dclose(ds);
    if ((ds = H5Dcreate2(fid, "/unsub", H5T_NATIVE_DOUBLE, unsub_space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) <
            0 ||
        H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, unsub_vals) < 0) {
        printf("writer: FAIL write /unsub\n");
        return 1;
    }
    H5Dclose(ds);
    if (H5Fend_step(fid) < 0) {
        printf("writer: FAIL end_step\n");
        return 1;
    }
    touch_sentinel(WRITES_DONE_SENTINEL);

    H5Sclose(sub_space);
    H5Sclose(unsub_space);
    free(unsub_vals);

    /* Wait for the reader to finish and leave the group before this
     * H5Fclose() destroys it -- see run_reader()'s matching comment. */
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
    int   nerrors        = 0;

    printf("vol-stream M8: subscription protocol, first increment (na+sm)\n");

    setenv("VOL_STREAM_NA", "na+sm", 1);
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
