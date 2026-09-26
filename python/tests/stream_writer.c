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
 *   grow       writes "/series" instead of "/grid": an unlimited dataset of
 *              GCOLS ints per row, one row in step 0, then steps 1..3 each
 *              extend it by a row and write only that row (value r*100+c),
 *              the tail-only pattern of test/b_push_fanout.c.
 *   types      writes "/rec" instead of "/grid": NREC records of
 *              {int a; double b; char c[4]}, and a scalar double attribute
 *              "/rec@scale", both rewritten every step through handles kept
 *              open across steps. In step s, record i is {s*10+i, s+i*0.5,
 *              "<s><i>"} and scale is s*0.25. Steps 1..2 follow step 0.
 *   block      then sets the Block queue policy with one slot of slack and
 *              commits steps 1..6 back to back, printing how long that took
 *              as "writer_ms <ms>". A reader that acks makes it wait.
 *
 * Synchronization is by empty files in <syncdir>: the writer touches
 * "committed" after step 0 and "writes_done" after its last step, and waits
 * for "ready" (the reader has subscribed) and "done" (the reader has closed).
 *
 * With STREAM_WRITER_SUBSCRIBERS=<n> in the environment, the writer instead
 * waits after step 0 in H5Fwait_subscribers() for n subscribers, and ignores
 * "ready": the subscription itself, over the transport, is what releases it.
 * Step 0 still comes first, since a Python subscriber needs the schema it
 * publishes before it can subscribe -- unless STREAM_WRITER_EARLY is also
 * set, in which case the writer waits before step 0, for a reader that
 * subscribes with subscribe(expect=). The writer touches "created" once the
 * file exists, for such a reader to open it.
 *
 * usage: stream_writer <column|narrowing|lifecycle|idle|eos|block|grow|types> <file> <syncdir>
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
#define GCOLS    4
#define NREC     3

typedef struct {
    int    a;
    double b;
    char   c[4];
} rec_t;

static char  g_syncdir[512];
static hid_t g_rec_type = H5I_INVALID_HID;

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

/* One row of "/series": created in step 0, extended by a row per step. */
static int
write_grow_step(hid_t fid, hid_t *ds, int s)
{
    hsize_t start[2] = {(hsize_t)s, 0}, count[2] = {1, GCOLS};
    hsize_t dims[2] = {(hsize_t)s + 1, GCOLS};
    int     vals[GCOLS], c;
    hid_t   fspace = H5I_INVALID_HID, mspace = H5I_INVALID_HID;
    int     rc = -1;

    for (c = 0; c < GCOLS; c++)
        vals[c] = s * 100 + c;
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0)
        goto done;
    if (s == 0) {
        hsize_t maxdims[2] = {H5S_UNLIMITED, GCOLS};
        hid_t   space = H5Screate_simple(2, dims, maxdims), dcpl = H5Pcreate(H5P_DATASET_CREATE);

        if (space < 0 || dcpl < 0 || H5Pset_chunk(dcpl, 2, count) < 0 ||
            (*ds = H5Dcreate2(fid, "/series", H5T_NATIVE_INT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0)
            goto done;
        H5Sclose(space);
        H5Pclose(dcpl);
    }
    else if (H5Dset_extent(*ds, dims) < 0)
        goto done;
    if ((fspace = H5Dget_space(*ds)) < 0 ||
        H5Sselect_hyperslab(fspace, H5S_SELECT_SET, start, NULL, count, NULL) < 0 ||
        (mspace = H5Screate_simple(2, count, NULL)) < 0 ||
        H5Dwrite(*ds, H5T_NATIVE_INT, mspace, fspace, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0)
        goto done;
    rc = 0;
done:
    if (fspace >= 0)
        H5Sclose(fspace);
    if (mspace >= 0)
        H5Sclose(mspace);
    if (rc < 0)
        fprintf(stderr, "stream_writer: FAIL grow step %d\n", s);
    return rc;
}

/* "/rec" and "/rec@scale", rewritten every step through open handles. */
static int
write_types_step(hid_t fid, hid_t *ds, hid_t *attr, int s)
{
    static hid_t names = H5I_INVALID_HID, units = H5I_INVALID_HID, vls = H5I_INVALID_HID;
    rec_t        recs[NREC];
    double       scale = s * 0.25;
    hsize_t      n     = NREC;
    char         nbuf[NREC][16], ubuf[16];
    const char  *nptr[NREC], *uptr = ubuf;
    int          i;

    for (i = 0; i < NREC; i++) {
        recs[i].a = s * 10 + i;
        recs[i].b = s + i * 0.5;
        snprintf(recs[i].c, sizeof(recs[i].c), "%d%d", s, i);
        snprintf(nbuf[i], sizeof(nbuf[i]), "n%d%d", s, i);
        nptr[i] = nbuf[i];
    }
    nptr[NREC - 1] = NULL; /* an unset string: None on the other side */
    snprintf(ubuf, sizeof(ubuf), "u%d", s);
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0)
        goto fail;
    if (s == 0) {
        hid_t str = H5Tcopy(H5T_C_S1), rt = H5Tcreate(H5T_COMPOUND, sizeof(rec_t));
        hid_t space = H5Screate_simple(1, &n, NULL), scalar = H5Screate(H5S_SCALAR);

        if (str < 0 || rt < 0 || space < 0 || scalar < 0 || H5Tset_size(str, 4) < 0 ||
            H5Tinsert(rt, "a", HOFFSET(rec_t, a), H5T_NATIVE_INT) < 0 ||
            H5Tinsert(rt, "b", HOFFSET(rec_t, b), H5T_NATIVE_DOUBLE) < 0 ||
            H5Tinsert(rt, "c", HOFFSET(rec_t, c), str) < 0 ||
            (*ds = H5Dcreate2(fid, "/rec", rt, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
            (*attr = H5Acreate2(*ds, "scale", H5T_NATIVE_DOUBLE, scalar, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
            /* variable-length strings: a dataset, and an attribute on /rec */
            (vls = H5Tcopy(H5T_C_S1)) < 0 || H5Tset_size(vls, H5T_VARIABLE) < 0 ||
            (names = H5Dcreate2(fid, "/names", vls, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
            (units = H5Acreate2(*ds, "units", vls, scalar, H5P_DEFAULT, H5P_DEFAULT)) < 0)
            goto fail;
        H5Tclose(str);
        H5Sclose(space);
        H5Sclose(scalar);
        g_rec_type = rt;
    }
    if (H5Dwrite(*ds, g_rec_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, recs) < 0 ||
        H5Awrite(*attr, H5T_NATIVE_DOUBLE, &scale) < 0 ||
        H5Dwrite(names, vls, H5S_ALL, H5S_ALL, H5P_DEFAULT, nptr) < 0 || H5Awrite(units, vls, &uptr) < 0 ||
        H5Fend_step(fid) < 0)
        goto fail;
    return 0;
fail:
    fprintf(stderr, "stream_writer: FAIL types step %d\n", s);
    return -1;
}

int
main(int argc, char **argv)
{
    hid_t   vol_id, fapl, fid, space, ds = H5I_INVALID_HID, attr = H5I_INVALID_HID;
    hsize_t dims[2] = {ROWS, COLS};
    const char *mode, *subs;
    int         s;

    if (argc != 4 || (strcmp(argv[1], "column") != 0 && strcmp(argv[1], "narrowing") != 0 &&
                      strcmp(argv[1], "lifecycle") != 0 && strcmp(argv[1], "idle") != 0 &&
                      strcmp(argv[1], "eos") != 0 && strcmp(argv[1], "block") != 0 &&
                      strcmp(argv[1], "grow") != 0 && strcmp(argv[1], "types") != 0)) {
        fprintf(stderr, "usage: %s <column|narrowing|lifecycle|idle|eos|block|grow|types> <file> <syncdir>\n",
                argv[0]);
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

    touch("created");
    if (getenv("STREAM_WRITER_EARLY") && (subs = getenv("STREAM_WRITER_SUBSCRIBERS")) != NULL &&
        H5Fwait_subscribers(fid, strtoull(subs, NULL, 10), 60000) < 0) {
        fprintf(stderr, "stream_writer: gave up waiting for %s subscribers before step 0\n", subs);
        return 1;
    }

    if ((!strcmp(mode, "grow")    ? write_grow_step(fid, &ds, 0)
         : !strcmp(mode, "types") ? write_types_step(fid, &ds, &attr, 0)
                                  : write_step(fid, space, &ds, 0)) < 0)
        return 1;
    touch("committed");
    if ((subs = getenv("STREAM_WRITER_SUBSCRIBERS")) != NULL) {
        if (H5Fwait_subscribers(fid, strtoull(subs, NULL, 10), 60000) < 0) {
            fprintf(stderr, "stream_writer: gave up waiting for %s subscribers\n", subs);
            return 1;
        }
    }
    else if (wait_for("ready", 60) < 0)
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
    else if (!strcmp(mode, "types")) {
        for (s = 1; s <= 2; s++)
            if (write_types_step(fid, &ds, &attr, s) < 0)
                return 1;
    }
    else if (!strcmp(mode, "grow")) {
        for (s = 1; s <= 3; s++)
            if (write_grow_step(fid, &ds, s) < 0)
                return 1;
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

    if (attr >= 0)
        H5Aclose(attr);
    if (g_rec_type >= 0)
        H5Tclose(g_rec_type);
    H5Dclose(ds);
    H5Sclose(space);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}
