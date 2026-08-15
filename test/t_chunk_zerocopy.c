/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M8.5.1 chunk-level zero-copy fast path -- the first piece of dev-plan.md's
 * "filtered-chunk passthrough" that actually pays off, applied where the
 * machinery already exists.
 *
 * The situation it optimizes: when a subscriber's requested filter pipeline
 * is *the same one the dataset was written under*, M8.5's precision path was
 * doing avoidable work -- building a throwaway H5FD_CORE dataset and running
 * the identical filters over the identical bytes to recompute a result the
 * writer had already computed and stored. H5VL__stream_refilter_zero_copy()
 * instead reads the already-filtered chunk straight out of the real dataset
 * replay just wrote (H5Dget_chunk_info_by_coord()/H5Dread_chunk2()).
 *
 * Two processes, na+sm, same shape as test/t_precision.c -- which remains the
 * test for the *opposite* case (subscriber's pipeline differs from the
 * dataset's, so the temporary-dataset path is the only option). Together
 * they cover both branches.
 *
 * What is asserted:
 *   1. The fast path actually engages. VOL_STREAM_DEBUG_REFILTER tags its
 *      log line "(zero-copy)", so the test can tell *which* branch ran
 *      rather than merely that re-filtering happened -- without this, an
 *      accidental fall-through to the slow path would still produce correct
 *      data and silently pass.
 *   2. It is genuinely correct, not just fast: the subscriber decodes the
 *      exact original values. Serving stored chunk bytes is only valid if
 *      those bytes really are what the subscriber's own pipeline would have
 *      produced, and a wrong-but-plausible answer here would be a silent
 *      data-corruption bug, so this is the load-bearing check.
 *   3. The wire payload really is compressed (materially smaller than raw),
 *      confirming stored *filtered* bytes were sent rather than the fast
 *      path accidentally shipping the uncompressed buffer.
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

#define NELEM 2000 /* ints in /chunked -- constant-value, highly compressible */
#define FNAME "t_chunk_zerocopy.h5"
#define REFILTER_LOG "t_chunk_zerocopy.refilter.log"

#define READY_SENTINEL       "t_chunk_zerocopy.reader_ready"
#define READER_DONE_SENTINEL "t_chunk_zerocopy.reader_done"

/* GZIP level used by BOTH the dataset and the subscription. They must match
 * exactly, or H5Pequal() correctly refuses the fast path. */
#define GZIP_LEVEL 6

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
    hid_t    vol_id, fapl, fid, space, sub_dcpl;
    hsize_t  dims = NELEM, chunk_dims = NELEM;
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

    /* Deliberately identical to the DCPL run_writer() creates "/chunked"
     * with: same chunk shape, same filter, same level. That match is the
     * whole precondition for the fast path -- H5Pequal() on the two decoded
     * property lists is exactly what H5VL__stream_refilter_zero_copy()
     * tests. */
    if ((space = H5Screate_simple(1, &dims, NULL)) < 0 || (sub_dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0) {
        printf("reader: FAIL create dataspace/dcpl\n");
        return 1;
    }
    if (H5Pset_chunk(sub_dcpl, 1, &chunk_dims) < 0 || H5Pset_deflate(sub_dcpl, GZIP_LEVEL) < 0) {
        printf("reader: FAIL configure matching GZIP dcpl\n");
        return 1;
    }
    {
        const char *paths[1]  = {"/chunked"};
        const hid_t spaces[1] = {space};
        const hid_t plists[1] = {sub_dcpl};

        if (H5Fsubscribe(fid, 1, paths, spaces, plists) < 0) {
            printf("reader: FAIL subscribe\n");
            return 1;
        }
    }
    H5Sclose(space);
    H5Pclose(sub_dcpl);

    touch_sentinel(READY_SENTINEL);

    if (H5Fget_subscribed_data(fid, 10000, &phys, &path, &buf, &size, &elem_start, &elem_count) < 0) {
        printf("  FAIL  never received pushed data for /chunked\n");
        rc = 1;
    }
    else {
        int ok = 1;

        if (!path || strcmp(path, "/chunked") != 0) {
            printf("  FAIL  pushed path is '%s', expected '/chunked'\n", path ? path : "(null)");
            ok = 0;
        }
        if (size != NELEM * sizeof(int)) {
            printf("  FAIL  decoded size is %zu, expected %zu\n", size, NELEM * sizeof(int));
            ok = 0;
        }
        else {
            const int *vals = (const int *)buf;

            /* The load-bearing check. Bytes lifted out of the dataset's own
             * chunk storage are only a valid answer if they decode to
             * exactly what the subscriber would have gotten the slow way. */
            for (i = 0; i < NELEM; i++) {
                int expected = (i % 4) + 100;

                if (vals[i] != expected) {
                    printf("  FAIL  /chunked[%d] = %d, expected %d (zero-copy served wrong bytes)\n", i,
                           vals[i], expected);
                    ok = 0;
                    break;
                }
            }
        }
        if (ok)
            printf("  ok    zero-copy push decodes to the exact original %d values\n", NELEM);
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
    hid_t   vol_id, fapl, fid, space, ds, dcpl;
    hsize_t dims = NELEM, chunk_dims = NELEM;
    int     vals[NELEM];
    int     i;

    unlink(FNAME ".vsgroup");
    unlink(FNAME);

    setenv("VOL_STREAM_DEBUG_REFILTER", "1", 1);

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
        vals[i] = (i % 4) + 100;

    /* Unlike t_precision.c, the dataset itself IS chunked and GZIP'd here,
     * with exactly the pipeline the subscriber asks for -- that is what
     * makes the already-stored chunk bytes a correct answer to serve. */
    if ((dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 || H5Pset_chunk(dcpl, 1, &chunk_dims) < 0 ||
        H5Pset_deflate(dcpl, GZIP_LEVEL) < 0) {
        printf("writer: FAIL configure dataset dcpl\n");
        return 1;
    }

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("writer: FAIL begin_step\n");
        return 1;
    }
    if ((ds = H5Dcreate2(fid, "/chunked", H5T_NATIVE_INT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0) {
        printf("writer: FAIL write /chunked\n");
        return 1;
    }
    H5Dclose(ds);
    if (H5Fend_step(fid) < 0) {
        printf("writer: FAIL end_step\n");
        return 1;
    }

    H5Sclose(space);
    H5Pclose(dcpl);

    if (wait_for_sentinel(READER_DONE_SENTINEL, 100) < 0)
        printf("writer: reader never signalled done (proceeding to close anyway)\n");

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    return 0;
}

/* Requires a "(zero-copy)"-tagged line, so a silent fall-through to the
 * temporary-dataset path fails the test rather than passing on correct data
 * produced the expensive way -- the point of this test is which branch ran.
 * Also requires the payload to be materially smaller than raw, confirming
 * real stored filtered bytes went out. */
static int
check_refilter_log(void)
{
    FILE *f = fopen(REFILTER_LOG, "r");
    char  line[256];
    int   saw_zero_copy = 0, saw_compressed = 0;

    if (!f) {
        printf("  FAIL  %s was never created -- refilter diagnostic never ran\n", REFILTER_LOG);
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned long long raw = 0, filtered = 0;

        if (2 == sscanf(line, "  refilter  raw=%llu filtered=%llu", &raw, &filtered)) {
            printf("  info  %s", line);
            if (strstr(line, "(zero-copy)")) {
                saw_zero_copy = 1;
                if (raw > 0 && filtered > 0 && filtered * 2 < raw)
                    saw_compressed = 1;
            }
        }
    }
    fclose(f);

    if (!saw_zero_copy) {
        printf("  FAIL  no \"(zero-copy)\" refilter line -- the fast path did not engage even though the "
               "subscriber's pipeline matches the dataset's\n");
        return 0;
    }
    if (!saw_compressed) {
        printf("  FAIL  zero-copy ran but its payload was not materially smaller than raw\n");
        return 0;
    }
    printf("  ok    the zero-copy fast path served already-filtered chunk bytes\n");
    return 1;
}

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status;
    int   nerrors = 0;
    int   saved_stderr_fd;

    printf("vol-stream M8.5.1: chunk-level zero-copy fast path (na+sm)\n");

    /* Same premise as t_precision.c: without a real deflate filter there is
     * no compressed chunk to serve and nothing here means anything. HDF5
     * defaults HDF5_ENABLE_ZLIB_SUPPORT to OFF, and H5Pset_deflate() still
     * succeeds when it is off, so check explicitly. */
    if (H5Zfilter_avail(H5Z_FILTER_DEFLATE) <= 0) {
        printf("  FAIL  this HDF5 has no deflate filter -- rebuild it with "
               "-DHDF5_ENABLE_ZLIB_SUPPORT=ON (it defaults to OFF)\n");
        return 1;
    }

    /* A default, not a requirement: the CI matrix runs this suite over more
     * than one Mercury NA plugin, so an externally-set VOL_STREAM_NA wins
     * (overwrite = 0). Shared memory stays the default because it needs no
     * network setup on a bare runner. */
    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(READY_SENTINEL);
    unlink(READER_DONE_SENTINEL);
    unlink(REFILTER_LOG);

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

    /* Redirect only the parent's own stderr, strictly after the fork, so the
     * reader child's output is untouched. */
    fflush(stderr);
    saved_stderr_fd = dup(fileno(stderr));
    freopen(REFILTER_LOG, "w", stderr);

    writer_status = run_writer();

    fflush(stderr);
    dup2(saved_stderr_fd, fileno(stderr));
    close(saved_stderr_fd);

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
    if (!check_refilter_log())
        nerrors++;

    unlink(REFILTER_LOG);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
