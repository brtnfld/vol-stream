/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M4: deferred write/attr-write requests.
 *
 * A dataset/attribute write captured into an open step (dev-plan.md's
 * "H5VL_request_class_t implemented so writes queue and resolve at
 * end_step") returns a request object via H5Dwrite_async()/H5Awrite_async()
 * whose completion tracks durability, not buffer safety -- the payload is
 * already copied out of the caller's buffer by the time the call returns
 * either way (see H5VL__stream_make_deferred_request()'s comment in
 * src/H5VLstream.c). What this test proves:
 *
 *   1. A write issued while a step is open reports IN_PROGRESS through
 *      H5ESwait() with a zero timeout, before end_step() runs.
 *   2. The same request reports done (no error) through H5ESwait() once
 *      end_step() has resolved the step.
 *   3. The data landed correctly regardless -- deferred tracking is purely
 *      additive over M2/M3's existing capture/replay.
 *
 * This does not exercise Mercury at all: deferred requests resolve against
 * the local step-completion cell (see H5VL_stream_step_completion_t),
 * independent of whether the transport is enabled. See test/t_transport.c
 * for the Mercury/Margo cross-process round trip.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NELEM 32

static int nerrors = 0;

#define EXPECT(cond, what)                                                                                  \
    do {                                                                                                    \
        if (!(cond)) {                                                                                       \
            printf("  FAIL  %s (%s:%d)\n", (what), __FILE__, __LINE__);                                     \
            nerrors++;                                                                                       \
        }                                                                                                    \
        else                                                                                                 \
            printf("  ok    %s\n", (what));                                                                 \
    } while (0)

#define CHECK(expr, what)                                                                                    \
    do {                                                                                                     \
        if ((expr) < 0) {                                                                                    \
            printf("  FAIL  %s (%s:%d)\n", (what), __FILE__, __LINE__);                                      \
            nerrors++;                                                                                       \
            return -1;                                                                                       \
        }                                                                                                     \
    } while (0)

static int
run(void)
{
    hid_t   vol_id, fapl, fid, sp, ds, es;
    hsize_t dims[1]   = {NELEM};
    int     wbuf[NELEM], rbuf[NELEM];
    size_t  count;
    bool    err_occurred;
    size_t  num_in_progress;
    int     i;

    for (i = 0; i < NELEM; i++)
        wbuf[i] = i * 7 + 3;

    CHECK((vol_id = H5VL_stream_register()), "register vol-stream");

    CHECK((fapl = H5Pcreate(H5P_FILE_ACCESS)), "create fapl");
    CHECK(H5Pset_vol(fapl, vol_id, NULL), "set vol on fapl");
    CHECK((fid = H5Fcreate("t_deferred.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl)), "create file");

    CHECK((sp = H5Screate_simple(1, dims, NULL)), "create dataspace");
    CHECK((es = H5EScreate()), "create event set");

    CHECK(H5Fbegin_step(fid, 0, NULL, 0), "begin_step");
    CHECK((ds = H5Dcreate2(fid, "d", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)),
          "create dataset (deferred placeholder)");

    CHECK(H5Dwrite_async(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, wbuf, es),
          "deferred write returns success");

    CHECK(H5ESget_count(es, &count), "get event set count");
    EXPECT(count == 1, "event set is tracking exactly one request");

    /* Zero timeout: a non-blocking poll. The step is still open, so the
     * shared completion cell has not been resolved -- this must report the
     * op still in progress, not silently drop it or report it done. */
    CHECK(H5ESwait(es, 0, &num_in_progress, &err_occurred), "poll event set before end_step");
    EXPECT(num_in_progress == 1, "request is IN_PROGRESS before end_step");
    EXPECT(!err_occurred, "no error reported while pending");

    CHECK(H5Dclose(ds), "close dataset before end_step");
    CHECK(H5Fend_step(fid), "end_step resolves the deferred request");

    /* Now resolved -- a generous timeout that should return immediately. */
    CHECK(H5ESwait(es, (uint64_t)5000000000ULL, &num_in_progress, &err_occurred), "wait after end_step");
    EXPECT(num_in_progress == 0, "request completed after end_step");
    EXPECT(!err_occurred, "no error reported on completion");

    CHECK(H5ESclose(es), "close event set");
    CHECK(H5Sclose(sp), "close dataspace");
    CHECK(H5Fclose(fid), "close file");
    CHECK(H5Pclose(fapl), "close fapl");

    /* Data correctness: deferred tracking must not change what M2/M3 already
     * replay under /step/0/d. */
    {
        hid_t rid, rds;

        CHECK((rid = H5Fopen("t_deferred.h5", H5F_ACC_RDONLY, H5P_DEFAULT)), "reopen natively");
        CHECK((rds = H5Dopen2(rid, "/step/0/d", H5P_DEFAULT)), "open replayed dataset");
        CHECK(H5Dread(rds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rbuf), "read replayed data");
        EXPECT(memcmp(wbuf, rbuf, sizeof(wbuf)) == 0, "replayed data matches what was written");
        CHECK(H5Dclose(rds), "close replayed dataset");
        CHECK(H5Fclose(rid), "close native reopen");
    }

    H5VLclose(vol_id);
    return 0;
}

int
main(void)
{
    printf("vol-stream M4: deferred write/attr-write requests\n");

    if (run() < 0)
        return 1;

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
