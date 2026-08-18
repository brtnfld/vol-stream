/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M8.5 per-subscriber precision: a subscriber's H5Fsubscribe() DCPL is now
 * actually acted on, not just validated -- see H5VL__stream_refilter_for_
 * subscriber()'s comment in src/H5VLstream.c and vs_tr_refilter_fn's in
 * tr_mercury.h.
 *
 * Two OS processes, na+sm, same shape as test/t_subscribe.c -- the CI-gating
 * exit gate, rock solid. test/t_precision_dual.c is its companion: the exit
 * gate met *literally* ("two subscribers... at different precisions from a
 * single end_step"), three processes, real and working, but not wired into
 * the default ctest run -- see that file's own top comment for why.
 *
 *   1. The writer creates/writes "/precise" (NELEM ints, a highly
 *      compressible constant-value pattern) with a plain, unfiltered DCPL
 *      -- the dataset itself is never compressed; any compression proven
 *      below happens purely because the *subscriber* asked for it.
 *   2. The reader subscribes to "/precise" with its own DCPL requesting
 *      GZIP -- H5Fsubscribe()'s plists parameter, unused until this
 *      increment.
 *   3. The reader retrieves the pushed data via H5Fget_subscribed_data()
 *      and checks it decodes back to the exact original values --
 *      H5Fget_subscribed_data() always hands back decoded values
 *      transparently, so a correct round trip through a real GZIP
 *      encode/decode is the core proof, and a wrong round trip would
 *      almost certainly show up as either wrong values or a hard decode
 *      failure (feeding non-deflate bytes through zlib inflate does not
 *      quietly produce plausible-looking wrong data).
 *   4. Separately, VOL_STREAM_DEBUG_REFILTER makes the writer log the
 *      actual raw/filtered byte counts to a file this test reads back --
 *      real, observable evidence that re-filtering happened and the wire
 *      bytes actually shrank, not just that decode succeeded (mirrors
 *      test/t_manifest_cache.c's own "prove it via an observable
 *      artifact, not just correctness" approach).
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

#define NELEM 2000 /* ints in /precise -- constant-value, highly compressible */
#define FNAME "t_precision.h5"
#define REFILTER_LOG "t_precision.refilter.log"

#define READY_SENTINEL       "t_precision.reader_ready"
#define READER_DONE_SENTINEL "t_precision.reader_done"

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
    uint64_t phys = (uint64_t)-1;
    char    *path        = NULL;
    void    *buf         = NULL;
    size_t   size        = 0;
    uint64_t elem_start = 0, elem_count = 0;
    hid_t    space, precise_dcpl;
    hsize_t  dims = NELEM, chunk_dims = NELEM;

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

    /* M8.5 precision: request GZIP on the pushed data via the subscription's
     * own DCPL -- "/precise" itself is written with H5P_DEFAULT (see
     * run_writer()), so this is purely a per-subscriber request. */
    if ((space = H5Screate_simple(1, &dims, NULL)) < 0 || (precise_dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0) {
        printf("reader: FAIL create dataspace/dcpl\n");
        return 1;
    }
    if (H5Pset_chunk(precise_dcpl, 1, &chunk_dims) < 0 || H5Pset_deflate(precise_dcpl, 6) < 0) {
        printf("reader: FAIL configure GZIP dcpl\n");
        return 1;
    }
    {
        const char *paths[1]  = {"/precise"};
        const hid_t spaces[1] = {space};
        const hid_t plists[1] = {precise_dcpl};

        if (H5Fsubscribe(fid, 1, paths, spaces, plists) < 0) {
            printf("reader: FAIL subscribe with precision dcpl\n");
            return 1;
        }
    }
    H5Sclose(space);
    H5Pclose(precise_dcpl);

    touch_sentinel(READY_SENTINEL);

    if (H5Fget_subscribed_data(fid, 10000, &phys, &path, &buf, &size, &elem_start, &elem_count) < 0) {
        printf("  FAIL  never received pushed data for /precise\n");
        rc = 1;
    }
    else {
        int ok = 1;

        if (!path || strcmp(path, "/precise") != 0) {
            printf("  FAIL  pushed path is '%s', expected '/precise'\n", path ? path : "(null)");
            ok = 0;
        }
        if (size != NELEM * sizeof(int)) {
            printf("  FAIL  decoded size is %zu, expected %zu (H5Fget_subscribed_data() must hand back "
                   "decoded values, not raw filtered bytes)\n",
                   size, NELEM * sizeof(int));
            ok = 0;
        }
        else {
            const int *vals = (const int *)buf;

            for (i = 0; i < NELEM; i++) {
                int expected = (i % 4) + 100;

                if (vals[i] != expected) {
                    printf("  FAIL  /precise[%d] = %d, expected %d (GZIP round trip produced wrong data)\n", i,
                           vals[i], expected);
                    ok = 0;
                    break;
                }
            }
        }
        if (ok)
            printf("  ok    GZIP-requested push decodes to the exact original %d values\n", NELEM);
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
    hid_t   vol_id, fapl, fid;
    hid_t   space, ds;
    hsize_t dims = NELEM;
    int     vals[NELEM];
    int     i;

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
    /* Constant-value pattern, deliberately maximally compressible -- makes
     * the size assertion in main() robust regardless of GZIP's exact ratio
     * on arbitrary data. */
    for (i = 0; i < NELEM; i++)
        vals[i] = (i % 4) + 100;

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("writer: FAIL begin_step\n");
        return 1;
    }
    /* Plain H5P_DEFAULT -- "/precise" itself is never compressed; only the
     * subscription's own dcpl (run_reader()) requests GZIP. */
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

/* Real, observable evidence re-filtering actually ran and actually shrank
 * the wire bytes -- see this file's top comment. Returns 1 if a
 * "raw=R filtered=F" line was found with F meaningfully smaller than R
 * (GZIP on a maximally-compressible constant pattern should do far better
 * than half; a generous 2x margin keeps this robust without being a tight,
 * brittle ratio check), 0 otherwise. */
static int
check_refilter_log(void)
{
    FILE *f = fopen(REFILTER_LOG, "r");
    char  line[256];
    int   found = 0;

    if (!f) {
        printf("  FAIL  %s was never created -- refilter diagnostic never ran\n", REFILTER_LOG);
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned long long raw = 0, filtered = 0;

        if (2 == sscanf(line, "  refilter  raw=%llu filtered=%llu", &raw, &filtered)) {
            printf("  info  %s\n", line);
            if (raw > 0 && filtered > 0 && filtered * 2 < raw)
                found = 1;
        }
    }
    fclose(f);
    if (!found)
        printf("  FAIL  no refilter log line showed filtered bytes meaningfully smaller than raw\n");
    return found;
}

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status;
    int   nerrors        = 0;
    int   saved_stderr_fd;

    printf("vol-stream M8.5: per-subscriber precision (na+sm)\n");

    /* This test's entire premise is that a subscriber's requested GZIP
     * pipeline measurably shrinks the wire bytes, so it cannot mean anything
     * without a working deflate filter. HDF5's own HDF5_ENABLE_ZLIB_SUPPORT
     * defaults to *OFF*, and H5Pset_deflate() still succeeds when it is off
     * (it only records the filter id in the DCPL) -- the failure surfaces
     * much later and very indirectly, as a push whose "filtered" size equals
     * its raw size plus an unrelated-looking "required filter is not
     * registered" error from the reader's decode. Checked explicitly here so
     * a zlib-less HDF5 reports the real reason instead of that puzzle. */
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

    /* Redirect only the parent's own stderr, only around run_writer() --
     * strictly after fork(), so the already-diverged reader child's stderr
     * is untouched. Restored immediately after so writer-side FAIL prints
     * (if any) still reach the terminal normally via later output. */
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
