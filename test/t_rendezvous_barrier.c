/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The rendezvous barrier: H5Fwait_subscribers().
 *
 * A subscription only affects writes issued after it reaches the writer, so
 * a writer that commits step 0 before its readers have subscribed silently
 * gives them nothing. Every other two-process test in this suite works
 * around that with a ready-sentinel *file* the reader touches and the writer
 * polls -- which works, and quietly reintroduces the shared-filesystem
 * dependency that using SSG for membership was supposed to remove.
 *
 * This test is the protocol-level answer, and it is written to prove that
 * claim rather than merely exercise the call:
 *
 *   THERE IS NO READY-SENTINEL FILE ANYWHERE IN THIS TEST.
 *
 * The writer's only synchronization before its first step is
 * H5Fwait_subscribers(fid, 1, timeout), which blocks until one distinct
 * subscriber has registered, answered entirely from the writer's own
 * subscription table over the transport.
 *
 * Assertions:
 *
 *   1. The writer's H5Fwait_subscribers() returns success -- it actually saw
 *      the subscriber, rather than timing out and proceeding anyway.
 *   2. The reader receives the data for step 0 -- THE FIRST step. This is
 *      the assertion that fails without the barrier: without it the writer
 *      races ahead and step 0's push is lost, which is exactly the silent
 *      loss documented in the rendezvous section.
 *   3. The delivered content is correct.
 *
 * The reader deliberately sleeps before subscribing, so that a writer which
 * did not wait would reliably lose the race rather than win it by luck. That
 * is what makes assertion 2 load-bearing: remove the barrier and this test
 * must fail, not flake.
 *
 * Only compiled/run when VOL_STREAM_HAVE_MERCURY is on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NELEM 8
#define FNAME "t_rendezvous_barrier.h5"

/* How long the reader dawdles before subscribing. Comfortably longer than
 * the writer's create-plus-first-step path, so an unsynchronized writer
 * loses the race every time. */
#define READER_DELAY_US 700000

/* Generous: this bounds a real failure, not the expected path. */
#define BARRIER_TIMEOUT_MS 30000

static int
run_reader(void)
{
    hid_t    vol_id, fapl, fid, sub_space;
    hsize_t  dims = NELEM;
    int      i, rc = 0;
    uint64_t phys = (uint64_t)-1, elem_start = 0, elem_count = 0;
    char    *path = NULL;
    void    *buf  = NULL;
    size_t   size = 0;
    const char *paths[1];
    hid_t       spaces[1];

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("reader: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL fapl\n");
        return 1;
    }

    /* Wait for the group sidecar, the same bootstrap every other transport
     * test uses -- this is joining the SSG group, not synchronizing on step
     * readiness, and it is the writer's own published contact information
     * rather than a sentinel invented by the test. */
    for (i = 0; i < 200; i++) {
        FILE *f = fopen(FNAME ".vsgroup", "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(50000);
    }

    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open/join\n");
        return 1;
    }

    /* Dawdle. Without the writer's barrier this guarantees step 0 is gone
     * before the subscription lands. */
    usleep(READER_DELAY_US);

    if ((sub_space = H5Screate_simple(1, &dims, NULL)) < 0) {
        printf("reader: FAIL dataspace\n");
        return 1;
    }
    paths[0]  = "/live";
    spaces[0] = sub_space;

    if (H5Fsubscribe(fid, 1, paths, spaces, NULL) < 0) {
        printf("reader: FAIL subscribe\n");
        rc = 1;
        goto done;
    }

    /* No sentinel touch here. The writer is not watching the filesystem. */

    if (H5Fget_subscribed_data(fid, 20000, &phys, &path, &buf, &size, &elem_start, &elem_count) < 0) {
        printf("reader: FAIL no data pushed -- step 0 was lost\n");
        rc = 1;
        goto done;
    }

    /* Assertion 2: the data is for the FIRST step. */
    if (phys != 0) {
        printf("reader: FAIL expected step 0, got step %llu\n", (unsigned long long)phys);
        rc = 1;
    }
    else
        printf("reader: ok    received step 0 -- the first step was not lost\n");

    /* Assertion 3: content. */
    if (size != NELEM * sizeof(int) || elem_count != NELEM) {
        printf("reader: FAIL unexpected size %zu / count %llu\n", size, (unsigned long long)elem_count);
        rc = 1;
    }
    else {
        const int *v  = (const int *)buf;
        int        ok = 1;

        for (i = 0; i < NELEM; i++)
            if (v[i] != 100 + i)
                ok = 0;
        if (!ok) {
            printf("reader: FAIL content mismatch\n");
            rc = 1;
        }
        else
            printf("reader: ok    content correct\n");
    }

done:
    free(path);
    free(buf);
    H5Sclose(sub_space);
    H5Fclose(fid);
    H5Pclose(fapl);
    return rc;
}

static int
run_writer(void)
{
    hid_t   vol_id, fapl, fid, sp, ds;
    hsize_t dims = NELEM;
    int     buf[NELEM];
    int     i, rc = 0;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("writer: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0) {
        printf("writer: FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("writer: FAIL create\n");
        return 1;
    }

    /* Assertion 1: the whole point. No filesystem polling -- this blocks on
     * the writer's own subscription table, filled over the transport. */
    if (H5Fwait_subscribers(fid, 1, BARRIER_TIMEOUT_MS) < 0) {
        printf("writer: FAIL H5Fwait_subscribers timed out\n");
        rc = 1;
        goto done;
    }
    printf("writer: ok    barrier released by a real subscription\n");

    for (i = 0; i < NELEM; i++)
        buf[i] = 100 + i;

    sp = H5Screate_simple(1, &dims, NULL);

    /* Step 0 -- the one an unsynchronized writer would lose. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("writer: FAIL begin_step\n");
        rc = 1;
        goto done_sp;
    }
    if ((ds = H5Dcreate2(fid, "live", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        printf("writer: FAIL create dataset\n");
        rc = 1;
        goto done_sp;
    }
    if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) < 0) {
        printf("writer: FAIL write\n");
        rc = 1;
    }
    H5Dclose(ds);
    if (H5Fend_step(fid) < 0) {
        printf("writer: FAIL end_step\n");
        rc = 1;
    }

    /* Keep the group alive long enough for the reader to drain and close
     * first -- the close-ordering rule every two-process program here
     * follows. */
    usleep(1500000);

done_sp:
    H5Sclose(sp);
done:
    H5Fclose(fid);
    H5Pclose(fapl);
    return rc;
}

int
main(void)
{
    pid_t reader_pid;
    int   wstatus, reader_rc, writer_rc;

    printf("t_rendezvous_barrier: writer-side subscriber barrier (no sentinel file)\n");

    /* Default to shared memory, with the final 0 so an externally-set
     * VOL_STREAM_NA wins -- the same convention t_rendezvous.c and
     * t_subscribe.c use, and what lets ctest run this without needing the
     * variable in its own environment. */
    setenv("VOL_STREAM_NA", "na+sm", 0);

    remove(FNAME);
    remove(FNAME ".vsgroup");

    /* Drain stdout before forking: the child inherits a copy of whatever is
     * still buffered, and its own flush would print this header a second
     * time. */
    fflush(stdout);

    if ((reader_pid = fork()) < 0) {
        printf("  FAIL  fork\n");
        return 1;
    }
    if (reader_pid == 0) {
        int rc = run_reader();

        /* _exit() does not flush stdio, and this child's assertions are the
         * interesting half of the test -- without this its diagnostics are
         * lost and a failure prints nothing about why. */
        fflush(NULL);
        _exit(rc);
    }

    writer_rc = run_writer();

    if (waitpid(reader_pid, &wstatus, 0) < 0) {
        printf("  FAIL  waitpid\n");
        return 1;
    }
    reader_rc = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;

    remove(FNAME);
    remove(FNAME ".vsgroup");

    if (writer_rc || reader_rc) {
        printf("t_rendezvous_barrier: FAILED (writer=%d reader=%d)\n", writer_rc, reader_rc);
        return 1;
    }
    printf("t_rendezvous_barrier: all tests passed\n");
    return 0;
}
