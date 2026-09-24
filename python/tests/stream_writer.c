/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The live writer the Python tests (test_stream.py, test_lifecycle.py) follow. It writes
 * "/grid", ROWS x COLS ints, one whole-dataset write per step, with the value
 * at (r, c) in step s equal to s*10000 + r*100 + c.
 *
 * Every mode first commits step 0 before any reader has subscribed, so the
 * reader always attaches late: step 0 is announced to it but carries nothing.
 *
 *   column     then steps 1..LOCKSTEP one at a time, waiting for "ack.<s>"
 *              after each, then steps LOCKSTEP+1..LAST back to back.
 *   narrowing  then steps 1..3 back to back.
 *   lifecycle  then steps 1..5 at 100ms intervals; then, once "gone" says the
 *              reader's process has exited, steps 6..15, printing the slowest
 *              of those commits as "max_commit_ms <ms>". A reader that left
 *              cleanly costs nothing; one that did not costs about a second a
 *              step in push timeouts until the transport declares it dead.
 *   idle       then nothing until "go" (write step 1) or "done".
 *   eos        then steps 1..3 back to back, and closes at once without
 *              waiting for "done": the reader sees the writer leave.
 *   block      then sets the Block queue policy with one slot of slack and
 *              commits steps 1..6 back to back, printing how long that took
 *              as "writer_ms <ms>". A reader that acks makes it wait.
 *
 * Synchronization is by empty files in <syncdir>: the writer touches
 * "committed" after step 0 and "writes_done" after its last step, and waits
 * for "ready" (the reader has subscribed) and "done" (the reader has closed).
 *
 * usage: stream_writer <column|narrowing|lifecycle|idle|eos|block> <file> <syncdir>
 */

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define ROWS     6
#define COLS     8
#define LOCKSTEP 3
#define LAST     7

static char g_syncdir[512];

static void
sync_path(char *out, size_t len, const char *name)
{
    snprintf(out, len, "%s/%s", g_syncdir, name);
}

static void
touch(const char *name)
{
    char  p[600];
    FILE *f;

    sync_path(p, sizeof(p), name);
    if ((f = fopen(p, "w")))
        fclose(f);
}

static int
wait_for(const char *name, int seconds)
{
    char p[600];
    int  i;

    sync_path(p, sizeof(p), name);
    for (i = 0; i < seconds * 10; i++) {
        if (access(p, F_OK) == 0)
            return 0;
        usleep(100000);
    }
    fprintf(stderr, "stream_writer: gave up waiting for %s\n", name);
    return -1;
}

static int
exists(const char *name)
{
    char p[600];

    sync_path(p, sizeof(p), name);
    return access(p, F_OK) == 0;
}

static double
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int
write_step(hid_t fid, hid_t space, hid_t *ds, int s)
{
    int vals[ROWS * COLS];
    int r, c;

    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++)
            vals[r * COLS + c] = s * 10000 + r * 100 + c;

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (s == 0 &&
         (*ds = H5Dcreate2(fid, "/grid", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) ||
        H5Dwrite(*ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
        fprintf(stderr, "stream_writer: FAIL step %d\n", s);
        return -1;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    hid_t   vol_id, fapl, fid, space, ds = H5I_INVALID_HID;
    hsize_t dims[2] = {ROWS, COLS};
    const char *mode;
    int         s;

    if (argc != 4 || (strcmp(argv[1], "column") != 0 && strcmp(argv[1], "narrowing") != 0 &&
                      strcmp(argv[1], "lifecycle") != 0 && strcmp(argv[1], "idle") != 0 &&
                      strcmp(argv[1], "eos") != 0 && strcmp(argv[1], "block") != 0)) {
        fprintf(stderr, "usage: %s <column|narrowing|lifecycle|idle|eos|block> <file> <syncdir>\n", argv[0]);
        return 2;
    }
    mode = argv[1];
    snprintf(g_syncdir, sizeof(g_syncdir), "%s", argv[3]);
    setenv("VOL_STREAM_NA", "na+sm", 0);

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0 ||
        (fid = H5Fcreate(argv[2], H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (space = H5Screate_simple(2, dims, NULL)) < 0) {
        fprintf(stderr, "stream_writer: FAIL setup (transport up?)\n");
        return 1;
    }

    if (write_step(fid, space, &ds, 0) < 0)
        return 1;
    touch("committed");
    if (wait_for("ready", 60) < 0)
        return 1;

    if (!strcmp(mode, "column")) {
        for (s = 1; s <= LAST; s++) {
            char ack[32];

            if (write_step(fid, space, &ds, s) < 0)
                return 1;
            if (s <= LOCKSTEP) {
                snprintf(ack, sizeof(ack), "ack.%d", s);
                if (wait_for(ack, 30) < 0)
                    return 1;
            }
        }
    }
    else if (!strcmp(mode, "narrowing") || !strcmp(mode, "eos")) {
        for (s = 1; s <= 3; s++)
            if (write_step(fid, space, &ds, s) < 0)
                return 1;
    }
    else if (!strcmp(mode, "lifecycle")) {
        double worst = 0;

        for (s = 1; s <= 5; s++) {
            if (write_step(fid, space, &ds, s) < 0)
                return 1;
            usleep(100000);
        }
        if (wait_for("gone", 60) < 0)
            return 1;
        for (s = 6; s <= 15; s++) {
            double t0 = now_ms(), dt;

            if (write_step(fid, space, &ds, s) < 0)
                return 1;
            if ((dt = now_ms() - t0) > worst)
                worst = dt;
            usleep(50000);
        }
        printf("max_commit_ms %.1f\n", worst);
        fflush(stdout);
    }
    else if (!strcmp(mode, "block")) {
        double t0;

        if (H5Fset_stream_queue_policy(fid, H5VL_STREAM_QUEUE_BLOCK, 1) < 0) {
            fprintf(stderr, "stream_writer: FAIL set queue policy\n");
            return 1;
        }
        t0 = now_ms();
        for (s = 1; s <= 6; s++)
            if (write_step(fid, space, &ds, s) < 0)
                return 1;
        printf("writer_ms %.1f\n", now_ms() - t0);
        fflush(stdout);
    }
    else { /* idle */
        while (!exists("go") && !exists("done"))
            usleep(50000);
        if (exists("go") && write_step(fid, space, &ds, 1) < 0)
            return 1;
    }
    touch("writes_done");

    /* Outlive the reader so its departure comes before the group goes away --
     * except in eos mode, where the writer leaving first is the point. */
    if (strcmp(mode, "eos") != 0)
        wait_for("done", 60);

    H5Dclose(ds);
    H5Sclose(space);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}
