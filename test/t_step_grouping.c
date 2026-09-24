/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Pins how a step's pushes are grouped as a subscriber sees them -- the
 * contract a per-step consumer (docs/python-plan.md's P2 reassembly, and
 * examples/detector_pipeline's monitor mode) is built on.
 *
 * t_subvolume_strided.c checks that a column subscription gets the right
 * elements across a whole run. It never asks whether ONE step's pushes are
 * complete by the time the reader learns the step exists. This does, for the
 * same column selection (ROWS separate runs per step):
 *
 *   Lockstep phase. The writer waits for the reader after every step. Once
 *   H5Fwait_step_ready() returns step N, a zero-timeout drain must return
 *   every one of N's pushes and nothing else. This holds only because
 *   vs_tr_writer_broadcast_step_ready() finishes delivering a step's pushes
 *   before it announces the step; if that order ever flips, this fails.
 *
 *   Lagging phase. The writer commits several steps before the reader reads
 *   anything. Pushes must come out grouped by step, in ascending order,
 *   never interleaved -- and draining step N must run into step N+1's first
 *   push, which the reader has to hold over because there is no peek. That
 *   carry-over is asserted to happen, so a consumer that discards it is
 *   known to be wrong rather than assumed to be.
 *
 *   Late join. After both phases, a second reader attaches to the running
 *   writer. Its first step-ready is the seed the join itself delivers for
 *   the writer's current step; that step was committed before this reader
 *   subscribed, so it must drain to nothing. The next step the writer
 *   commits must then arrive whole. This pins an empty first step as normal,
 *   so a per-step consumer skips it rather than reporting missing data.
 *
 * Every step's pushes must cover exactly the column, with that step's
 * values. The step a value belongs to is encoded in the value itself, so
 * nothing here assumes how physical step numbers are assigned.
 *
 * Three processes (writer, reader, late reader), na+sm, same shape as
 * test/t_subvolume_strided.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define ROWS 6
#define COLS 8
#define COL  3

#define LOCKSTEP_STEPS 3
#define LAGGING_STEPS  4
#define NSTEPS         (LOCKSTEP_STEPS + LAGGING_STEPS)
#define LATE_STEP      NSTEPS /* the one step written after the late reader joins */

#define FNAME "t_step_grouping.h5"

#define READY_SENTINEL       "t_step_grouping.reader_ready"
#define ACK_SENTINEL_FMT     "t_step_grouping.ack.%d"
#define WRITES_DONE_SENTINEL "t_step_grouping.writes_done"
#define READER_DONE_SENTINEL "t_step_grouping.reader_done"
#define LATE_GO_SENTINEL     "t_step_grouping.late_go"
#define LATE_READY_SENTINEL  "t_step_grouping.late_ready"
#define LATE_DONE_SENTINEL   "t_step_grouping.late_done"

static int
val_at(int s, int r, int c)
{
    return s * 10000 + r * 100 + c;
}

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

static void
ack_name(char *out, size_t len, int s)
{
    snprintf(out, len, ACK_SENTINEL_FMT, s);
}

/* One push, checked against step ordinal s. Marks the rows it covers in
 * seen[]; a row covered twice, a value from another step, or an element
 * outside the column is a failure. */
static int
check_push(int s, uint64_t elem_start, uint64_t elem_count, const int *vals, int seen[ROWS])
{
    uint64_t k;
    int      rc = 0;

    for (k = 0; k < elem_count; k++) {
        uint64_t flat = elem_start + k;
        int      r    = (int)(flat / COLS);
        int      c    = (int)(flat % COLS);

        if (c != COL || r < 0 || r >= ROWS) {
            printf("  FAIL  step %d: push covers flat index %llu, outside the subscribed column\n", s,
                   (unsigned long long)flat);
            rc = 1;
            continue;
        }
        if (vals[k] != val_at(s, r, c)) {
            printf("  FAIL  step %d: element (%d,%d) = %d, expected %d%s\n", s, r, c, vals[k],
                   val_at(s, r, c), vals[k] / 10000 != s ? " -- a value from another step" : "");
            rc = 1;
        }
        if (seen[r]) {
            printf("  FAIL  step %d: row %d delivered twice\n", s, r);
            rc = 1;
        }
        seen[r] = 1;
    }
    return rc;
}

static int
check_complete(int s, const int seen[ROWS], const char *why)
{
    int r, rc = 0;

    for (r = 0; r < ROWS; r++)
        if (!seen[r]) {
            printf("  FAIL  step %d: row %d missing after the drain -- %s\n", s, r, why);
            rc = 1;
        }
    return rc;
}

/* Join the writer's group and subscribe to the column. Returns the file id,
 * or H5I_INVALID_HID after printing why. */
static hid_t
open_and_subscribe(const char *who, hid_t *vol_id, hid_t *fapl)
{
    hid_t   fid, space;
    hsize_t dims[2]  = {ROWS, COLS};
    hsize_t start[2] = {0, COL};
    hsize_t count[2] = {ROWS, 1};
    int     i;

    if ((*vol_id = H5VL_stream_register()) < 0) {
        printf("%s: FAIL register\n", who);
        return H5I_INVALID_HID;
    }
    if ((*fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(*fapl, *vol_id, NULL) < 0 ||
        H5Pset_file_locking(*fapl, false, true) < 0) {
        printf("%s: FAIL fapl\n", who);
        return H5I_INVALID_HID;
    }
    for (i = 0; i < 100; i++) {
        FILE *f = fopen(FNAME ".vsgroup", "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000);
    }
    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, *fapl)) < 0) {
        printf("%s: FAIL open/join\n", who);
        return H5I_INVALID_HID;
    }

    if ((space = H5Screate_simple(2, dims, NULL)) < 0 ||
        H5Sselect_hyperslab(space, H5S_SELECT_SET, start, NULL, count, NULL) < 0) {
        printf("%s: FAIL build column selection\n", who);
        return H5I_INVALID_HID;
    }
    {
        const char *paths[1]  = {"/grid"};
        const hid_t spaces[1] = {space};

        if (H5Fsubscribe(fid, 1, paths, spaces, NULL) < 0) {
            printf("%s: FAIL subscribe\n", who);
            return H5I_INVALID_HID;
        }
    }
    H5Sclose(space);
    return fid;
}

static int
run_reader(void)
{
    hid_t    vol_id, fapl, fid;
    uint64_t prev_phys = 0;
    int      have_prev = 0;
    int      carried   = 0;
    int      s, rc = 0;

    /* The push popped past the end of a step, held for the next one. */
    int      have_held = 0;
    uint64_t held_phys = 0, held_start = 0, held_count = 0;
    void    *held_buf  = NULL;

    if ((fid = open_and_subscribe("reader", &vol_id, &fapl)) < 0)
        return 1;

    touch_sentinel(READY_SENTINEL);

    for (s = 0; s < NSTEPS; s++) {
        int      lagging = s >= LOCKSTEP_STEPS;
        int      seen[ROWS];
        uint64_t phys = 0, wall_ns = 0;

        memset(seen, 0, sizeof(seen));

        if (s == LOCKSTEP_STEPS && wait_for_sentinel(WRITES_DONE_SENTINEL, 200) < 0) {
            printf("reader: FAIL writer never finished the lagging phase\n");
            return 1;
        }

        if (H5Fwait_step_ready(fid, 20000, &phys, &wall_ns) < 0) {
            printf("  FAIL  step %d: no step-ready notification\n", s);
            rc = 1;
            break;
        }
        if (have_prev && phys <= prev_phys) {
            printf("  FAIL  step %d: step-ready for phys %llu after %llu -- not ascending\n", s,
                   (unsigned long long)phys, (unsigned long long)prev_phys);
            rc = 1;
        }
        prev_phys = phys;
        have_prev = 1;

        if (have_held) {
            if (held_phys != phys) {
                printf("  FAIL  step %d: held push is for phys %llu, but this step is %llu\n", s,
                       (unsigned long long)held_phys, (unsigned long long)phys);
                rc = 1;
            }
            else
                rc |= check_push(s, held_start, held_count, (const int *)held_buf, seen);
            free(held_buf);
            held_buf  = NULL;
            have_held = 0;
        }

        for (;;) {
            uint64_t data_phys = 0, elem_start = 0, elem_count = 0;
            char    *path = NULL;
            void    *buf  = NULL;
            size_t   size = 0;

            if (H5Fget_subscribed_data(fid, 0, &data_phys, &path, &buf, &size, &elem_start, &elem_count) < 0)
                break;
            free(path);

            if (data_phys == phys) {
                rc |= check_push(s, elem_start, elem_count, (const int *)buf, seen);
                free(buf);
                continue;
            }
            if (data_phys < phys) {
                printf("  FAIL  step %d: push for earlier phys %llu arrived after step %llu was announced "
                       "-- steps interleaved\n",
                       s, (unsigned long long)data_phys, (unsigned long long)phys);
                rc = 1;
                free(buf);
                continue;
            }
            if (!lagging) {
                printf("  FAIL  step %d: push for later phys %llu while the writer was waiting on this "
                       "reader\n",
                       s, (unsigned long long)data_phys);
                rc = 1;
                free(buf);
                continue;
            }
            have_held  = 1;
            held_phys  = data_phys;
            held_start = elem_start;
            held_count = elem_count;
            held_buf   = buf;
            carried++;
            break;
        }

        rc |= check_complete(s, seen,
                             lagging ? "pushes grouped wrong, or a later push discarded"
                                     : "step announced before its pushes were queued");

        if (!lagging) {
            char ack[64];

            ack_name(ack, sizeof(ack), s);
            touch_sentinel(ack);
        }
    }

    if (have_held) {
        printf("  FAIL  a push for phys %llu is left over after the last step\n", (unsigned long long)held_phys);
        free(held_buf);
        rc = 1;
    }

    /* The lagging phase queued every step before the reader looked, so each
     * step but the last must have run into the next one's first push. */
    if (carried != LAGGING_STEPS - 1) {
        printf("  FAIL  carried a push over %d time(s), expected %d -- the lagging phase did not lag, or "
               "the queues are not FIFO\n",
               carried, LAGGING_STEPS - 1);
        rc = 1;
    }

    if (!rc)
        printf("  ok    %d steps, %d runs each: every step complete at step-ready, never interleaved, "
               "%d carry-over(s) while lagging\n",
               NSTEPS, ROWS, carried);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    touch_sentinel(READER_DONE_SENTINEL);
    return rc;
}

/* Attaches after the writer has committed NSTEPS steps. See the top comment's
 * "Late join". */
static int
run_late_reader(void)
{
    hid_t    vol_id, fapl, fid;
    uint64_t seed_phys = 0, phys = 0, wall_ns = 0;
    int      seen[ROWS];
    int      stray = 0, rc = 0;

    if (wait_for_sentinel(LATE_GO_SENTINEL, 400) < 0) {
        printf("late reader: FAIL writer never signalled the late join\n");
        return 1;
    }
    if ((fid = open_and_subscribe("late reader", &vol_id, &fapl)) < 0)
        return 1;

    /* The writer is idle until LATE_READY, so the only step-ready that can
     * be queued now is the join's own seed. */
    if (H5Fwait_step_ready(fid, 5000, &seed_phys, &wall_ns) < 0) {
        printf("  FAIL  late join: no seeded step-ready for the writer's current step\n");
        rc = 1;
    }
    else {
        for (;;) {
            uint64_t data_phys = 0, elem_start = 0, elem_count = 0;
            char    *path = NULL;
            void    *buf  = NULL;
            size_t   size = 0;

            if (H5Fget_subscribed_data(fid, 0, &data_phys, &path, &buf, &size, &elem_start, &elem_count) < 0)
                break;
            free(path);
            free(buf);
            stray++;
        }
        if (stray) {
            printf("  FAIL  late join: the seeded step (phys %llu) drained %d push(es); it was committed "
                   "before this reader subscribed, so it must drain to nothing\n",
                   (unsigned long long)seed_phys, stray);
            rc = 1;
        }
    }

    touch_sentinel(LATE_READY_SENTINEL);

    memset(seen, 0, sizeof(seen));
    if (H5Fwait_step_ready(fid, 20000, &phys, &wall_ns) < 0) {
        printf("  FAIL  late join: no step-ready for the step written after joining\n");
        rc = 1;
    }
    else {
        if (!rc && phys <= seed_phys) {
            printf("  FAIL  late join: next step phys %llu is not after the seed %llu\n",
                   (unsigned long long)phys, (unsigned long long)seed_phys);
            rc = 1;
        }
        for (;;) {
            uint64_t data_phys = 0, elem_start = 0, elem_count = 0;
            char    *path = NULL;
            void    *buf  = NULL;
            size_t   size = 0;

            if (H5Fget_subscribed_data(fid, 0, &data_phys, &path, &buf, &size, &elem_start, &elem_count) < 0)
                break;
            free(path);
            if (data_phys != phys) {
                printf("  FAIL  late join: push for phys %llu while draining step %llu\n",
                       (unsigned long long)data_phys, (unsigned long long)phys);
                rc = 1;
            }
            else
                rc |= check_push(LATE_STEP, elem_start, elem_count, (const int *)buf, seen);
            free(buf);
        }
        rc |= check_complete(LATE_STEP, seen, "the first step after a late join arrived short");
    }

    if (!rc)
        printf("  ok    late join: seeded step drained empty, the next step arrived whole\n");

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    touch_sentinel(LATE_DONE_SENTINEL);
    return rc;
}

/* One whole-dataset write per step, through one handle created in step 0 --
 * t_predicate.c's pattern. The runs come from the subscription, not from how
 * the writer split its I/O. */
static int
write_step(hid_t fid, hid_t space, hid_t *ds, int s)
{
    int vals[ROWS * COLS];
    int r, c;

    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++)
            vals[r * COLS + c] = val_at(s, r, c);

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (s == 0 &&
         (*ds = H5Dcreate2(fid, "/grid", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) ||
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
    hsize_t dims[2] = {ROWS, COLS};
    int     s;

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

    if (wait_for_sentinel(READY_SENTINEL, 200) < 0) {
        printf("writer: FAIL reader never subscribed\n");
        return 1;
    }

    if ((space = H5Screate_simple(2, dims, NULL)) < 0) {
        printf("writer: FAIL create dataspace\n");
        return 1;
    }

    for (s = 0; s < NSTEPS; s++) {
        if (write_step(fid, space, &ds, s) < 0)
            return 1;

        if (s < LOCKSTEP_STEPS) {
            char ack[64];

            ack_name(ack, sizeof(ack), s);
            if (wait_for_sentinel(ack, 200) < 0) {
                printf("writer: FAIL reader never finished step %d\n", s);
                return 1;
            }
        }
    }

    touch_sentinel(WRITES_DONE_SENTINEL);
    wait_for_sentinel(READER_DONE_SENTINEL, 200);

    /* Late join: let the second reader attach to a writer that already has
     * NSTEPS committed steps, then commit one more for it. */
    touch_sentinel(LATE_GO_SENTINEL);
    if (wait_for_sentinel(LATE_READY_SENTINEL, 300) < 0) {
        printf("writer: FAIL late reader never subscribed\n");
        return 1;
    }
    if (write_step(fid, space, &ds, LATE_STEP) < 0)
        return 1;
    wait_for_sentinel(LATE_DONE_SENTINEL, 300);

    H5Dclose(ds);
    H5Sclose(space);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}

static void
cleanup_sentinels(void)
{
    char ack[64];
    int  s;

    unlink(READY_SENTINEL);
    unlink(WRITES_DONE_SENTINEL);
    unlink(READER_DONE_SENTINEL);
    unlink(LATE_GO_SENTINEL);
    unlink(LATE_READY_SENTINEL);
    unlink(LATE_DONE_SENTINEL);
    for (s = 0; s < LOCKSTEP_STEPS; s++) {
        ack_name(ack, sizeof(ack), s);
        unlink(ack);
    }
}

int
main(void)
{
    pid_t reader_pid, late_pid;
    int   reader_status = 0, late_status = 0, writer_status, nerrors = 0;

    printf("vol-stream: per-step push grouping for a multi-run subscription (na+sm)\n");

    setenv("VOL_STREAM_NA", "na+sm", 0);
    cleanup_sentinels();

    /* A stale sidecar from an earlier run would send the reader to a dead
     * group -- see t_subvolume_strided.c's identical pre-fork cleanup. */
    unlink(FNAME ".vsgroup");
    unlink(FNAME);

    fflush(NULL);
    if ((reader_pid = fork()) < 0) {
        perror("fork");
        return 1;
    }
    if (reader_pid == 0) {
        int rc = run_reader();

        fflush(NULL);
        _exit(rc);
    }
    if ((late_pid = fork()) < 0) {
        perror("fork");
        return 1;
    }
    if (late_pid == 0) {
        int rc = run_late_reader();

        fflush(NULL);
        _exit(rc);
    }

    writer_status = run_writer();

    if (waitpid(reader_pid, &reader_status, 0) < 0 || waitpid(late_pid, &late_status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    cleanup_sentinels();

    if (writer_status != 0) {
        printf("\nwriter process reported failure\n");
        nerrors++;
    }
    if (!(WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 0)) {
        printf("\nreader process reported failure (status=%d)\n", reader_status);
        nerrors++;
    }
    if (!(WIFEXITED(late_status) && WEXITSTATUS(late_status) == 0)) {
        printf("\nlate reader process reported failure (status=%d)\n", late_status);
        nerrors++;
    }

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
