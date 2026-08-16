/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * A PARALLEL writer applying a queue policy under real reader lag.
 *
 * This is the case the plain parallel_queue_policy test cannot reach. There,
 * no reader is attached, so no rank has a tracked reader and the ranks cannot
 * disagree about lag even if the reduction were removed -- it pins that the
 * collectives are entered in lockstep, and nothing more.
 *
 * Here a reader attaches and acks, and the asymmetry that makes this hard
 * appears on its own: a reader subscribes and acks to exactly ONE writer rank,
 * the one it happens to find by probing SSG group members
 * (vs_tr_reader_ack_step()). So exactly one rank sees the lag locally and the
 * others see none.
 *
 * The policy here is DISCARD, and that choice is the whole point. Block would
 * NOT actually distinguish the two implementations: without the reduction the
 * single acked rank blocks while its peers run ahead into the replay's own
 * collectives and wait for it there, so every rank still observes the same
 * wall time and the run still completes. Block looks tested and is not.
 *
 * Discard diverges properly, because the ranks take structurally different
 * code paths rather than the same path at different times:
 *
 *   Per-rank decision (what the code did before): the acked rank calls
 *   H5VL__stream_discard_step() -- create an empty group, return -- while its
 *   peers call H5VL__stream_replay_step_parallel() and enter the cross-rank
 *   aggregation. Different collective sequences on the same communicator:
 *   a hang, or if it survives, a step carrying some ranks' slabs and not
 *   others'.
 *
 *   Reduced decision (H5VL__stream_queue_lag_view()): every rank sees the same
 *   min-acked step, so every rank discards.
 *
 * So the assertions are that the run finishes at all (backed by a tight
 * timeout in CMakeLists.txt) and that step 2 is discarded UNIFORMLY -- the
 * step group exists, and carries no data from any rank. Steps 0 and 1 must
 * still be intact and complete.
 *
 * Shape: writer ranks under mpiexec, reader a separate single process, both
 * coordinated by sentinel files exactly as test/t_queue_policy.c does for the
 * serial case -- see run_parallel_lag_test.sh.
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NELEM         8
#define NRANKS_EXPECTED 3
#define RESERVE_SLOTS   1
#define SLEEP_SECS      1.5

#define PREFIX_SENTINEL "t_parallel_lag.prefix_ready"
#define ACK0_SENTINEL   "t_parallel_lag.ack0"
#define ACK1_SENTINEL   "t_parallel_lag.ack1"

static double
elapsed_seconds(const struct timespec *t0)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - t0->tv_sec) + (double)(now.tv_nsec - t0->tv_nsec) / 1e9;
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

/* One step: every rank creates the same dataset and writes its own slab. */
static int
write_step(hid_t fid, int s, int rank, int nranks)
{
    hid_t    ds, fspace, mspace, sel;
    hsize_t  dims[1]  = {(hsize_t)(NELEM * nranks)};
    hsize_t  start[1] = {(hsize_t)(rank * NELEM)};
    hsize_t  count[1] = {NELEM};
    int      buf[NELEM], i;
    char     name[32];
    uint64_t logical = (uint64_t)s;

    snprintf(name, sizeof(name), "d%d", s);
    for (i = 0; i < NELEM; i++)
        buf[i] = s * 1000 + rank * 100 + i;

    if (H5Fbegin_step(fid, 1, &logical, 0) < 0)
        return -1;

    if ((fspace = H5Screate_simple(1, dims, NULL)) < 0)
        return -1;
    if ((ds = H5Dcreate2(fid, name, H5T_NATIVE_INT, fspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0)
        return -1;
    if ((mspace = H5Screate_simple(1, count, NULL)) < 0)
        return -1;
    if ((sel = H5Scopy(fspace)) < 0 ||
        H5Sselect_hyperslab(sel, H5S_SELECT_SET, start, NULL, count, NULL) < 0)
        return -1;
    if (H5Dwrite(ds, H5T_NATIVE_INT, mspace, sel, H5P_DEFAULT, buf) < 0)
        return -1;

    H5Sclose(sel);
    H5Sclose(mspace);
    H5Dclose(ds);
    H5Sclose(fspace);

    return (H5Fend_step(fid) < 0) ? -1 : 0;
}

static int
do_write(const char *fname)
{
    hid_t           vol, fapl, fid;
    int             rank, nranks;
    struct timespec t0;
    double          step2_secs;
    int             rc = 0;

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    if ((vol = H5VL_stream_register()) < 0)
        return 1;
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol, NULL) < 0 ||
        H5Pset_fapl_mpio(fapl, MPI_COMM_WORLD, MPI_INFO_NULL) < 0)
        return 1;
    if ((fid = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        fprintf(stderr, "rank %d: FAIL create\n", rank);
        return 1;
    }

    /* Every rank sets the same policy, as H5Fset_stream_queue_policy()
     * documents. Without the collective reduction inside, only the rank the
     * reader happens to ack to would ever see pressure. */
    if (H5Fset_stream_queue_policy(fid, H5VL_STREAM_QUEUE_DISCARD, RESERVE_SLOTS) < 0) {
        fprintf(stderr, "rank %d: FAIL set policy\n", rank);
        return 1;
    }

    /* Steps 0 and 1 commit before the reader exists. */
    if (write_step(fid, 0, rank, nranks) < 0 || write_step(fid, 1, rank, nranks) < 0) {
        fprintf(stderr, "rank %d: FAIL prefix steps\n", rank);
        return 1;
    }

    /* Make the two committed steps visible to another process before
     * advertising them. A parallel writer's MPI-IO buffers are not otherwise
     * on disk yet, and a reader that opens on the sentinel builds its step
     * index immediately -- it would find no /step group at all and fail its
     * first H5Fbegin_step(). (Flushing between steps is meaningful precisely
     * because no step is open here; it does not and must not commit one --
     * see H5Fbegin_step()'s warning in H5VLstream.h.) */
    if (H5Fflush(fid, H5F_SCOPE_GLOBAL) < 0) {
        fprintf(stderr, "rank %d: FAIL flush before publishing\n", rank);
        return 1;
    }

    /* Publishing the sentinel is rank 0's job, but only once every rank has
     * finished flushing -- the file must be complete before a reader in
     * another process opens it. */
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0)
        touch_sentinel(PREFIX_SENTINEL);

    /* Wait for the reader to attach and ack step 0. Every rank waits, so all
     * of them reach the next step boundary together. */
    if (wait_for_sentinel(ACK0_SENTINEL, 200) < 0) {
        fprintf(stderr, "rank %d: FAIL reader never acked step 0\n", rank);
        return 1;
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* Step 2 needs the policy: 2 > min_acked(0) + reserve_slots(1). With
     * Discard every rank must drop it -- and must agree, since one rank
     * discarding while the others replay puts different collective sequences
     * on the same communicator. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (write_step(fid, 2, rank, nranks) < 0) {
        fprintf(stderr, "rank %d: FAIL step 2\n", rank);
        return 1;
    }
    step2_secs = elapsed_seconds(&t0);

    printf("rank %d: step 2 (discarded) took %.2fs\n", rank, step2_secs);
    fflush(stdout);

    /* Let the reader finish so its ack cannot arrive mid-teardown. */
    if (wait_for_sentinel(ACK1_SENTINEL, 200) < 0) {
        fprintf(stderr, "rank %d: FAIL reader never acked step 1\n", rank);
        return 1;
    }
    MPI_Barrier(MPI_COMM_WORLD);

    H5Fclose(fid);
    H5Pclose(fapl);

    {
        int any_bad = 0;

        MPI_Allreduce(&rc, &any_bad, 1, MPI_INT, MPI_LOR, MPI_COMM_WORLD);
        rc = any_bad;
    }
    MPI_Barrier(MPI_COMM_WORLD); /* every rank's file closed before rank 0 reopens */
    /* Rank 0 verifies the outcome natively, after every rank has closed. The
     * discard must have been unanimous: /step/2 exists (so later steps stay
     * reachable) and carries no dataset at all. A per-rank decision would
     * leave "d2" there holding the slabs of whichever ranks replayed. */
    if (rc == 0 && rank == 0) {
        hid_t nfapl = H5Pcreate(H5P_FILE_ACCESS), rfid;

        if ((rfid = H5Fopen(fname, H5F_ACC_RDONLY, nfapl)) < 0) {
            fprintf(stderr, "verify: FAIL reopen natively\n");
            rc = 1;
        }
        else {
            htri_t has_d2, has_step2, has_d0;

            H5E_BEGIN_TRY
            {
                has_step2 = H5Lexists(rfid, "/step/2", H5P_DEFAULT);
                has_d2    = H5Lexists(rfid, "/step/2/d2", H5P_DEFAULT);
                has_d0    = H5Lexists(rfid, "/step/0/d0", H5P_DEFAULT);
            }
            H5E_END_TRY

            if (has_step2 <= 0) {
                fprintf(stderr, "verify: FAIL /step/2 missing -- a discarded step must still exist so "
                                "later steps stay reachable\n");
                rc = 1;
            }
            if (has_d2 > 0) {
                fprintf(stderr, "verify: FAIL /step/2/d2 exists -- the discard was NOT unanimous, so some "
                                "rank replayed a step the others dropped\n");
                rc = 1;
            }
            if (has_d0 <= 0) {
                fprintf(stderr, "verify: FAIL /step/0/d0 missing -- an unpressured step was lost\n");
                rc = 1;
            }
            H5Fclose(rfid);
        }
        H5Pclose(nfapl);

        if (rc == 0)
            printf("writer: step 2 discarded unanimously; steps 0-1 intact\n");
    }

    return rc;
}

static int
do_read(const char *fname)
{
    hid_t           vol, fapl, fid;
    struct timespec t0;

    if (wait_for_sentinel(PREFIX_SENTINEL, 300) < 0) {
        printf("reader: FAIL no prefix sentinel\n");
        return 1;
    }

    if ((vol = H5VL_stream_register()) < 0)
        return 1;
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol, NULL) < 0)
        return 1;

    /* Opening joins the writer's SSG group; the first begin_step builds the
     * index over the two committed steps and acks step 0. Deliberately no
     * reopen later -- that would drop this reader from the writer's tracked
     * set, the same M3 limitation test/t_queue_policy.c documents. */
    if ((fid = H5Fopen(fname, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open\n");
        return 1;
    }

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("reader: FAIL begin_step 0\n");
        return 1;
    }
    touch_sentinel(ACK0_SENTINEL);
    printf("reader: acked step 0, stalling %.2fs\n", SLEEP_SECS);
    fflush(stdout);

    /* The stall the writer must actually wait out. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (elapsed_seconds(&t0) < SLEEP_SECS)
        usleep(50000);

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("reader: FAIL begin_step 1\n");
        return 1;
    }
    touch_sentinel(ACK1_SENTINEL);
    printf("reader: acked step 1, releasing the writer\n");
    fflush(stdout);

    H5Fclose(fid);
    H5Pclose(fapl);
    return 0;
}

int
main(int argc, char **argv)
{
    int rc;

    MPI_Init(&argc, &argv);

    if (argc < 3) {
        fprintf(stderr, "usage: %s write|read <file>\n", argv[0]);
        rc = 2;
    }
    else if (strcmp(argv[1], "write") == 0)
        rc = do_write(argv[2]);
    else if (strcmp(argv[1], "read") == 0)
        rc = do_read(argv[2]);
    else {
        fprintf(stderr, "unknown mode '%s'\n", argv[1]);
        rc = 2;
    }

    MPI_Finalize();
    return rc;
}
