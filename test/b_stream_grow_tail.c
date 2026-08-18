/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Benchmark, not a correctness test: b_stream_grow.c's same growing
 * time-series array, but through ONE persistent handle -- H5Dset_extent()
 * plus writing only the NEW tail slice each step (test/t_dset_resize.c's
 * case 3), not a full rewrite. The O(N) counterpart to that file's O(N^2)
 * measurement, now that H5VL__stream_carry_forward_resized() makes a
 * tail-only write produce a complete snapshot at every step instead of a
 * fill-value gap (see docs/dev-plan.md).
 *
 * Same two-process shape, same reporting structure as b_stream_grow.c;
 * see that file's own comment for what is not repeated here. The one real
 * difference beyond the write pattern: the reader's subscription push is
 * now only ever the new tail (CHUNK elements), not the whole cumulative
 * array, since the write itself only ever covers the tail -- the
 * subscription protocol pushes what the write's own selection overlaps,
 * and there is no more-than-the-write to push once the write itself is
 * O(N) per step.
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

#define FNAME          "b_stream_grow_tail.h5"
#define READY_SENTINEL "b_stream_grow_tail.reader_ready"
#define DONE_SENTINEL  "b_stream_grow_tail.reader_done"

#define NSTEPS 50
#define CHUNK  8192 /* elements added per step; 4 bytes each */

typedef struct {
    long long commit_start_ns; /* writer: right before H5Fbegin_step() */
    long long commit_end_ns;   /* writer: right after H5Fend_step() returns */
    long long receipt_ns;      /* reader: right after H5Fget_subscribed_data() returns */
    uint64_t  bytes;           /* reader: this step's pushed size in bytes */
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
    hid_t vol_id, fapl, fid, space, dcpl, ds = -1;
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

    if (NULL == (vals = (int *)malloc((size_t)CHUNK * sizeof(int)))) {
        printf("writer: FAIL alloc\n");
        return 1;
    }

    {
        hsize_t dims = CHUNK, maxdims = H5S_UNLIMITED, chunk = CHUNK;

        if ((space = H5Screate_simple(1, &dims, &maxdims)) < 0 || (dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
            H5Pset_chunk(dcpl, 1, &chunk) < 0) {
            printf("writer: FAIL space/dcpl\n");
            free(vals);
            return 1;
        }
    }

    for (s = 0; s < NSTEPS; s++) {
        hsize_t newdims = (hsize_t)(s + 1) * CHUNK;
        hsize_t start = (hsize_t)s * CHUNK, count = CHUNK;
        hid_t   fspace, mspace;
        int     i;

        for (i = 0; i < CHUNK; i++)
            vals[i] = (int)start + i;

        g_timing[s].commit_start_ns = now_ns();

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            printf("writer: FAIL begin_step %d\n", s);
            free(vals);
            return 1;
        }
        if (s == 0) {
            if ((ds = H5Dcreate2(fid, "/series", H5T_NATIVE_INT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) <
                0) {
                printf("writer: FAIL create step 0\n");
                free(vals);
                return 1;
            }
        }
        else if (H5Dset_extent(ds, &newdims) < 0) {
            printf("writer: FAIL set_extent step %d\n", s);
            free(vals);
            return 1;
        }

        if ((fspace = H5Dget_space(ds)) < 0 ||
            H5Sselect_hyperslab(fspace, H5S_SELECT_SET, &start, NULL, &count, NULL) < 0) {
            printf("writer: FAIL hyperslab step %d\n", s);
            free(vals);
            return 1;
        }
        if ((mspace = H5Screate_simple(1, &count, NULL)) < 0) {
            printf("writer: FAIL memspace step %d\n", s);
            free(vals);
            return 1;
        }
        if (H5Dwrite(ds, H5T_NATIVE_INT, mspace, fspace, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
            printf("writer: FAIL write/end step %d\n", s);
            free(vals);
            return 1;
        }
        H5Sclose(mspace);
        H5Sclose(fspace);

        g_timing[s].commit_end_ns = now_ns();
    }

    H5Dclose(ds);
    H5Sclose(space);
    H5Pclose(dcpl);
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

    /* See b_stream_grow.c's own comment for why subscription, not
     * begin_step, is the only way to observe live data here. */
    {
        hid_t   sub_space;
        hsize_t sub_dims = (hsize_t)NSTEPS * CHUNK; /* generous upper bound */
        const char *paths[1]  = {"/series"};

        if ((sub_space = H5Screate_simple(1, &sub_dims, NULL)) < 0) {
            printf("reader: FAIL subscription dataspace\n");
            return 1;
        }
        {
            const hid_t spaces[1] = {sub_space};

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
            /* Unlike b_stream_grow.c: the write itself only ever covers the
             * new tail, so that is all a subscriber to the whole object
             * ever gets pushed -- CHUNK elements, not the whole cumulative
             * array. This is the O(N) win itself, not a test artifact. */
            const int *ints    = (const int *)buf;
            size_t     count   = size / sizeof(int);
            int        expect0 = s * CHUNK;

            if (count != (size_t)CHUNK || ints[0] != expect0 || ints[count - 1] != expect0 + CHUNK - 1) {
                printf("reader: FAIL values/size wrong at step %d (got %zu elements starting at %d)\n", s,
                       count, count ? ints[0] : -1);
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
    int       s, stride = NSTEPS <= 10 ? 1 : NSTEPS / 10;

    printf("\nstep  pushed size   start-to-receipt latency   writer commit latency\n");
    for (s = 0; s < NSTEPS; s += stride) {
        long long lat    = g_timing[s].receipt_ns - g_timing[s].commit_start_ns;
        long long commit = g_timing[s].commit_end_ns - g_timing[s].commit_start_ns;

        printf("%4d  %10llu B   %20.3f ms   %14.3f ms\n", s, (unsigned long long)g_timing[s].bytes,
               (double)lat / 1e6, (double)commit / 1e6);
    }

    for (s = 0; s < NSTEPS; s++) {
        long long lat    = g_timing[s].receipt_ns - g_timing[s].commit_start_ns;
        long long commit = g_timing[s].commit_end_ns - g_timing[s].commit_start_ns;

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
    }

    printf("\ntotal bytes streamed (sum of every step's OWN new tail, not a snapshot): %.2f MiB -- O(N), "
           "not the O(N^2) b_stream_grow.c measures for the same growth\n",
           (double)total_bytes / (1024.0 * 1024.0));
    printf("start-to-receipt latency (H5Fbegin_step -> subscriber's data in hand), %d steps: min %.3f ms, "
           "max %.3f ms, mean %.3f ms\n",
           NSTEPS, (double)min_lat / 1e6, (double)max_lat / 1e6, (double)sum_lat / NSTEPS / 1e6);
    printf("writer-side commit latency (H5Fbegin_step..H5Fend_step), %d steps: min %.3f ms, max %.3f ms, "
           "mean %.3f ms -- roughly CONSTANT across steps, unlike b_stream_grow.c's growing cost\n",
           NSTEPS, (double)min_commit / 1e6, (double)max_commit / 1e6, (double)sum_commit / NSTEPS / 1e6);

    {
        long long span_ns = g_timing[NSTEPS - 1].receipt_ns - g_timing[0].commit_start_ns;

        if (span_ns > 0)
            printf("total wall time for the whole %d-step run: %.2f ms (the metric that matters for "
                   "comparing against b_stream_grow.c's O(N^2) run -- aggregate MiB/s is the wrong lens "
                   "here, since total bytes differs by design)\n",
                   NSTEPS, (double)span_ns / 1e6);
    }
} /* end print_report() */

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status;
    int   nerrors = 0;

    /* See b_stream_grow.c's own comment for why ofi+tcp, not na+sm. */
    setenv("VOL_STREAM_NA", "ofi+tcp", 0);

    printf("vol-stream benchmark: growing time-series array, TAIL-ONLY writes, streamed over Mercury "
           "(%s)\n",
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
