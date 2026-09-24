/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * What flush and refresh mean on a stream, which is what reporting
 * H5VL_CAP_FLAG_FLUSH_REFRESH promises.
 *
 * The step is the unit of durability: an open step's writes are captured,
 * not yet in the file. So inside an open step, flush succeeds on every
 * object -- file, a live dataset, a dataset or group created in the step --
 * and does not commit the step, and refresh fails with a reason, since the
 * file does not yet hold anything to reload. A reader refreshes against the
 * step it is positioned on: the same values, and a virtual group succeeds.
 * Both of these used to fail outright, which the flag did not admit.
 *
 * No transport needed.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define N     4
#define FNAME "t_flush_refresh.h5"

static int nerrors = 0;

#define CHECK(cond, ...)                                                                                      \
    do {                                                                                                      \
        if (cond)                                                                                             \
            printf("  ok    " __VA_ARGS__);                                                                   \
        else {                                                                                                \
            printf("  FAIL  " __VA_ARGS__);                                                                   \
            nerrors++;                                                                                        \
        }                                                                                                     \
        printf("\n");                                                                                         \
    } while (0)

static int saw_reason = 0;

static herr_t
find_reason(unsigned n, const H5E_error2_t *err, void *udata)
{
    (void)n;
    (void)udata;
    if (err && err->desc && strstr(err->desc, "nothing to reload"))
        saw_reason = 1;
    return 0;
}

int
main(void)
{
    hid_t    fapl, fid, sp, ds, new_ds, grp, rfid, rds, rgrp;
    hsize_t  n = N;
    int      vals[N], got[N], i;
    uint64_t caps = 0;
    herr_t   r;

    printf("vol-stream: flush and refresh inside and outside a step\n");
    unlink(FNAME);
    fapl = H5Pcreate(H5P_FILE_ACCESS);
    if (H5Pset_fapl_stream(fapl, NULL) < 0 || (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (sp = H5Screate_simple(1, &n, NULL)) < 0) {
        printf("  FAIL  setup\n");
        return 1;
    }
    H5Pget_vol_cap_flags(fapl, &caps);
    CHECK(caps & H5VL_CAP_FLAG_FLUSH_REFRESH, "H5VL_CAP_FLAG_FLUSH_REFRESH is reported");

    for (i = 0; i < N; i++)
        vals[i] = i;
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (ds = H5Dcreate2(fid, "/x", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
        printf("  FAIL  step 0\n");
        return 1;
    }

    /* Step 1, open: a live dataset, and a dataset and group created in it. */
    for (i = 0; i < N; i++)
        vals[i] = 100 + i;
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 ||
        (new_ds = H5Dcreate2(fid, "/y", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        (grp = H5Gcreate2(fid, "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        printf("  FAIL  step 1 setup\n");
        return 1;
    }
    CHECK(H5Dflush(ds) >= 0, "H5Dflush() on a live dataset inside an open step succeeds");
    CHECK(H5Dflush(new_ds) >= 0, "H5Dflush() on a dataset created in the open step succeeds");
    CHECK(H5Gflush(grp) >= 0, "H5Gflush() on a group created in the open step succeeds");
    CHECK(H5Fflush(fid, H5F_SCOPE_GLOBAL) >= 0, "H5Fflush() inside an open step succeeds");
    H5E_BEGIN_TRY
    {
        r = H5Drefresh(ds);
    }
    H5E_END_TRY
    if (r < 0) /* before any other HDF5 call */
        H5Ewalk2(H5E_DEFAULT, H5E_WALK_DOWNWARD, find_reason, NULL);
    CHECK(r < 0 && saw_reason, "H5Drefresh() inside an open step fails, and says why");
    H5Eclear2(H5E_DEFAULT);

    /* The flushes did not commit step 1: it is still open. (Not checked by
     * opening the file natively here: a second handle in the same process
     * shares the file, and its close would flush it.) */
    {
        H5F_step_status_t st = H5F_STEP_NOT_IN_STEP;

        CHECK(H5Fstep_status(fid, &st) >= 0 && st == H5F_STEP_IN_STEP,
              "and none of them committed the open step");
    }
    H5Dclose(new_ds);
    H5Gclose(grp);
    CHECK(H5Fend_step(fid) >= 0, "the step then commits normally");
    H5Dclose(ds);
    H5Sclose(sp);
    H5Fclose(fid);

    /* Reader, positioned at step 0: refresh reloads that step. */
    if ((rfid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0 || H5Fbegin_step(rfid, 0, NULL, 0) < 0 ||
        (rds = H5Dopen2(rfid, "/x", H5P_DEFAULT)) < 0 || (rgrp = H5Gopen2(rfid, "/", H5P_DEFAULT)) < 0) {
        printf("  FAIL  reader setup\n");
        return 1;
    }
    CHECK(H5Drefresh(rds) >= 0 &&
              H5Dread(rds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) >= 0 && got[0] == 0,
          "a reader's H5Drefresh() reloads the step it is positioned on (step 0's value)");
    H5Dclose(rds);
    CHECK(H5Fbegin_step(rfid, 0, NULL, 0) >= 0 && (rds = H5Dopen2(rfid, "/x", H5P_DEFAULT)) >= 0 &&
              H5Dread(rds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) >= 0 && got[0] == 100,
          "and step 1, committed after the mid-step flushes, holds its own data");
    CHECK(H5Grefresh(rgrp) >= 0, "a reader's H5Grefresh() on a virtual group succeeds");
    H5Gclose(rgrp);
    H5Dclose(rds);
    H5Fclose(rfid);
    H5Pclose(fapl);
    unlink(FNAME);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
