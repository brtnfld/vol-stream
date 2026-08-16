/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Benchmark, not a correctness test: how the writer's own per-step cost
 * scales with the NUMBER OF SUBSCRIBERS.
 *
 * b_stream_grow.c and b_stream_grow_tail.c both measure one writer against
 * exactly one subscriber, which is the case where the data push matters
 * least -- one RPC per step, whose round trip the writer either waits on or
 * does not. The interesting question for the async push is what happens at
 * fan-out, because the writer issues one push per (subscriber, selection
 * run, predicate run, chunk):
 *
 *   - Waiting on each push inline, a step costs the SUM of those round
 *     trips. Every additional subscriber adds its own latency to every
 *     H5Fend_step(), whether or not the writer has anything else to do.
 *   - Issuing them all and settling the set once before the step is
 *     announced (vs_tr_drain_pushes()), a step costs roughly the SLOWEST
 *     round trip plus the cost of issuing each -- so the per-subscriber term
 *     drops to serialization/issue cost rather than a full network round
 *     trip.
 *
 * So this reports writer-side commit latency (H5Fbegin_step..H5Fend_step)
 * against subscriber count, one run per count, in a single process tree.
 * Each subscriber is its own OS process with its own HDF5 library state,
 * joining the writer's SSG group and subscribing to the whole object, and
 * every one of them verifies the values it receives -- a fan-out
 * measurement that silently dropped pushes would otherwise look like a
 * spectacular speedup.
 *
 * Only compiled/run when VOL_STREAM_HAVE_MERCURY is on; see
 * test/CMakeLists.txt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NSTEPS    30
#define CHUNK     8192 /* elements written per step; 4 bytes each */
#define MAX_SUBS  8

/* Subscriber counts measured, in order. Powers of two so a per-subscriber
 * round-trip term (linear) is easy to tell apart from a fixed one. */
static const int g_sub_counts[] = {1, 2, 4, 8};
#define N_SUB_COUNTS ((int)(sizeof(g_sub_counts) / sizeof(g_sub_counts[0])))

static char g_fname[64];
static char g_group_file[80];
static char g_ready_sentinel[MAX_SUBS][64];
static char g_done_sentinel[MAX_SUBS][64];

/* mmap'd MAP_SHARED so the writer child's timings survive into the parent. */
typedef struct {
    long long commit_ns[NSTEPS]; /* writer: H5Fbegin_step..H5Fend_step, per step */
    int       reader_errors;     /* readers: nonzero if any value check failed */
} shared_t;

static shared_t *g_shared;

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

static void
set_names(int nsubs)
{
    int i;

    /* A distinct file (and so a distinct group sidecar) per subscriber
     * count, so one run's leftover SSG state cannot bleed into the next. */
    snprintf(g_fname, sizeof(g_fname), "b_push_fanout_%d.h5", nsubs);
    snprintf(g_group_file, sizeof(g_group_file), "%s.vsgroup", g_fname);
    for (i = 0; i < nsubs; i++) {
        snprintf(g_ready_sentinel[i], sizeof(g_ready_sentinel[i]), "b_push_fanout_%d.ready.%d", nsubs, i);
        snprintf(g_done_sentinel[i], sizeof(g_done_sentinel[i]), "b_push_fanout_%d.done.%d", nsubs, i);
    }
}

static void
clean_names(int nsubs)
{
    int i;

    unlink(g_fname);
    unlink(g_group_file);
    for (i = 0; i < nsubs; i++) {
        unlink(g_ready_sentinel[i]);
        unlink(g_done_sentinel[i]);
    }
}

static int
run_writer(int nsubs)
{
    hid_t vol_id, fapl, fid, space, dcpl, ds = -1;
    int   s, i, *vals;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("writer: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("writer: FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(g_fname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("writer: FAIL create (transport up? VOL_STREAM_NA set?)\n");
        return 1;
    }

    /* Every subscriber must be subscribed before the first step: a
     * subscription is not applied retroactively to steps already committed,
     * so a late one would simply receive nothing and make the writer look
     * artificially fast. */
    for (i = 0; i < nsubs; i++)
        if (wait_for(g_ready_sentinel[i], 500) < 0)
            printf("writer: WARNING subscriber %d never signaled ready; proceeding anyway\n", i);

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
        hsize_t   newdims = (hsize_t)(s + 1) * CHUNK;
        hsize_t   start = (hsize_t)s * CHUNK, count = CHUNK;
        hid_t     fspace, mspace;
        long long t0;

        for (i = 0; i < CHUNK; i++)
            vals[i] = s * CHUNK + i;

        t0 = now_ns();

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            printf("writer: FAIL begin step %d\n", s);
            free(vals);
            return 1;
        }

        /* Same tail-only pattern as b_stream_grow_tail.c: one persistent
         * handle, extended each step, only the new slice written -- so the
         * pushed payload is a constant CHUNK elements per step and the only
         * thing varying across runs here is the subscriber count. */
        if (s == 0) {
            if ((ds = H5Dcreate2(fid, "/series", H5T_NATIVE_INT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0) {
                printf("writer: FAIL create dataset\n");
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

        g_shared->commit_ns[s] = now_ns() - t0;
    }

    H5Dclose(ds);
    H5Sclose(space);
    H5Pclose(dcpl);
    free(vals);

    /* Subscribers must finish and close while this writer's group is still
     * alive; leaving first would leave them waiting on a group that is
     * being torn down. */
    for (i = 0; i < nsubs; i++)
        if (wait_for(g_done_sentinel[i], 500) < 0)
            printf("writer: WARNING subscriber %d never signaled done\n", i);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}

static int
run_reader(int idx)
{
    hid_t vol_id, fapl, fid;
    int   s, rc = 0;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("reader %d: FAIL register\n", idx);
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader %d: FAIL fapl\n", idx);
        return 1;
    }

    if (wait_for(g_group_file, 500) < 0) {
        printf("reader %d: FAIL writer's group sidecar never appeared\n", idx);
        return 1;
    }
    if ((fid = H5Fopen(g_fname, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader %d: FAIL open\n", idx);
        return 1;
    }

    {
        hid_t       sub_space;
        hsize_t     sub_dims = (hsize_t)NSTEPS * CHUNK; /* generous upper bound */
        const char *paths[1] = {"/series"};

        if ((sub_space = H5Screate_simple(1, &sub_dims, NULL)) < 0) {
            printf("reader %d: FAIL subscription dataspace\n", idx);
            return 1;
        }
        {
            const hid_t spaces[1] = {sub_space};

            if (H5Fsubscribe(fid, 1, paths, spaces, NULL) < 0) {
                printf("reader %d: FAIL subscribe\n", idx);
                H5Sclose(sub_space);
                return 1;
            }
        }
        H5Sclose(sub_space);
    }

    touch(g_ready_sentinel[idx]);

    for (s = 0; s < NSTEPS; s++) {
        uint64_t phys = (uint64_t)-1, elem_start = 0, elem_count = 0;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;

        if (H5Fget_subscribed_data(fid, 20000 /* 20s */, &phys, &path, &buf, &size, &elem_start,
                                    &elem_count) < 0) {
            printf("reader %d: FAIL subscribed data at step %d (timed out)\n", idx, s);
            rc = 1;
            break;
        }

        /* Every subscriber checks every step: fan-out that silently dropped
         * pushes would otherwise read as a large speedup. */
        {
            const int *ints    = (const int *)buf;
            size_t     count   = size / sizeof(int);
            int        expect0 = s * CHUNK;

            if (count != (size_t)CHUNK || ints[0] != expect0 || ints[count - 1] != expect0 + CHUNK - 1) {
                printf("reader %d: FAIL values/size wrong at step %d (got %zu elements starting at %d)\n", idx,
                       s, count, count ? ints[0] : -1);
                rc = 1;
            }
        }
        free(path);
        free(buf);
    }

    if (rc)
        g_shared->reader_errors++;

    touch(g_done_sentinel[idx]);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return rc;
}

/* Mean writer commit latency in ms for one subscriber count, or -1 on
 * failure. */
static double
run_one(int nsubs)
{
    pid_t readers[MAX_SUBS];
    pid_t writer_pid;
    int   i, status, failed = 0;

    set_names(nsubs);
    clean_names(nsubs);
    memset(g_shared, 0, sizeof(*g_shared));

    /* Drain stdio before forking: a buffered, not-yet-written parent line
     * would otherwise be duplicated by every child that inherits it. */
    fflush(NULL);

    for (i = 0; i < nsubs; i++) {
        if ((readers[i] = fork()) < 0) {
            perror("fork");
            return -1;
        }
        if (readers[i] == 0) {
            int rc = run_reader(i);

            fflush(NULL);
            _exit(rc);
        }
    }

    /* The writer runs in its own child too, so each run gets a clean HDF5/
     * transport state rather than re-registering the VOL in one long-lived
     * parent across four runs. */
    if ((writer_pid = fork()) < 0) {
        perror("fork");
        return -1;
    }
    if (writer_pid == 0) {
        int rc = run_writer(nsubs);

        fflush(NULL);
        _exit(rc);
    }

    if (waitpid(writer_pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        failed = 1;
    for (i = 0; i < nsubs; i++)
        if (waitpid(readers[i], &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            failed = 1;

    clean_names(nsubs);

    if (failed || g_shared->reader_errors)
        return -1;

    {
        long long sum = 0;
        int       s;

        for (s = 0; s < NSTEPS; s++)
            sum += g_shared->commit_ns[s];
        return (double)sum / NSTEPS / 1e6;
    }
}

int
main(void)
{
    double results[N_SUB_COUNTS];
    int    c, nerrors = 0;

    setenv("VOL_STREAM_NA", "ofi+tcp", 0);

    printf("vol-stream benchmark: writer-side cost vs SUBSCRIBER COUNT (%s)\n", getenv("VOL_STREAM_NA"));
    printf("  %d steps, %d elements (%zu KiB) pushed per step per subscriber\n\n", NSTEPS, CHUNK,
           (size_t)CHUNK * sizeof(int) / 1024);

    g_shared = (shared_t *)mmap(NULL, sizeof(shared_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                                 -1, 0);
    if (g_shared == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    for (c = 0; c < N_SUB_COUNTS; c++) {
        /* Let the previous run's group fully dissolve before standing up the
         * next one. Without this the runs are flaky: ofi+tcp reuses ports,
         * so a SWIM ping still in flight to a just-departed member lands on
         * the next run's process at the same address and SSG rejects it
         * ("dping req recv error: group ... not found"), which can cascade
         * into a failed subscribe. Same class of teardown race the settle
         * window in vs_tr_stop() exists for, just across whole runs. */
        if (c > 0)
            usleep(1500000);

        results[c] = run_one(g_sub_counts[c]);
        if (results[c] < 0) {
            printf("run with %d subscriber(s) FAILED\n", g_sub_counts[c]);
            nerrors++;
        }
    }

    if (!nerrors) {
        printf("subscribers   mean writer commit latency   vs 1 subscriber   added per subscriber\n");
        for (c = 0; c < N_SUB_COUNTS; c++)
            printf("%11d   %22.3f ms   %15.2fx   %18.3f ms\n", g_sub_counts[c], results[c],
                   results[c] / results[0],
                   g_sub_counts[c] > 1 ? (results[c] - results[0]) / (g_sub_counts[c] - 1) : 0.0);
        printf("\n\"added per subscriber\" is the slope that matters: with the push waited on inline it is a\n"
               "full round trip, and with it issued asynchronously and settled once per step it is only the\n"
               "cost of issuing one more RPC.\n");
    }

    munmap(g_shared, sizeof(shared_t));

    if (nerrors) {
        printf("\n%d failure(s) -- benchmark did not complete cleanly\n", nerrors);
        return 1;
    }
    return 0;
}
