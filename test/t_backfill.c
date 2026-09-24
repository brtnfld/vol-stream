/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Backfill for a late joiner: H5Fsubscribe_from().
 *
 * The writer commits steps 0-2 before any reader exists; step 1 writes only
 * /y. A reader then joins, discards the join announcement, and subscribes to
 * /x from step 0. The writer commits step 3. The reader must see steps 0, 1,
 * 2, 3 -- each once, in order -- with /x's data at 0, 2 and 3 (read back
 * from the writer's file for the first two, live for the last) and nothing
 * for /x at step 1, which did not write it. A second H5Fsubscribe_from() on
 * the same file must be refused: it must be the reader's first subscription.
 *
 * Two processes, na+sm, same shape as test/t_subscribe.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define N     8
#define FNAME "t_backfill.h5"

#define COMMITTED_SENTINEL  "t_backfill.committed"
#define SUBSCRIBED_SENTINEL "t_backfill.subscribed"
#define DONE_SENTINEL       "t_backfill.done"

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
value(int s, int i)
{
    return s * 100 + i;
}

static int
run_reader(void)
{
    hid_t      vol_id, fapl, fid, space;
    hsize_t    dims = N;
    uint64_t   phys = 0, wall = 0;
    int        s, rc = 0;
    const char *paths[1] = {"/x"};

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL setup\n");
        return 1;
    }
    if (wait_for(COMMITTED_SENTINEL, 300) < 0 || (fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open/join\n");
        return 1;
    }
    /* The join announces the writer's current step; that is not part of the
     * ordered backfill, so drop it first, as the doc comment says. */
    while (H5Fwait_step_ready(fid, 0, &phys, &wall) >= 0)
        ;

    if ((space = H5Screate_simple(1, &dims, NULL)) < 0) {
        printf("reader: FAIL dataspace\n");
        return 1;
    }
    {
        const hid_t spaces[1] = {space};

        if (H5Fsubscribe_from(fid, 0, 1, paths, spaces, NULL) < 0) {
            printf("  FAIL  H5Fsubscribe_from()\n");
            return 1;
        }
        H5E_BEGIN_TRY
        {
            if (H5Fsubscribe_from(fid, 0, 1, paths, spaces, NULL) >= 0) {
                printf("  FAIL  a second H5Fsubscribe_from() was accepted\n");
                rc = 1;
            }
            else
                printf("  ok    a second H5Fsubscribe_from() is refused (it must come first)\n");
        }
        H5E_END_TRY
    }
    H5Sclose(space);
    touch(SUBSCRIBED_SENTINEL);

    for (s = 0; s < 4; s++) {
        int got_x = 0, ok = 1;

        if (H5Fwait_step_ready(fid, 30000, &phys, &wall) < 0) {
            printf("  FAIL  step %d was never announced\n", s);
            rc = 1;
            break;
        }
        if (phys != (uint64_t)s) {
            printf("  FAIL  expected step %d next, got step %llu\n", s, (unsigned long long)phys);
            rc = 1;
            break;
        }
        /* Every push of a step is queued before its announcement. */
        for (;;) {
            uint64_t p = 0, es = 0, ec = 0;
            char    *path = NULL;
            void    *buf  = NULL;
            size_t   size = 0;
            int      i;

            if (H5Fget_subscribed_data(fid, 0, &p, &path, &buf, &size, &es, &ec, NULL) < 0)
                break;
            if (p != (uint64_t)s || !path || strcmp(path, "/x") != 0 || es != 0 || ec != N ||
                size != N * sizeof(int))
                ok = 0;
            else
                for (i = 0; i < N; i++)
                    if (((int *)buf)[i] != value(s, i))
                        ok = 0;
            got_x++;
            free(path);
            free(buf);
        }
        if (!ok || got_x != (s == 1 ? 0 : 1)) {
            printf("  FAIL  step %d: %d push(es) for /x, %s\n", s, got_x, ok ? "values right" : "values wrong");
            rc = 1;
        }
        else
            printf("  ok    step %d %s\n", s,
                   s == 1   ? "announced, with nothing for /x (it did not write /x)"
                   : s == 3 ? "arrived live, after the backfill"
                            : "backfilled from the writer's file, with the right values");
    }
    if (!rc && H5Fwait_step_ready(fid, 500, &phys, &wall) >= 0) {
        printf("  FAIL  an extra step %llu was announced\n", (unsigned long long)phys);
        rc = 1;
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    touch(DONE_SENTINEL);
    return rc;
}

static int
write_step(hid_t fid, hid_t space, hid_t *xds, hid_t *yds, int s)
{
    int vals[N], i;

    for (i = 0; i < N; i++)
        vals[i] = value(s, i);
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0)
        return -1;
    if (s == 0 &&
        ((*xds = H5Dcreate2(fid, "/x", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
         (*yds = H5Dcreate2(fid, "/y", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0))
        return -1;
    if (s != 1 && H5Dwrite(*xds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0)
        return -1;
    if (H5Dwrite(*yds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0)
        return -1;
    return H5Fend_step(fid) < 0 ? -1 : 0;
}

static int
run_writer(void)
{
    hid_t   vol_id, fapl, fid, space, xds = H5I_INVALID_HID, yds = H5I_INVALID_HID;
    hsize_t dims = N;
    int     s;

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0 ||
        (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (space = H5Screate_simple(1, &dims, NULL)) < 0) {
        printf("writer: FAIL setup\n");
        return 1;
    }
    for (s = 0; s < 3; s++)
        if (write_step(fid, space, &xds, &yds, s) < 0) {
            printf("writer: FAIL step %d\n", s);
            return 1;
        }
    touch(COMMITTED_SENTINEL);
    if (wait_for(SUBSCRIBED_SENTINEL, 300) < 0) {
        printf("writer: FAIL reader never subscribed\n");
        return 1;
    }
    /* H5Fbegin_step() serves the backfill before this step's own data. */
    if (write_step(fid, space, &xds, &yds, 3) < 0) {
        printf("writer: FAIL step 3\n");
        return 1;
    }
    wait_for(DONE_SENTINEL, 600);

    H5Dclose(xds);
    H5Dclose(yds);
    H5Sclose(space);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}

static void
clean(void)
{
    unlink(COMMITTED_SENTINEL);
    unlink(SUBSCRIBED_SENTINEL);
    unlink(DONE_SENTINEL);
    unlink(FNAME ".vsgroup");
    unlink(FNAME);
}

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status, nerrors = 0;

    printf("vol-stream: backfill for a late joiner, H5Fsubscribe_from() (na+sm)\n");
    setenv("VOL_STREAM_NA", "na+sm", 0);
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
    writer_status = run_writer();
    if (waitpid(pid, &reader_status, 0) < 0) {
        perror("waitpid");
        return 1;
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
