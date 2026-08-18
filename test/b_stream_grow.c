/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Benchmark, not a correctness test: the ADIOS2-style growing time-series
 * array (t_dset_resize.c's case 2, the supported pattern now that resizing
 * a live cross-step handle is refused), streamed for real -- a writer and a
 * reader in two separate OS processes over Mercury, the same two-process
 * shape test/t_transport.c uses for its exit gate.
 *
 * Every existing timing-flavored file in this suite (t_parallel_lag.c,
 * t_queue_policy.c, t_rendezvous.c) uses clock_gettime(CLOCK_MONOTONIC) to
 * bound a wait, not to report a number. This is the first file whose actual
 * point is the numbers.
 *
 * What it measures, once per step s = 0..NSTEPS-1, /series growing from
 * CHUNK to NSTEPS*CHUNK elements (re-created fresh and rewritten in full
 * each step -- the working alternative to the declined in-place resize):
 *
 *   - writer-side commit latency: H5Fbegin_step() through H5Fend_step()
 *     returning, i.e. the cost this connector adds to committing a step of
 *     that step's own size;
 *   - end-to-end latency: from the writer's H5Fend_step() returning to the
 *     reader's H5Dread() of that step's /series returning, over na+sm --
 *     real notification-plus-transfer cost, not a same-process shortcut;
 *   - aggregate throughput over the whole run.
 *
 * The self-sufficient-snapshot convention (each step's own copy holds the
 * FULL array so far, not just what changed) is what re-creating each step
 * naturally gives you, and it costs what it looks like it costs: total
 * bytes moved across N steps growing by CHUNK each time is O(N^2), not
 * O(N) -- reported explicitly below rather than left to be inferred from a
 * single aggregate number.
 *
 * Both processes share one clock (CLOCK_MONOTONIC is machine-wide, and both
 * are forked from the same parent, so no cross-process clock sync is
 * needed) and one mmap'd MAP_SHARED timing array -- no pipes, no extra
 * files, and nothing that depends on the HDF5 API's own wall_time_ns
 * (which is an opaque application value here, not a benchmark hook).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <time.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME          "b_stream_grow.h5"
#define READY_SENTINEL "b_stream_grow.reader_ready"
#define DONE_SENTINEL  "b_stream_grow.reader_done"

#define NSTEPS 50
#define CHUNK  8192 /* elements added per step; 4 bytes each */

typedef struct {
    long long commit_start_ns; /* writer: right before H5Fbegin_step() */
    long long commit_end_ns;   /* writer: right after H5Fend_step() returns */
    long long receipt_ns;      /* reader: right after H5Dread() returns */
    uint64_t  bytes;           /* reader: this step's /series size in bytes */
} step_timing_t;

static step_timing_t *g_timing; /* mmap'd MAP_SHARED, filled by both children */

static long long
now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static void
touch(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f);
}

static int
wait_for(const char *path, int max_polls)
{
    int i;

    for (i = 0; i < max_polls; i++) {
        FILE *f = fopen(path, "r");

        if (f) {
            fclose(f);
            return 0;
        }
        usleep(20000); /* 20ms */
    }
    return -1;
}

static int
run_writer(void)
{
    hid_t vol_id, fapl, fid;
    int   s, *vals;

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

    if (wait_for(READY_SENTINEL, 200) < 0)
        printf("writer: WARNING reader never signaled ready; proceeding anyway\n");

    if (NULL == (vals = (int *)malloc((size_t)NSTEPS * CHUNK * sizeof(int)))) {
        printf("writer: FAIL alloc\n");
        return 1;
    }

    for (s = 0; s < NSTEPS; s++) {
        size_t  n_elem = (size_t)(s + 1) * CHUNK;
        hsize_t dims   = (hsize_t)n_elem;
        hid_t   space, ds;
        int     i;

        for (i = 0; i < (int)n_elem; i++)
            vals[i] = i;

        g_timing[s].commit_start_ns = now_ns();

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || (space = H5Screate_simple(1, &dims, NULL)) < 0 ||
            (ds = H5Dcreate2(fid, "/series", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) <
                0) {
            printf("writer: FAIL begin/create step %d\n", s);
            free(vals);
            return 1;
        }
        if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
            printf("writer: FAIL write/end step %d\n", s);
            free(vals);
            return 1;
        }
        H5Dclose(ds);
        H5Sclose(space);

        g_timing[s].commit_end_ns = now_ns();
    }

    free(vals);

    if (wait_for(DONE_SENTINEL, 200) < 0)
        printf("writer: WARNING reader never signaled done\n");

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}

static int
run_reader(void)
{
    hid_t vol_id, fapl, fid;
    int   s, rc = 0;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("reader: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL fapl\n");
        return 1;
    }

    if (wait_for(FNAME ".vsgroup", 200) < 0) {
        printf("reader: FAIL writer's group sidecar never appeared\n");
        return 1;
    }
    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open (writer not there yet? transport up?)\n");
        return 1;
    }

    /* H5Fwait_step_ready() + H5Fbegin_step() + H5Dopen2() (t_manypaths.c's
     * pattern) does NOT work here: H5Fbegin_step()'s reader-index build is a
     * one-time scan of the underlying file's own group listing
     * (H5VL__stream_reader_build_index()), the same kind of native metadata
     * read that h5ls/h5dump fail at while the writer holds the file open
     * (dev-plan.md's "existing tools on a live stream" finding) -- it is
     * simply not visible from a second process's own file handle without
     * the writer closing first. That pattern is for a *finished* stream.
     *
     * The only mechanism that actually delivers live data is subscription:
     * H5Fget_subscribed_data() receives pushed bytes over the SAME Mercury
     * RPC round trip that H5Fwait_step_ready() rides, never touching the
     * underlying file's metadata at all -- which is exactly why M8's
     * subscription protocol exists as the differentiator, not an add-on. */
    {
        hid_t   sub_space;
        hsize_t sub_dims = (hsize_t)NSTEPS * CHUNK; /* generous upper bound; see note below */
        const char *paths[1]  = {"/series"};

        if ((sub_space = H5Screate_simple(1, &sub_dims, NULL)) < 0) {
            printf("reader: FAIL subscription dataspace\n");
            return 1;
        }
        {
            const hid_t spaces[1] = {sub_space};

            /* This subscription's own extent never matches any single
             * step's growing /series extent, so M8.5's own extent-mismatch
             * rule (src/H5VLstream.c) declines the exact-selection routing
             * and falls back to sending the write's whole overlap -- which,
             * for a whole-object write, is exactly this step's whole
             * cumulative array. That fallback is what is being measured. */
            if (H5Fsubscribe(fid, 1, paths, spaces, NULL) < 0) {
                printf("reader: FAIL subscribe to /series\n");
                H5Sclose(sub_space);
                return 1;
            }
        }
        H5Sclose(sub_space);
    }

    touch(READY_SENTINEL);

    for (s = 0; s < NSTEPS; s++) {
        uint64_t phys = (uint64_t)-1, elem_start = 0, elem_count = 0;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;

        if (H5Fget_subscribed_data(fid, 10000 /* 10s */, &phys, &path, &buf, &size, &elem_start,
                                    &elem_count) < 0) {
            printf("reader: FAIL subscribed data %d (timed out)\n", s);
            rc = 1;
            break;
        }
        g_timing[s].receipt_ns = now_ns();
        g_timing[s].bytes      = (uint64_t)size;

        {
            const int *ints  = (const int *)buf;
            size_t     count = size / sizeof(int);

            if (count != (size_t)(s + 1) * CHUNK || ints[0] != 0 || ints[count - 1] != (int)count - 1) {
                printf("reader: FAIL values/size wrong at step %d (got %zu elements, expected %d)\n", s,
                       count, (s + 1) * CHUNK);
                rc = 1;
            }
        }
        free(path);
        free(buf);
    }

    touch(DONE_SENTINEL);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return rc;
}

static void
print_report(void)
{
    uint64_t  total_bytes = 0;
    long long min_lat = 0, max_lat = 0, sum_lat = 0;
    long long min_commit = 0, max_commit = 0, sum_commit = 0;
    int       s, n_early = 0, stride = NSTEPS <= 10 ? 1 : NSTEPS / 10;
    long long sum_early_ns = 0;

    /* start-to-receipt: commit_start (before H5Fbegin_step) to the
     * subscriber's H5Fget_subscribed_data() returning. Always >= 0, and the
     * number that actually matters end to end -- unlike measuring from
     * commit_end (see below), it does not depend on where inside the
     * writer's own step machinery the push happens to be issued. */
    printf("\nstep  /series size   start-to-receipt latency   writer commit latency\n");
    for (s = 0; s < NSTEPS; s += stride) {
        long long lat    = g_timing[s].receipt_ns - g_timing[s].commit_start_ns;
        long long commit = g_timing[s].commit_end_ns - g_timing[s].commit_start_ns;

        printf("%4d  %11llu B   %20.3f ms   %14.3f ms\n", s, (unsigned long long)g_timing[s].bytes,
               (double)lat / 1e6, (double)commit / 1e6);
    }

    for (s = 0; s < NSTEPS; s++) {
        long long lat    = g_timing[s].receipt_ns - g_timing[s].commit_start_ns;
        long long commit = g_timing[s].commit_end_ns - g_timing[s].commit_start_ns;
        long long early  = g_timing[s].commit_end_ns - g_timing[s].receipt_ns; /* >0: subscriber had the
                                                                                 * data before H5Fend_step()
                                                                                 * itself returned */

        total_bytes += g_timing[s].bytes;
        if (s == 0 || lat < min_lat)
            min_lat = lat;
        if (lat > max_lat)
            max_lat = lat;
        sum_lat += lat;
        if (s == 0 || commit < min_commit)
            min_commit = commit;
        if (commit > max_commit)
            max_commit = commit;
        sum_commit += commit;
        if (early > 0) {
            n_early++;
            sum_early_ns += early;
        }
    }

    printf("\ntotal bytes streamed (sum of every step's full snapshot): %.2f MiB\n",
           (double)total_bytes / (1024.0 * 1024.0));
    printf("final step alone: %.2f MiB (%.1f%% of the total) -- the O(N^2) cost of re-sending the whole\n"
           "  cumulative array every step, against the O(N) that actually changed since the last one\n",
           (double)g_timing[NSTEPS - 1].bytes / (1024.0 * 1024.0),
           100.0 * (double)g_timing[NSTEPS - 1].bytes / (double)total_bytes);
    printf("start-to-receipt latency (H5Fbegin_step -> subscriber's data in hand), %d steps: min %.3f ms, "
           "max %.3f ms, mean %.3f ms\n",
           NSTEPS, (double)min_lat / 1e6, (double)max_lat / 1e6, (double)sum_lat / NSTEPS / 1e6);
    printf("writer-side commit latency (H5Fbegin_step..H5Fend_step), %d steps: min %.3f ms, max %.3f ms, "
           "mean %.3f ms\n",
           NSTEPS, (double)min_commit / 1e6, (double)max_commit / 1e6, (double)sum_commit / NSTEPS / 1e6);
    if (n_early > 0)
        printf("subscriber had the data before the writer's own H5Fend_step() returned in %d/%d steps "
               "(mean %.3f ms early) -- the push happens mid-replay, not after commit\n",
               n_early, NSTEPS, (double)sum_early_ns / n_early / 1e6);

    {
        long long span_ns = g_timing[NSTEPS - 1].receipt_ns - g_timing[0].commit_start_ns;

        if (span_ns > 0)
            printf("aggregate throughput over the whole run: %.2f MiB/s\n",
                   (double)total_bytes / (1024.0 * 1024.0) / ((double)span_ns / 1e9));
    }
} /* end print_report() */

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status;
    int   nerrors = 0;

    /* na+sm's zero-copy path needs process_vm_readv() cross-memory attach,
     * which a restrictive kernel.yama.ptrace_scope denies for a transfer
     * this size even between fork()-related processes (every other test
     * here only ever moves a scalar, small enough to stay on na+sm's inline
     * path and never trip this) -- not something to work around by relaxing
     * a kernel security policy. ofi+tcp needs no cross-memory attach at
     * all, at the cost of going through a real (loopback) TCP socket. */
    setenv("VOL_STREAM_NA", "ofi+tcp", 0);

    printf("vol-stream benchmark: growing time-series array streamed over Mercury (%s)\n",
           getenv("VOL_STREAM_NA"));
    printf("  %d steps, +%d elements/step, /series reaches %d elements (%.1f MiB) at the final step\n",
           NSTEPS, CHUNK, NSTEPS * CHUNK, (double)NSTEPS * CHUNK * sizeof(int) / (1024.0 * 1024.0));

    unlink(READY_SENTINEL);
    unlink(DONE_SENTINEL);

    g_timing = (step_timing_t *)mmap(NULL, sizeof(step_timing_t) * NSTEPS, PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_timing == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(g_timing, 0, sizeof(step_timing_t) * NSTEPS);

    /* Cleared before the fork, not inside run_writer(): a reader polls for
     * this sidecar to know the writer is up, so one left behind by a previous
     * run in the same directory sends it to join a group that died with that
     * run -- and then to open a file the current writer is about to unlink
     * ("can't retrieve stat info for file"). Same fix, and the same reason,
     * as t_precision_dual.c's own pre-fork cleanup. */
    unlink(FNAME ".vsgroup");

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
    unlink(DONE_SENTINEL);

    if (writer_status != 0) {
        printf("\nwriter process reported failure\n");
        nerrors++;
    }
    if (!WIFEXITED(reader_status) || WEXITSTATUS(reader_status) != 0) {
        printf("\nreader process reported failure\n");
        nerrors++;
    }

    if (!nerrors)
        print_report();

    munmap(g_timing, sizeof(step_timing_t) * NSTEPS);

    if (nerrors) {
        printf("\n%d failure(s) -- benchmark did not complete cleanly\n", nerrors);
        return 1;
    }
    return 0;
}
