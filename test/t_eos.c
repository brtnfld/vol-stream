/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * End of stream, as a subscriber sees it.
 *
 * A subscriber cannot otherwise tell a writer that has finished from one that
 * has paused: both just stop announcing steps. The reader side now treats the
 * writer leaving the group as the end of the stream. This pins what that
 * means:
 *
 *   1. While the writer is in the group, H5Fstep_status() never reports
 *      H5F_STEP_EOS, however idle the stream is.
 *   2. Steps the writer announced before leaving are still delivered after it
 *      has gone, and EOS is not reported while any of them is unconsumed.
 *   3. Once the last one is consumed, H5Fstep_status() reports H5F_STEP_EOS,
 *      and H5Fwait_step_ready() and H5Fget_subscribed_data() return at once
 *      rather than waiting out their timeouts.
 *
 * Two processes, na+sm. The writer is the one that leaves first here, the
 * opposite of every other two-process test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define N     8
#define FNAME "t_eos.h5"

#define READY_SENTINEL    "t_eos.reader_ready"
#define WROTE_SENTINEL    "t_eos.wrote"
#define BLOCKING_SENTINEL "t_eos.blocking"

static double
now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int
wait_for_sentinel(const char *path, int max_iters)
{
    int i;

    for (i = 0; i < max_iters; i++) {
        if (access(path, F_OK) == 0)
            return 0;
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
is_eos(hid_t fid)
{
    H5F_step_status_t st = H5F_STEP_NOT_IN_STEP;

    return H5Fstep_status(fid, &st) >= 0 && st == H5F_STEP_EOS;
}

static int
run_reader(void)
{
    hid_t    vol_id, fapl, fid, space;
    hsize_t  dims = N;
    uint64_t phys = 0, wall = 0;
    double   t0;
    int      s, i, rc = 0;

#define CHECK(cond, ...)                                                                                      \
    do {                                                                                                      \
        if (cond)                                                                                             \
            printf("  ok    " __VA_ARGS__);                                                                   \
        else {                                                                                                \
            printf("  FAIL  " __VA_ARGS__);                                                                   \
            rc = 1;                                                                                           \
        }                                                                                                     \
        printf("\n");                                                                                         \
    } while (0)

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL setup\n");
        return 1;
    }
    for (i = 0; i < 100 && access(FNAME ".vsgroup", F_OK) != 0; i++)
        usleep(100000);
    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open/join\n");
        return 1;
    }
    if ((space = H5Screate_simple(1, &dims, NULL)) < 0) {
        printf("reader: FAIL dataspace\n");
        return 1;
    }
    {
        const char *paths[1]  = {"/x"};
        const hid_t spaces[1] = {space};

        if (H5Fsubscribe(fid, 1, paths, spaces, NULL) < 0) {
            printf("reader: FAIL subscribe\n");
            return 1;
        }
    }
    H5Sclose(space);
    touch_sentinel(READY_SENTINEL);

    if (wait_for_sentinel(WROTE_SENTINEL, 300) < 0) {
        printf("reader: FAIL writer never wrote\n");
        return 1;
    }
    CHECK(!is_eos(fid), "no EOS while the writer is in the group");
    for (s = 0; s < 3; s++)
        CHECK(H5Fwait_step_ready(fid, 5000, &phys, &wall) >= 0, "step %d announced", s);
    CHECK(!is_eos(fid), "no EOS with every announced step consumed but the writer still in the group");

    /* The writer now waits a second, commits two more steps, and closes. */
    touch_sentinel(BLOCKING_SENTINEL);
    CHECK(H5Fwait_step_ready(fid, 20000, &phys, &wall) >= 0, "step 3 announced while blocked");

    /* Let the writer finish and leave. Step 4 was announced before it left. */
    sleep(2);
    CHECK(!is_eos(fid), "no EOS while an announced step is still unconsumed");
    CHECK(H5Fwait_step_ready(fid, 5000, &phys, &wall) >= 0, "step 4 delivered after the writer left");

    /* The departure reaches the reader through the group's membership
     * updates; allow for that rather than assume it is already here. */
    t0 = now_s();
    while (!is_eos(fid) && now_s() - t0 < 10)
        usleep(50000);
    CHECK(is_eos(fid), "H5Fstep_status() reports EOS once the last step is consumed (%.2f s)", now_s() - t0);

    t0 = now_s();
    CHECK(H5Fwait_step_ready(fid, 10000, &phys, &wall) < 0 && now_s() - t0 < 1.0,
          "H5Fwait_step_ready() returns at once at EOS (%.3f s)", now_s() - t0);

    /* Drain what was pushed, then the data wait must not block either. */
    for (;;) {
        uint64_t p, es, ec;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;

        if (H5Fget_subscribed_data(fid, 0, &p, &path, &buf, &size, &es, &ec) < 0)
            break;
        free(path);
        free(buf);
    }
    t0 = now_s();
    {
        uint64_t p, es, ec;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;

        CHECK(H5Fget_subscribed_data(fid, 10000, &p, &path, &buf, &size, &es, &ec) < 0 &&
                  now_s() - t0 < 1.0,
              "H5Fget_subscribed_data() returns at once at EOS (%.3f s)", now_s() - t0);
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return rc;
#undef CHECK
}

static int
write_step(hid_t fid, hid_t space, hid_t *ds, int s)
{
    int vals[N], i;

    for (i = 0; i < N; i++)
        vals[i] = s * 100 + i;
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (s == 0 &&
         (*ds = H5Dcreate2(fid, "/x", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) ||
        H5Dwrite(*ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
        printf("writer: FAIL step %d\n", s);
        return -1;
    }
    return 0;
}

static int
run_writer(void)
{
    hid_t   vol_id, fapl, fid, space, ds = H5I_INVALID_HID;
    hsize_t dims = N;
    int     s;

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0 ||
        (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (space = H5Screate_simple(1, &dims, NULL)) < 0) {
        printf("writer: FAIL setup (transport up? VOL_STREAM_NA set?)\n");
        return 1;
    }
    if (wait_for_sentinel(READY_SENTINEL, 300) < 0) {
        printf("writer: FAIL reader never subscribed\n");
        return 1;
    }
    for (s = 0; s < 3; s++)
        if (write_step(fid, space, &ds, s) < 0)
            return 1;
    touch_sentinel(WROTE_SENTINEL);

    if (wait_for_sentinel(BLOCKING_SENTINEL, 300) < 0) {
        printf("writer: FAIL reader never blocked\n");
        return 1;
    }
    sleep(1);
    for (s = 3; s < 5; s++)
        if (write_step(fid, space, &ds, s) < 0)
            return 1;

    /* Leave first: that departure is what the reader is waiting to see. */
    H5Dclose(ds);
    H5Sclose(space);
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

    printf("vol-stream: end of stream when the writer leaves (na+sm)\n");

    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(READY_SENTINEL);
    unlink(WROTE_SENTINEL);
    unlink(BLOCKING_SENTINEL);
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
    unlink(WROTE_SENTINEL);
    unlink(BLOCKING_SENTINEL);

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
