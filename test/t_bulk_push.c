/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Phase 1 (RFC sec:bulk-phase1): a large push payload goes by Mercury bulk.
 *
 * One step writes /big, T_BULK_MIB MiB of int64 (256 by default; set 1024 for
 * the RFC's 1 GiB gate), and /small, a few ints. The reader subscribes to both.
 * Asserted:
 *
 *   1. /big arrives whole and every value is right -- the subscriber pulled
 *      it from the writer's registered memory, and the writer kept that
 *      memory alive until the pull completed. Freeing it early is the
 *      phase's real risk; under ASan it is a use-after-free.
 *   2. /small arrives too, inline.
 *   3. Exactly one push went by bulk -- /big above the default threshold,
 *      /small below it -- read from the writer's VOL_STREAM_PUSH_STATS line,
 *      so a regression that sends everything inline (or everything by bulk)
 *      fails rather than passes on correct data alone.
 *
 * Two processes, na+sm, same shape as test/t_subscribe.c.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NSMALL 16
#define FNAME  "t_bulk_push.h5"
#define STATS  "t_bulk_push.stats"

#define READY_SENTINEL "t_bulk_push.reader_ready"
#define DONE_SENTINEL  "t_bulk_push.reader_done"

static size_t
big_elems(void)
{
    const char *e   = getenv("T_BULK_MIB");
    long        mib = e && *e ? atol(e) : 256;

    if (mib <= 0)
        mib = 256;
    return (size_t)mib * 1024 * 1024 / sizeof(int64_t);
}

static int64_t
big_value(size_t i)
{
    return (int64_t)(i * 2654435761u) ^ (int64_t)i;
}

static double
now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int
wait_for(const char *path, int tenths)
{
    int i;

    for (i = 0; i < tenths; i++) {
        if (access(path, F_OK) == 0)
            return 0;
        usleep(100000);
    }
    return -1;
}

static void
touch(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f);
}

static int
run_reader(void)
{
    hid_t   vol_id, fapl, fid, big_space, small_space;
    hsize_t big_dims = big_elems(), small_dims = NSMALL;
    size_t  n        = big_elems(), big_got = 0;
    int     small_ok = 0, rc = 0, i;
    double  t0 = 0, t_first = 0;

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL setup\n");
        return 1;
    }
    if (wait_for(FNAME ".vsgroup", 100) < 0 || (fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open/join\n");
        return 1;
    }
    if ((big_space = H5Screate_simple(1, &big_dims, NULL)) < 0 ||
        (small_space = H5Screate_simple(1, &small_dims, NULL)) < 0) {
        printf("reader: FAIL dataspaces\n");
        return 1;
    }
    {
        const char *paths[2]  = {"/big", "/small"};
        const hid_t spaces[2] = {big_space, small_space};

        if (H5Fsubscribe(fid, 2, paths, spaces, NULL) < 0) {
            printf("reader: FAIL subscribe\n");
            return 1;
        }
    }
    H5Sclose(big_space);
    H5Sclose(small_space);
    touch(READY_SENTINEL);

    t0 = now_s();
    for (i = 0; i < 64 && (big_got < n || !small_ok); i++) {
        uint64_t phys = 0, es = 0, ec = 0;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;

        if (H5Fget_subscribed_data(fid, 120000, &phys, &path, &buf, &size, &es, &ec, NULL) < 0) {
            printf("  FAIL  timed out with %zu of %zu /big elements received\n", big_got, n);
            rc = 1;
            break;
        }
        if (path && strcmp(path, "/small") == 0) {
            const int *v = (const int *)buf;
            int        k;

            small_ok = (size == NSMALL * sizeof(int) && es == 0 && ec == NSMALL);
            for (k = 0; small_ok && k < NSMALL; k++)
                small_ok = (v[k] == 7 * k);
            if (!small_ok) {
                printf("  FAIL  /small arrived wrong (%zu bytes, [%llu, +%llu))\n", size,
                       (unsigned long long)es, (unsigned long long)ec);
                rc = 1;
            }
        }
        else if (path && strcmp(path, "/big") == 0) {
            const int64_t *v = (const int64_t *)buf;
            uint64_t       k;

            if (!t_first)
                t_first = now_s();
            if (size != ec * sizeof(int64_t) || es + ec > n) {
                printf("  FAIL  /big push [%llu, +%llu) carries %zu bytes\n", (unsigned long long)es,
                       (unsigned long long)ec, size);
                rc = 1;
            }
            else {
                for (k = 0; k < ec; k++)
                    if (v[k] != big_value((size_t)(es + k))) {
                        printf("  FAIL  /big[%llu] is wrong -- the pulled bytes are not the ones written\n",
                               (unsigned long long)(es + k));
                        rc = 1;
                        break;
                    }
                big_got += (size_t)ec;
            }
        }
        free(path);
        free(buf);
        if (rc)
            break;
    }

    if (!rc && big_got == n && small_ok)
        printf("  ok    /big (%zu MiB) arrived intact, and /small with it (%.2f s from subscribe to "
               "last byte)\n",
               n * sizeof(int64_t) / (1024 * 1024), now_s() - t0);
    else if (!rc) {
        printf("  FAIL  received %zu of %zu /big elements, /small %s\n", big_got, n,
               small_ok ? "ok" : "missing");
        rc = 1;
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    touch(DONE_SENTINEL);
    return rc;
}

static int
run_writer(void)
{
    hid_t    vol_id, fapl, fid, big_space, small_space, big_ds, small_ds;
    hsize_t  big_dims = big_elems(), small_dims = NSMALL;
    size_t   n        = big_elems(), i;
    int64_t *big;
    int      small[NSMALL];

    /* The push statistics are printed to stderr when the transport stops;
     * keep them for main() to read. */
    if (!freopen(STATS, "w", stderr)) {
        printf("writer: FAIL redirect stderr\n");
        return 1;
    }
    if (NULL == (big = (int64_t *)malloc(n * sizeof(int64_t)))) {
        printf("writer: FAIL allocate %zu MiB\n", n * sizeof(int64_t) / (1024 * 1024));
        return 1;
    }
    for (i = 0; i < n; i++)
        big[i] = big_value(i);
    for (i = 0; i < NSMALL; i++)
        small[i] = 7 * (int)i;

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0 ||
        (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("writer: FAIL setup (transport up?)\n");
        return 1;
    }
    if (wait_for(READY_SENTINEL, 300) < 0) {
        printf("writer: FAIL reader never subscribed\n");
        return 1;
    }
    if ((big_space = H5Screate_simple(1, &big_dims, NULL)) < 0 ||
        (small_space = H5Screate_simple(1, &small_dims, NULL)) < 0 || H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (big_ds = H5Dcreate2(fid, "/big", H5T_NATIVE_INT64, big_space, H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT)) < 0 ||
        H5Dwrite(big_ds, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, H5P_DEFAULT, big) < 0 ||
        (small_ds = H5Dcreate2(fid, "/small", H5T_NATIVE_INT, small_space, H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT)) < 0 ||
        H5Dwrite(small_ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, small) < 0) {
        printf("writer: FAIL write\n");
        return 1;
    }
    /* The caller's own buffer is released as soon as the write returns --
     * the connector staged its own copy, and that copy is what a bulk push
     * pulls from. */
    free(big);
    if (H5Fend_step(fid) < 0) {
        printf("writer: FAIL end_step\n");
        return 1;
    }
    H5Dclose(big_ds);
    H5Dclose(small_ds);
    H5Sclose(big_space);
    H5Sclose(small_space);

    wait_for(DONE_SENTINEL, 1800);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    fflush(stderr);
    return 0;
}

/* The "N via bulk" count from the writer's push statistics, or -1. */
static long
bulk_pushes(void)
{
    char  line[1024];
    long  n = -1;
    FILE *f = fopen(STATS, "r");

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, " via bulk");

        if (p) {
            while (p > line && p[-1] >= '0' && p[-1] <= '9')
                p--;
            n = atol(p);
        }
    }
    fclose(f);
    return n;
}

static void
clean(void)
{
    unlink(READY_SENTINEL);
    unlink(DONE_SENTINEL);
    unlink(STATS);
    unlink(FNAME ".vsgroup");
    unlink(FNAME);
}

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status, nerrors = 0;
    long  nbulk;

    printf("vol-stream Phase 1: a large push goes by Mercury bulk (na+sm)\n");
    setenv("VOL_STREAM_NA", "na+sm", 0);
    unsetenv("VOL_STREAM_BULK_THRESHOLD"); /* the default is what is under test */
    clean();

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
    setenv("VOL_STREAM_PUSH_STATS", "1", 1); /* writer only: the reader forked before this */
    writer_status = run_writer();
    if (waitpid(pid, &reader_status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    nbulk = bulk_pushes();
    if (nbulk == 1)
        printf("  ok    exactly one push went by bulk: /big above the threshold, /small inline below it\n");
    else {
        printf("  FAIL  %ld push(es) went by bulk, expected exactly 1 (/big only)\n", nbulk);
        nerrors++;
    }
    clean();

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
