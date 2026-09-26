/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Phase 1 follow-up: one bulk registration serves many pushes.
 *
 * One step writes /grid, ROWS x COLS ints; the reader subscribes to one
 * column, so the write splits into ROWS runs, and with
 * VOL_STREAM_BULK_THRESHOLD=0 every run goes by bulk. The writer's push
 * statistics must show ROWS pushes via bulk but ONE registration -- the
 * step's staging buffer, registered once and pulled from at ROWS offsets --
 * and every value must arrive right, which also checks the offsets.
 *
 * Two processes, na+sm, same shape as test/t_bulk_push.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define ROWS  32
#define COLS  8
#define COL   5
#define FNAME "t_bulk_regions.h5"
#define STATS "t_bulk_regions.stats"

#define READY_SENTINEL "t_bulk_regions.reader_ready"
#define DONE_SENTINEL  "t_bulk_regions.reader_done"

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
    hid_t    vol_id, fapl, fid, space;
    hsize_t  dims[2] = {ROWS, COLS}, start[2] = {0, COL}, count[2] = {ROWS, 1};
    uint64_t phys = 0, wall = 0;
    int      seen[ROWS], i, rc = 0, pushes = 0;

    memset(seen, 0, sizeof(seen));
    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0 ||
        wait_for(FNAME ".vsgroup", 100) < 0 || (fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open/join\n");
        return 1;
    }
    if ((space = H5Screate_simple(2, dims, NULL)) < 0 ||
        H5Sselect_hyperslab(space, H5S_SELECT_SET, start, NULL, count, NULL) < 0) {
        printf("reader: FAIL selection\n");
        return 1;
    }
    {
        const char *paths[1]  = {"/grid"};
        const hid_t spaces[1] = {space};

        if (H5Fsubscribe(fid, 1, paths, spaces, NULL) < 0) {
            printf("reader: FAIL subscribe\n");
            return 1;
        }
    }
    H5Sclose(space);
    touch(READY_SENTINEL);

    if (H5Fwait_step_ready(fid, 30000, &phys, &wall) < 0) {
        printf("  FAIL  the step was never announced\n");
        rc = 1;
    }
    for (;;) {
        uint64_t p = 0, es = 0, ec = 0, k;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;

        if (H5Fget_subscribed_data(fid, 0, &p, &path, &buf, &size, &es, &ec, NULL) < 0)
            break;
        pushes++;
        for (k = 0; k < ec; k++) {
            uint64_t flat = es + k;
            int      r = (int)(flat / COLS), c = (int)(flat % COLS);

            if (c == COL && r < ROWS && ((int *)buf)[k] == r * 100 + c)
                seen[r] = 1;
        }
        free(path);
        free(buf);
    }
    for (i = 0; i < ROWS; i++)
        if (!seen[i]) {
            printf("  FAIL  row %d of the column never arrived right\n", i);
            rc = 1;
            break;
        }
    if (!rc)
        printf("  ok    all %d column elements arrived right, in %d pushes\n", ROWS, pushes);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    touch(DONE_SENTINEL);
    return rc;
}

static int
run_writer(void)
{
    hid_t   vol_id, fapl, fid, space, ds;
    hsize_t dims[2] = {ROWS, COLS};
    int     vals[ROWS * COLS], r, c;

    if (!freopen(STATS, "w", stderr)) {
        printf("writer: FAIL redirect stderr\n");
        return 1;
    }
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++)
            vals[r * COLS + c] = r * 100 + c;
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
    if ((space = H5Screate_simple(2, dims, NULL)) < 0 || H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (ds = H5Dcreate2(fid, "/grid", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
        printf("writer: FAIL write\n");
        return 1;
    }
    H5Dclose(ds);
    H5Sclose(space);
    wait_for(DONE_SENTINEL, 300);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    fflush(stderr);
    return 0;
}

/* The two counts from "N via bulk (M registrations)", or -1. */
static void
bulk_counts(long *pushes, long *regs)
{
    char  line[1024];
    FILE *f = fopen(STATS, "r");

    *pushes = *regs = -1;
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, " via bulk (");

        if (p) {
            char *q = p;

            while (q > line && q[-1] >= '0' && q[-1] <= '9')
                q--;
            *pushes = atol(q);
            *regs   = atol(p + strlen(" via bulk ("));
        }
    }
    fclose(f);
}

static void
clean(void)
{
    unlink(READY_SENTINEL);
    unlink(DONE_SENTINEL);
    unlink(STATS);
    unlink(FNAME ".vsgroup");
    unlink(FNAME ".vsdone");
    unlink(FNAME);
}

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status, nerrors = 0;
    long  pushes, regs;

    printf("vol-stream Phase 1: one bulk registration serves a step's pushes (na+sm)\n");
    setenv("VOL_STREAM_NA", "na+sm", 0);
    setenv("VOL_STREAM_BULK_THRESHOLD", "0", 1);
    unsetenv("VOL_STREAM_STAGE_PAYLOAD"); /* staging on: the step's one buffer */
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
    setenv("VOL_STREAM_PUSH_STATS", "1", 1);
    writer_status = run_writer();
    if (waitpid(pid, &reader_status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    bulk_counts(&pushes, &regs);
    if (pushes == ROWS && regs == 1)
        printf("  ok    %ld pushes went by bulk from %ld registration\n", pushes, regs);
    else {
        printf("  FAIL  %ld pushes via bulk from %ld registrations, expected %d from 1\n", pushes, regs, ROWS);
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
