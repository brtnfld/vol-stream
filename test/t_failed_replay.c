/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * A step whose replay fails is removed from the file, and the next step
 * commits in its place.
 *
 * H5Fend_step() does not advance the step number when replay fails, so the
 * next step is replayed into the same /step/<n>/. The failed replay's
 * objects were left there, so the next replay could collide with them
 * too -- and every step after it -- and the writer's path index still
 * named step n for objects the failed step had replayed.
 *
 * The failure is made by a real collision, not a test hook: outside any
 * step the application creates /step/1/d itself (passed straight through),
 * then step 1 writes /d, whose replay into /step/1/d fails. The retried
 * step writes /e.
 *
 * No transport needed.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_failed_replay.h5"

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

static int
step_with(hid_t fid, const char *path, hid_t sp, int val)
{
    int    v[4] = {val, val, val, val};
    hid_t  ds;
    herr_t r;

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (ds = H5Dcreate2(fid, path, H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, v) < 0)
        return -2;
    H5E_BEGIN_TRY
    {
        r = H5Fend_step(fid);
    }
    H5E_END_TRY
    H5Dclose(ds);
    return r < 0 ? -1 : 0;
}

int
main(void)
{
    hsize_t n = 4;
    hid_t   fapl, fid, sp, lcpl, blocker, nf;
    int     rc;

    printf("vol-stream: a step whose replay fails is removed, and the next commits in its place\n");
    unlink(FNAME);
    fapl = H5Pcreate(H5P_FILE_ACCESS);
    lcpl = H5Pcreate(H5P_LINK_CREATE);
    if (H5Pset_fapl_stream(fapl, NULL) < 0 || H5Pset_create_intermediate_group(lcpl, 1) < 0 ||
        (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (sp = H5Screate_simple(1, &n, NULL)) < 0 || step_with(fid, "/a", sp, 1) != 0) {
        printf("  FAIL  setup\n");
        return 1;
    }

    /* Outside a step, straight through to the file: the object step 1's
     * replay of /d will collide with. */
    if ((blocker = H5Dcreate2(fid, "/step/1/d", H5T_NATIVE_INT, sp, lcpl, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        printf("  FAIL  create the blocker\n");
        return 1;
    }
    H5Dclose(blocker);

    rc = step_with(fid, "/d", sp, 2);
    CHECK(rc == -1, "the step whose replay collides fails");

    rc = step_with(fid, "/e", sp, 3);
    CHECK(rc == 0, "the next step commits");
    rc = step_with(fid, "/f", sp, 4);
    CHECK(rc == 0, "and so does the one after it");

    H5Sclose(sp);
    H5Pclose(lcpl);
    H5Fclose(fid);

    if ((nf = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  native open\n");
        return 1;
    }
    {
        htri_t e1, d1, f2;

        H5E_BEGIN_TRY
        {
            e1 = H5Lexists(nf, "/step/1", H5P_DEFAULT) > 0 && H5Lexists(nf, "/step/1/e", H5P_DEFAULT) > 0;
            d1 = H5Lexists(nf, "/step/1", H5P_DEFAULT) > 0 && H5Lexists(nf, "/step/1/d", H5P_DEFAULT) > 0;
            f2 = H5Lexists(nf, "/step/2", H5P_DEFAULT) > 0 && H5Lexists(nf, "/step/2/f", H5P_DEFAULT) > 0;
        }
        H5E_END_TRY
        CHECK(e1 && f2 && !d1, "natively, step 1 is the retried step (/e), step 2 is /f, and nothing of the failed one "
                               "remains");
    }
    H5Fclose(nf);

    /* Through the connector: step 1 is /e, and /d never existed. */
    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0 || H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  reader open\n");
        return 1;
    }
    {
        hid_t e, d;

        /* H5Dopen2(), not H5Lexists(): a step reader resolves opens to the
         * step, but not link queries (dev-plan.md). */
        H5E_BEGIN_TRY
        {
            e = H5Dopen2(fid, "/e", H5P_DEFAULT);
            d = H5Dopen2(fid, "/d", H5P_DEFAULT);
        }
        H5E_END_TRY
        CHECK(e >= 0 && d < 0, "a connector reader at step 1 opens /e, and /d does not exist");
        if (e >= 0)
            H5Dclose(e);
        if (d >= 0)
            H5Dclose(d);
    }
    H5Fclose(fid);
    H5Pclose(fapl);
    unlink(FNAME);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
