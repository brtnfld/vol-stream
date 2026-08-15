/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M5 exit gate: SSG rendezvous and late joiners.
 *
 *   1. A writer starts with no readers and proceeds -- N1 steps commit with
 *      nobody in its SSG group but itself.
 *   2. A reader attaches at step N1-1 (a late joiner, dev-plan.md's own
 *      "attaches at step 500" scenario, scaled down) and gets a coherent
 *      view: its first H5Fwait_step_ready() returns the writer's current
 *      step immediately -- seeded by vs_tr_reader_join_group()'s
 *      get_current_step query, not learned by waiting for the next write
 *      (which the late joiner already missed).
 *   3. The reader then receives N2 further steps live, proving continuity
 *      from the seeded point into the ordinary push path.
 *   4. The reader is SIGKILLed -- an unclean death, no graceful SSG leave,
 *      unlike test/t_transport.c's orderly shutdown -- and the writer
 *      commits N3 more steps afterward. Each of those end_step() calls is
 *      timed: SSG's SWIM failure detector (configured for ~600ms-1s
 *      detection in vs_tr_writer_start_group()) drops the dead member from
 *      the group view on its own schedule, and
 *      vs_tr_writer_broadcast_step_ready() bounds each individual push to
 *      1s (margo_forward_timed()) regardless, so total time for N3 steps
 *      must stay well under what an indefinite stall would look like.
 *
 * Only compiled/run when VOL_STREAM_HAVE_MERCURY is on; see
 * test/CMakeLists.txt. Uses na+sm, like test/t_transport.c.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define N1    3 /* steps committed before any reader exists */
#define N2    2 /* live steps the late joiner receives after joining */
#define N3    3 /* steps committed after the reader is SIGKILLed */
#define FNAME "t_rendezvous.h5"

#define LATE_JOIN_SENTINEL "t_rendezvous.late_join_ok"
#define JOINED_SENTINEL     "t_rendezvous.reader_joined"
#define GOT_LIVE_SENTINEL   "t_rendezvous.reader_got_live"

static double
elapsed_seconds(struct timespec *start)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1.0e9;
}

static int
write_one_step(hid_t fid, hid_t sp, int step_no)
{
    char           name[32];
    const uint64_t logical = 1000 + (uint64_t)step_no;
    const uint64_t wall_ns = (uint64_t)(step_no + 1) * 1000;
    int            val     = step_no;
    hid_t          ds;

    snprintf(name, sizeof(name), "d%d", step_no);

    if (H5Fbegin_step(fid, 1, &logical, wall_ns) < 0)
        return -1;
    if ((ds = H5Dcreate2(fid, name, H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0)
        return -1;
    if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val) < 0) {
        H5Dclose(ds);
        return -1;
    }
    H5Dclose(ds);
    return H5Fend_step(fid);
}

static int
run_writer(pid_t reader_pid)
{
    hid_t           vol_id, fapl, fid, sp;
    int             s;
    struct timespec t0;
    double          no_reader_secs, post_death_secs;

    unlink(FNAME ".vsgroup");

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
    if ((sp = H5Screate(H5S_SCALAR)) < 0) {
        printf("writer: FAIL create dataspace\n");
        return 1;
    }

    /* --- Phase 1: no readers at all. --- */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (s = 0; s < N1; s++) {
        if (write_one_step(fid, sp, s) < 0) {
            printf("writer: FAIL step %d (no readers yet)\n", s);
            return 1;
        }
    }
    no_reader_secs = elapsed_seconds(&t0);
    printf("  ok    %d steps committed with no readers, %.3fs (did not wait for anyone)\n", N1,
           no_reader_secs);

    /* Signal the late joiner it may attach now, then wait for it to report
     * it has joined and verified its coherent view. */
    {
        FILE *f = fopen(LATE_JOIN_SENTINEL, "w");

        if (f)
            fclose(f);
    }
    for (s = 0; s < 100; s++) {
        FILE *f = fopen(JOINED_SENTINEL, "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000);
    }
    if (s == 100) {
        printf("writer: FAIL reader never reported joined\n");
        return 1;
    }

    /* --- Phase 2: N2 more steps, the late joiner should see these live. --- */
    for (s = N1; s < N1 + N2; s++) {
        if (write_one_step(fid, sp, s) < 0) {
            printf("writer: FAIL step %d (live steps for the joiner)\n", s);
            return 1;
        }
    }

    for (s = 0; s < 100; s++) {
        FILE *f = fopen(GOT_LIVE_SENTINEL, "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000);
    }
    if (s == 100) {
        printf("writer: FAIL reader never reported receiving the live steps\n");
        return 1;
    }

    /* --- Phase 3: kill the reader uncleanly, then keep writing. --- */
    if (kill(reader_pid, SIGKILL) < 0) {
        perror("writer: kill");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (s = N1 + N2; s < N1 + N2 + N3; s++) {
        if (write_one_step(fid, sp, s) < 0) {
            printf("writer: FAIL step %d (after reader death)\n", s);
            return 1;
        }
    }
    post_death_secs = elapsed_seconds(&t0);

    /* Generous bound: N3 steps, each individually capped at 1s by
     * margo_forward_timed() even in the worst case where SWIM has not yet
     * dropped the dead member from the group view, plus SWIM's own
     * detection overhead. Nowhere near what an actual stall looks like
     * (this would time out the whole test via ctest's TIMEOUT instead). */
    printf("  %s  %d steps committed after reader death, %.3fs (bound: %.1fs)\n",
           (post_death_secs < (double)(N3 + 2)) ? "ok  " : "FAIL", N3, post_death_secs, (double)(N3 + 2));

    H5Sclose(sp);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    return (post_death_secs < (double)(N3 + 2)) ? 0 : 1;
}

static int
run_reader(void)
{
    hid_t vol_id, fapl, fid;
    int   s, rc = 0;
    int   i;

    /* Wait to be let in until after the writer has already committed N1
     * steps -- this is what makes this a late joiner. */
    for (i = 0; i < 100; i++) {
        FILE *f = fopen(LATE_JOIN_SENTINEL, "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000);
    }
    if (i == 100) {
        printf("reader: FAIL never got the go-ahead to join\n");
        return 1;
    }

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("reader: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL fapl\n");
        return 1;
    }

    /* Same durable-superblock wait as t_transport.c: the writer only
     * publishes the group-id sidecar after flushing. */
    for (i = 0; i < 100; i++) {
        FILE *f = fopen(FNAME ".vsgroup", "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000);
    }

    /* Joining here (inside H5Fopen()) is what makes this a late joiner: N1
     * steps already exist that this process never saw committed. */
    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open/join\n");
        return 1;
    }

    /* The coherent view: this must return immediately (it was seeded by
     * vs_tr_reader_join_group()'s get_current_step query), reporting the
     * writer's *current* step -- N1-1 -- not block waiting for a step this
     * reader already missed. */
    {
        uint64_t phys = (uint64_t)-1, wall_ns = 0;

        if (H5Fwait_step_ready(fid, 0, &phys, &wall_ns) < 0) {
            printf("  FAIL  late joiner did not get a seeded coherent view\n");
            rc = 1;
        }
        else if (phys != (uint64_t)(N1 - 1)) {
            printf("  FAIL  coherent view reported step %llu, expected %d\n", (unsigned long long)phys,
                   N1 - 1);
            rc = 1;
        }
        else
            printf("  ok    late joiner's first wait returned the current step (%llu) immediately\n",
                   (unsigned long long)phys);
    }

    {
        FILE *f = fopen(JOINED_SENTINEL, "w");

        if (f)
            fclose(f);
    }

    /* Now receive N2 more steps live, continuing on from the seeded one. */
    for (s = N1; s < N1 + N2; s++) {
        uint64_t phys = (uint64_t)-1, wall_ns = 0;

        if (H5Fwait_step_ready(fid, 10000, &phys, &wall_ns) < 0) {
            printf("  FAIL  live step_ready %d timed out\n", s);
            rc = 1;
            break;
        }
        if (phys != (uint64_t)s) {
            printf("  FAIL  live step out of order: got %llu, expected %d\n", (unsigned long long)phys, s);
            rc = 1;
            continue;
        }
        printf("  ok    live step_ready for physical step %llu after joining late\n",
               (unsigned long long)phys);
    }

    {
        FILE *f = fopen(GOT_LIVE_SENTINEL, "w");

        if (f)
            fclose(f);
    }
    fflush(NULL);

    /* Now just wait to be killed -- see run_writer()'s Phase 3. A real
     * crash would not close fid/fapl/vol_id or leave the SSG group
     * gracefully, so neither does this: it deliberately never gets past
     * here. */
    H5Fwait_step_ready(fid, 30000, NULL, NULL);

    /* Unreachable in the normal run (SIGKILL beats the 30s timeout above),
     * but if the writer somehow never killed us, exit cleanly rather than
     * leaving a runaway process behind. */
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return rc;
}

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status;
    int   nerrors        = 0;

    printf("vol-stream M5: SSG rendezvous and late joiners (na+sm)\n");

    /* A default, not a requirement: the CI matrix runs this suite over more
     * than one Mercury NA plugin, so an externally-set VOL_STREAM_NA wins
     * (overwrite = 0). Shared memory stays the default because it needs no
     * network setup on a bare runner. */
    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(LATE_JOIN_SENTINEL);
    unlink(JOINED_SENTINEL);
    unlink(GOT_LIVE_SENTINEL);

    if ((pid = fork()) < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        int rc = run_reader();

        fflush(NULL);
        _exit(rc);
    }

    writer_status = run_writer(pid);

    if (waitpid(pid, &reader_status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    unlink(LATE_JOIN_SENTINEL);
    unlink(JOINED_SENTINEL);
    unlink(GOT_LIVE_SENTINEL);

    if (writer_status != 0) {
        printf("\nwriter process reported failure\n");
        nerrors++;
    }
    /* The reader is SIGKILLed by design -- WIFSIGNALED with SIGKILL is the
     * *expected* outcome, not a failure. Its earlier printf()s (already
     * flushed before it blocked in the final wait) already reported
     * whether its own checks passed. */
    if (!(WIFSIGNALED(reader_status) && WTERMSIG(reader_status) == SIGKILL)) {
        printf("\nreader process did not die the way this test expected (status=%d)\n", reader_status);
        nerrors++;
    }

    /* Data correctness, decoupled from the live-notification proof above:
     * reopen fresh and confirm all N1+N2+N3 steps replayed correctly,
     * including the ones committed after the reader was gone. */
    {
        hid_t rid;

        if ((rid = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
            printf("  FAIL  reopen natively\n");
            nerrors++;
        }
        else {
            int total = N1 + N2 + N3;
            int s;

            for (s = 0; s < total; s++) {
                char  path[64];
                int   val = -1;
                hid_t ds;

                snprintf(path, sizeof(path), "/step/%d/d%d", s, s);
                if ((ds = H5Dopen2(rid, path, H5P_DEFAULT)) < 0) {
                    printf("  FAIL  open %s\n", path);
                    nerrors++;
                    continue;
                }
                H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val);
                if (val != s) {
                    printf("  FAIL  %s: got %d, expected %d\n", path, val, s);
                    nerrors++;
                }
                H5Dclose(ds);
            }
            printf("  ok    all %d steps (including the %d written after the reader died) replayed "
                   "correctly\n",
                   total, N3);
            H5Fclose(rid);
        }
    }

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
