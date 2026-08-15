/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M7 exit gate: under a deliberately slow reader, Block stalls the writer
 * and loses nothing, Discard drops only whole steps, Spill does neither and
 * the lagging reader catches up from local storage.
 *
 * Two OS processes, na+sm, same overall shape as test/t_rendezvous.c. What
 * makes the reader "deliberately slow" here is not how fast it opens the
 * file, but how long it waits between acking one step and the next --
 * see run_reader()'s comment for why its index is built to cover exactly
 * two steps up front (avoiding a reopen mid-test, which would drop this
 * reader from the writer's tracked set the same way a graceful departure
 * would -- a real M3 limitation, "a reader's own step index does not yet
 * grow live", not something this test works around).
 *
 * Run three times, once per policy, sequentially in one process:
 *
 *   Prefix: the writer commits steps 0 and 1 before the reader ever opens.
 *   The reader then opens (joins the group), advances to step 0 and acks it
 *   -- now tracked, with reserve_slots=1 -- then sleeps SLEEP_SECS before
 *   advancing to step 1 (already in its index; no reopen) and acking that.
 *
 *   Meanwhile the writer, right after the reader's first ack, commits step
 *   2 -- which needs the policy, since physical_step 2 > min_acked(0) +
 *   reserve_slots(1). Each policy's own end_step() call for step 2 is
 *   timed:
 *     Block:   blocks until the reader's second ack (~SLEEP_SECS).
 *     Discard: returns immediately; step 2's data is genuinely gone.
 *     Spill:   also returns immediately; step 2's manifest+payload go to
 *              BAKE instead. The writer then waits for the reader's second
 *              ack and commits one more (drain-trigger) step, which drains
 *              step 2 back into the file for real before this test checks
 *              it.
 *
 * Only compiled/run when VOL_STREAM_HAVE_MERCURY is on; see
 * test/CMakeLists.txt. Spill is skipped (not a failure) if the connector
 * was built without VOL_STREAM_HAVE_BAKE -- there is nothing more to prove
 * beyond what Discard already does in that configuration (see
 * H5VL__stream_apply_queue_policy()'s documented fallback).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define RESERVE_SLOTS 1
#define SLEEP_SECS    1.2

#define PREFIX_SENTINEL "t_queue_policy.prefix_ready"
#define ACK0_SENTINEL   "t_queue_policy.reader_ack0"
#define ACK1_SENTINEL   "t_queue_policy.reader_ack1"

static double
elapsed_seconds(struct timespec *start)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1.0e9;
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

static int
write_one_step(hid_t fid, hid_t sp, int step_no)
{
    const uint64_t logical = 1000 + (uint64_t)step_no;
    const uint64_t wall_ns = (uint64_t)(step_no + 1) * 1000;
    int            val     = step_no;
    hid_t          ds;

    if (H5Fbegin_step(fid, 1, &logical, wall_ns) < 0)
        return -1;
    if ((ds = H5Dcreate2(fid, "val", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0)
        return -1;
    if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val) < 0) {
        H5Dclose(ds);
        return -1;
    }
    H5Dclose(ds);
    return H5Fend_step(fid);
}

static int
run_reader(const char *fname)
{
    hid_t vol_id, fapl, fid;
    int   i, rc = 0;

    if (wait_for_sentinel(PREFIX_SENTINEL, 100) < 0) {
        printf("reader: FAIL never got the prefix-ready sentinel\n");
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
    for (i = 0; i < 100; i++) {
        FILE *f = fopen(fname, "r"); /* wait for the writer's superblock (see t_transport.c) */

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000);
    }

    /* Joining here builds the step index lazily on the first H5Fbegin_step()
     * below -- at that point the writer has committed exactly steps 0 and 1
     * (the prefix), so the index covers exactly those two, letting this
     * reader advance through both without ever reopening. See this file's
     * top comment for why that matters. */
    if ((fid = H5Fopen(fname, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open/join\n");
        return 1;
    }

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("reader: FAIL advance to step 0\n");
        rc = 1;
    }
    touch_sentinel(ACK0_SENTINEL);

    /* The "deliberately slow" part: consume step 0, then sit on it for a
     * while before moving on, exactly as if reading and processing it took
     * real time -- the writer, meanwhile, is free to keep committing. */
    {
        struct timespec t0;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (elapsed_seconds(&t0) < SLEEP_SECS)
            usleep(50000);
    }

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("reader: FAIL advance to step 1\n");
        rc = 1;
    }
    touch_sentinel(ACK1_SENTINEL);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return rc;
}

static int
run_writer(hid_t vol_id, const char *fname, H5VL_stream_queue_policy_t policy, const char *policy_name,
           pid_t reader_pid, double *out_step2_secs)
{
    hid_t           fapl, fid, sp;
    struct timespec t0;
    char            group_sidecar[128];

    (void)reader_pid;

    snprintf(group_sidecar, sizeof(group_sidecar), "%s.vsgroup", fname);
    unlink(group_sidecar);
    unlink(fname);

    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("writer(%s): FAIL fapl\n", policy_name);
        return 1;
    }
    if ((fid = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("writer(%s): FAIL create (transport up? VOL_STREAM_NA set?)\n", policy_name);
        return 1;
    }
    if ((sp = H5Screate(H5S_SCALAR)) < 0) {
        printf("writer(%s): FAIL create dataspace\n", policy_name);
        return 1;
    }
    if (H5Fset_stream_queue_policy(fid, policy, RESERVE_SLOTS) < 0) {
        printf("writer(%s): FAIL set queue policy\n", policy_name);
        return 1;
    }

    /* Prefix: steps 0 and 1, before the reader exists at all -- see this
     * file's top comment. Flushed explicitly: a reader in a different
     * process opening right after the sentinel appears must see a complete,
     * durable "/step" group, not race these writes the same way an
     * unflushed superblock could race H5Fcreate() itself (the bug
     * H5VL__stream_transport_start_writer() already works around once, at
     * file-create time -- this is the same race one step later). */
    if (write_one_step(fid, sp, 0) < 0 || write_one_step(fid, sp, 1) < 0 || H5Fflush(fid, H5F_SCOPE_GLOBAL) < 0) {
        printf("writer(%s): FAIL prefix steps\n", policy_name);
        return 1;
    }
    touch_sentinel(PREFIX_SENTINEL);

    if (wait_for_sentinel(ACK0_SENTINEL, 100) < 0) {
        printf("writer(%s): FAIL reader never acked step 0\n", policy_name);
        return 1;
    }

    /* Step 2: physical_step(2) > min_acked(0) + reserve_slots(1) -- this is
     * the step the policy actually governs. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (write_one_step(fid, sp, 2) < 0) {
        printf("writer(%s): FAIL step 2\n", policy_name);
        return 1;
    }
    *out_step2_secs = elapsed_seconds(&t0);

    if (policy == H5VL_STREAM_QUEUE_SPILL) {
        /* Give the drain something to work with: wait for the reader's
         * second ack (min_acked becomes 1), then commit one more step --
         * H5VL__stream_apply_queue_policy() drains step 2 back into the
         * real file at the top of that call, before deciding step 3's own
         * fate. */
        if (wait_for_sentinel(ACK1_SENTINEL, 100) < 0) {
            printf("writer(%s): FAIL reader never acked step 1 (drain trigger)\n", policy_name);
            return 1;
        }
        if (write_one_step(fid, sp, 3) < 0) {
            printf("writer(%s): FAIL drain-trigger step 3\n", policy_name);
            return 1;
        }
    }

    H5Sclose(sp);
    H5Fclose(fid);
    H5Pclose(fapl);

    return 0;
}

/* Reopens fname natively (no vol-stream) and checks whether /step/2/val
 * exists with the value 2. */
static int
step2_is_real(const char *fname, int *out_present)
{
    hid_t rid, ds;
    int   val = -1;

    if ((rid = H5Fopen(fname, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0)
        return -1;

    if (H5Lexists(rid, "/step/2/val", H5P_DEFAULT) <= 0) {
        *out_present = 0;
        H5Fclose(rid);
        return 0;
    }

    if ((ds = H5Dopen2(rid, "/step/2/val", H5P_DEFAULT)) < 0) {
        H5Fclose(rid);
        return -1;
    }
    H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val);
    H5Dclose(ds);
    H5Fclose(rid);

    *out_present = (val == 2);
    return 0;
}

/* /step/2 itself must exist regardless of policy -- the reader index's hard
 * contiguity requirement (see H5VL__stream_discard_step()'s comment). */
static int
step2_group_exists(const char *fname, int *out_exists)
{
    hid_t rid;

    if ((rid = H5Fopen(fname, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0)
        return -1;
    *out_exists = (H5Lexists(rid, "/step/2", H5P_DEFAULT) > 0);
    H5Fclose(rid);
    return 0;
}

static int
run_scenario(hid_t vol_id, H5VL_stream_queue_policy_t policy, const char *policy_name)
{
    char   fname[64];
    pid_t  pid;
    int    reader_status = 0, writer_status = 0;
    double step2_secs = -1.0;
    int    nerrors = 0;

    printf("-- %s --\n", policy_name);

    snprintf(fname, sizeof(fname), "t_queue_policy_%s.h5", policy_name);
    unlink(PREFIX_SENTINEL);
    unlink(ACK0_SENTINEL);
    unlink(ACK1_SENTINEL);

    /* A default, not a requirement: the CI matrix runs this suite over more
     * than one Mercury NA plugin, so an externally-set VOL_STREAM_NA wins
     * (overwrite = 0). Shared memory stays the default because it needs no
     * network setup on a bare runner. */
    setenv("VOL_STREAM_NA", "na+sm", 0);

    fflush(NULL); /* the child inherits a copy of any unflushed stdio buffer -- avoid duplicated output */
    if ((pid = fork()) < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        int rc = run_reader(fname);

        fflush(NULL);
        _exit(rc);
    }

    writer_status = run_writer(vol_id, fname, policy, policy_name, pid, &step2_secs);

    if (waitpid(pid, &reader_status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    unlink(PREFIX_SENTINEL);
    unlink(ACK0_SENTINEL);
    unlink(ACK1_SENTINEL);

    if (writer_status != 0) {
        printf("  FAIL  writer process reported failure\n");
        nerrors++;
    }
    if (!(WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 0)) {
        printf("  FAIL  reader process reported failure (status=%d)\n", reader_status);
        nerrors++;
    }

    printf("  step 2's end_step() took %.3fs\n", step2_secs);

    {
        int group_exists = 0, present = 0;

        if (step2_group_exists(fname, &group_exists) < 0) {
            printf("  FAIL  could not reopen %s natively\n", fname);
            nerrors++;
        }
        else if (!group_exists) {
            printf("  FAIL  /step/2 does not exist -- contiguity broken\n");
            nerrors++;
        }

        if (step2_is_real(fname, &present) < 0) {
            printf("  FAIL  could not check /step/2/val\n");
            nerrors++;
        }
        else if (policy == H5VL_STREAM_QUEUE_BLOCK) {
            if (step2_secs < SLEEP_SECS * 0.5) {
                printf("  FAIL  Block returned too fast (%.3fs) -- did not wait for the reader\n",
                       step2_secs);
                nerrors++;
            }
            else
                printf("  ok    Block stalled the writer for the reader (%.3fs >= %.3fs)\n", step2_secs,
                       SLEEP_SECS * 0.5);
            if (!present) {
                printf("  FAIL  Block lost step 2's data\n");
                nerrors++;
            }
            else
                printf("  ok    Block lost nothing -- step 2's data is present\n");
        }
        else if (policy == H5VL_STREAM_QUEUE_DISCARD) {
            if (step2_secs >= SLEEP_SECS * 0.5) {
                printf("  FAIL  Discard waited (%.3fs) -- should have returned immediately\n", step2_secs);
                nerrors++;
            }
            else
                printf("  ok    Discard did not wait for the reader (%.3fs)\n", step2_secs);
            if (present) {
                printf("  FAIL  Discard did not drop step 2's data\n");
                nerrors++;
            }
            else
                printf("  ok    Discard dropped step 2's data, but /step/2 itself still exists\n");
        }
        else { /* SPILL */
            if (step2_secs >= SLEEP_SECS * 0.5) {
                printf("  FAIL  Spill waited (%.3fs) -- should have returned immediately\n", step2_secs);
                nerrors++;
            }
            else
                printf("  ok    Spill did not wait for the reader (%.3fs)\n", step2_secs);
            if (!present) {
                printf("  FAIL  Spill lost step 2's data -- did not drain\n");
                nerrors++;
            }
            else
                printf("  ok    Spill lost nothing either -- step 2 drained back once the reader caught "
                       "up\n");
        }
    }

    return nerrors ? 1 : 0;
}

int
main(void)
{
    hid_t vol_id;
    int   nerrors = 0;

    printf("vol-stream M7: queue policy under a deliberately slow reader (na+sm)\n");

    /* Registered once, in the parent (writer) process, and reused across all
     * three scenarios below -- HDF5's dynamic-optional-operation registry
     * rejects registering the same op string twice in one process, and the
     * writer is this same parent process for all three runs (only the
     * reader is a fresh fork() each time). */
    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("FAIL register\n");
        return 1;
    }

    /* A default, not a requirement: the CI matrix runs this suite over more
     * than one Mercury NA plugin, so an externally-set VOL_STREAM_NA wins
     * (overwrite = 0). Shared memory stays the default because it needs no
     * network setup on a bare runner. */
    setenv("VOL_STREAM_NA", "na+sm", 0);

    nerrors += run_scenario(vol_id, H5VL_STREAM_QUEUE_BLOCK, "block");
    nerrors += run_scenario(vol_id, H5VL_STREAM_QUEUE_DISCARD, "discard");
#ifdef VOL_STREAM_HAVE_BAKE
    nerrors += run_scenario(vol_id, H5VL_STREAM_QUEUE_SPILL, "spill");
#else
    printf("-- spill -- SKIPPED (connector built without VOL_STREAM_HAVE_BAKE)\n");
#endif

    H5VLclose(vol_id);

    if (nerrors) {
        printf("\n%d scenario(s) failed\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
