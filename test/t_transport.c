/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M4 exit gate (the part exercised without ofi+tcp/multi-host CI, which this
 * suite cannot drive): a writer and a reader, in two separate OS processes,
 * connected over Mercury on the na+sm plugin -- the first vol-stream test
 * where the two roles are not one process opening its own output file (M3's
 * "single process both sides" scope). Rendezvous is now M5's SSG group join
 * rather than M4's original hand-rolled attach RPC, but the round trip this
 * test proves is unchanged; see test/t_rendezvous.c for M5's own exit gate
 * (late joiners, a departing reader, a writer that starts with none).
 *
 * Only compiled/run when VOL_STREAM_HAVE_MERCURY is on; see
 * test/CMakeLists.txt.
 *
 * Sequence:
 *   1. The writer H5Fcreate()s (starts Margo, creates its SSG group, and
 *      stores its id to t_transport.h5.vsgroup) and then waits, bounded,
 *      for a sentinel file the reader writes right after joining --
 *      otherwise the writer's first end_step() could broadcast step_ready
 *      before the reader has joined the group.
 *   2. The writer commits NSTEPS steps, each with a distinct wall_time_ns.
 *   3. The reader calls H5Fwait_step_ready() once per step and checks the
 *      physical step numbers arrive in order with the right wall_time_ns --
 *      proving the Mercury/Margo round trip, not just that the API compiles.
 *   4. Both processes close; the parent re-opens the file fresh (a new,
 *      unrelated file_state) and confirms the replayed data itself is
 *      correct -- decoupled from the live-notification proof above, same
 *      split as test/t_deferred.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NSTEPS         5
#define FNAME          "t_transport.h5"
#define READY_SENTINEL "t_transport.reader_ready"
#define DONE_SENTINEL  "t_transport.reader_done"

static int
run_writer(void)
{
    hid_t vol_id, fapl, fid, sp;
    int   s;

    unlink(FNAME ".vsgroup");

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("writer: FAIL register\n");
        return 1;
    }
    /* HDF5's default file locking assumes one process at a time; without
     * SWMR (out of scope here -- see H5VLstream.h's M3 note on readers), a
     * concurrent reader's open would otherwise fail with EAGAIN. Safe to
     * disable here: the writer only ever appends new /step/<n>/ groups, and
     * a reader only reads a step after end_step() commits it, so there is
     * no overlapping mutation for locking to protect against. */
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("writer: FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("writer: FAIL create (transport up? VOL_STREAM_NA set?)\n");
        return 1;
    }

    /* Bounded wait for the reader's attach -- a reader that never shows up
     * must not hang this test forever. */
    for (s = 0; s < 100; s++) {
        FILE *f = fopen(READY_SENTINEL, "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000); /* 100ms */
    }
    if (s == 100)
        printf("writer: WARNING reader never signaled ready; proceeding anyway\n");

    if ((sp = H5Screate(H5S_SCALAR)) < 0) {
        printf("writer: FAIL create dataspace\n");
        return 1;
    }

    for (s = 0; s < NSTEPS; s++) {
        char           name[32];
        const uint64_t logical = 100 + (uint64_t)s;
        const uint64_t wall_ns = (uint64_t)(s + 1) * 1000;
        hid_t          ds;
        int            val = s;

        snprintf(name, sizeof(name), "d%d", s);

        if (H5Fbegin_step(fid, 1, &logical, wall_ns) < 0) {
            printf("writer: FAIL begin_step %d\n", s);
            return 1;
        }
        if ((ds = H5Dcreate2(fid, name, H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
            printf("writer: FAIL create dataset %d\n", s);
            return 1;
        }
        if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val) < 0) {
            printf("writer: FAIL write %d\n", s);
            H5Dclose(ds);
            return 1;
        }
        H5Dclose(ds);
        if (H5Fend_step(fid) < 0) {
            printf("writer: FAIL end_step %d (should broadcast step_ready)\n", s);
            return 1;
        }
    }

    /* Bounded wait for the reader to finish consuming and close: otherwise
     * this process's own H5Fclose() (which destroys the SSG group) can race
     * the reader's H5Fclose() (which leaves it gracefully), producing
     * harmless but noisy "group not found" errors on whichever side loses.
     * Not required for correctness -- vs_tr_stop() tears down cleanly
     * either way -- just for a clean run. */
    for (s = 0; s < 100; s++) {
        FILE *f = fopen(DONE_SENTINEL, "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000); /* 100ms */
    }

    H5Sclose(sp);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}

static int
run_reader(void)
{
    hid_t vol_id, fapl, fid;
    int   s, rc = 0;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("reader: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL fapl\n");
        return 1;
    }

    /* Wait for the writer's SSG group-id sidecar file before calling
     * H5Fopen() at all: H5VL__stream_transport_start_writer() only
     * publishes it after flushing the underlying file, so its presence is
     * proof the file is safe to open. H5Fopen()'s own internal group-join
     * retry (see H5VL__stream_transport_start_reader() in
     * src/H5VLstream.c) runs only *after* the underlying native open
     * already succeeded, so it cannot by itself absorb the race against
     * the writer's H5Fcreate(). */
    {
        int i;

        for (i = 0; i < 100; i++) {
            FILE *f = fopen(FNAME ".vsgroup", "r");

            if (f) {
                fclose(f);
                break;
            }
            usleep(100000); /* 100ms */
        }
    }

    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open (writer not there yet? transport up?)\n");
        return 1;
    }

    /* The group join was attempted synchronously inside the H5Fopen()
     * above -- tell the writer it can proceed. */
    {
        FILE *f = fopen(READY_SENTINEL, "w");

        if (f)
            fclose(f);
    }

    for (s = 0; s < NSTEPS; s++) {
        uint64_t phys = (uint64_t)-1, wall_ns = 0;

        if (H5Fwait_step_ready(fid, 10000 /* 10s */, &phys, &wall_ns) < 0) {
            printf("  FAIL  step_ready notification %d (timed out)\n", s);
            rc = 1;
            break;
        }
        if (phys != (uint64_t)s) {
            printf("  FAIL  step_ready out of order: got %llu, expected %d\n", (unsigned long long)phys, s);
            rc = 1;
            continue;
        }
        if (wall_ns != (uint64_t)(s + 1) * 1000) {
            printf("  FAIL  step %d: wall_time_ns mismatch, got %llu\n", s, (unsigned long long)wall_ns);
            rc = 1;
            continue;
        }
        printf("  ok    step_ready for physical step %d (wall_time_ns=%llu)\n", s,
               (unsigned long long)wall_ns);
    }

    /* Tell the writer it is safe to close (see run_writer()'s matching wait
     * for why). */
    {
        FILE *f = fopen(DONE_SENTINEL, "w");

        if (f)
            fclose(f);
    }

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
    int   nerrors = 0;

    printf("vol-stream M4: two-process step_ready round trip over Mercury (na+sm)\n");

    setenv("VOL_STREAM_NA", "na+sm", 1);
    unlink(READY_SENTINEL);
    unlink(DONE_SENTINEL);

    if ((pid = fork()) < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* _exit() skips stdio flushing -- without this, the reader's own
         * printf()s (its per-step "ok"/"FAIL" lines) are silently lost
         * whenever stdout is fully buffered rather than line buffered,
         * e.g. under ctest or a pipe. */
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
    unlink(DONE_SENTINEL);

    if (writer_status != 0) {
        printf("\nwriter process reported failure\n");
        nerrors++;
    }
    if (!WIFEXITED(reader_status) || WEXITSTATUS(reader_status) != 0) {
        printf("\nreader process reported failure\n");
        nerrors++;
    }

    /* Data correctness, decoupled from the live notification above: reopen
     * fresh (a brand new file_state, its own index) and confirm the
     * replayed data is right. */
    {
        hid_t rid;

        if ((rid = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
            printf("  FAIL  reopen natively\n");
            nerrors++;
        }
        else {
            int s;

            for (s = 0; s < NSTEPS; s++) {
                char path[64];
                int  val = -1;
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
                else
                    printf("  ok    %s data correct\n", path);
                H5Dclose(ds);
            }
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
