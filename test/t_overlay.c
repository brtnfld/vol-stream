/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The /stream overlay: a timeline view of each dataset for tools that read
 * the file natively (h5py, h5dump, H5Web), checked here natively.
 *
 *   /T    int[3], written in steps 0, 1 and 3 -- not 2. /stream/T is [4][3],
 *         and row 2 holds step 1's values: the state as of step 2.
 *   /g/U  double[2], first written in step 1. /stream/g/U is [3][2], rows
 *         steps 1..3, with first_step = 1.
 *   /grow extendible: its shape can change, so it gets no overlay.
 *
 * And with the overlay off (the default) there is no /stream at all.
 * No transport needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_overlay.h5"

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
write_file(int overlay)
{
    H5VL_stream_config_t cfg;
    hid_t                fapl, fid, s3, s2, sg, dcpl, t, u = H5I_INVALID_HID, g = H5I_INVALID_HID;
    hsize_t              n3 = 3, n2 = 2, one = 1, unlim = H5S_UNLIMITED;
    int                  tv[3], gv = 0, s, i;
    double               uv[2];

    H5VL_stream_config_init(&cfg);
    cfg.overlay = overlay;
    fapl        = H5Pcreate(H5P_FILE_ACCESS);
    dcpl        = H5Pcreate(H5P_DATASET_CREATE);
    if (H5Pset_fapl_stream(fapl, &cfg) < 0 || (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (s3 = H5Screate_simple(1, &n3, NULL)) < 0 || (s2 = H5Screate_simple(1, &n2, NULL)) < 0 ||
        (sg = H5Screate_simple(1, &one, &unlim)) < 0 || H5Pset_chunk(dcpl, 1, &one) < 0)
        return -1;

    for (s = 0; s < 4; s++) {
        for (i = 0; i < 3; i++)
            tv[i] = s * 10 + i;
        uv[0] = s + 0.5;
        uv[1] = s + 0.25;
        gv    = s;
        if (H5Fbegin_step(fid, 0, NULL, 0) < 0)
            return -1;
        if (s == 0 && ((t = H5Dcreate2(fid, "/T", H5T_NATIVE_INT, s3, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
                       (g = H5Dcreate2(fid, "/grow", H5T_NATIVE_INT, sg, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0))
            return -1;
        if (s == 1 && (u = H5Dcreate2(fid, "/g/U", H5T_NATIVE_DOUBLE, s2, H5P_DEFAULT, H5P_DEFAULT,
                                      H5P_DEFAULT)) < 0)
            return -1;
        if (s != 2 && H5Dwrite(t, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, tv) < 0)
            return -1;
        if (s >= 1 && H5Dwrite(u, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, uv) < 0)
            return -1;
        if (H5Dwrite(g, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &gv) < 0 || H5Fend_step(fid) < 0)
            return -1;
    }
    H5Dclose(t);
    H5Dclose(u);
    H5Dclose(g);
    H5Sclose(s3);
    H5Sclose(s2);
    H5Sclose(sg);
    H5Pclose(dcpl);
    H5Pclose(fapl);
    return H5Fclose(fid);
}

int
main(void)
{
    hid_t   nf, ds, sp, at;
    hsize_t dims[2] = {0, 0};
    int     tv[4][3], ok, s, i;
    double  uv[3][2];
    uint64_t first = 99;

    printf("vol-stream: the /stream overlay, read natively\n");
    unsetenv("VOL_STREAM_OVERLAY");

    /* Off: no /stream. */
    unlink(FNAME);
    if (write_file(-1) < 0 || (nf = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  write or reopen (overlay off)\n");
        return 1;
    }
    CHECK(H5Lexists(nf, "/stream", H5P_DEFAULT) <= 0, "with the overlay off there is no /stream");
    H5Fclose(nf);

    /* On. */
    unlink(FNAME);
    if (write_file(1) < 0 || (nf = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  write or reopen (overlay on)\n");
        return 1;
    }

    if ((ds = H5Dopen2(nf, "/stream/T", H5P_DEFAULT)) < 0) {
        printf("  FAIL  /stream/T missing\n");
        return 1;
    }
    sp = H5Dget_space(ds);
    H5Sget_simple_extent_dims(sp, dims, NULL);
    CHECK(dims[0] == 4 && dims[1] == 3, "/stream/T is [%llu][%llu], one row per step",
          (unsigned long long)dims[0], (unsigned long long)dims[1]);
    ok = H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, tv) >= 0;
    for (s = 0; ok && s < 4; s++)
        for (i = 0; i < 3; i++)
            ok = ok && tv[s][i] == (s == 2 ? 1 : s) * 10 + i;
    CHECK(ok, "its rows are steps 0-3, and step 2, which did not write /T, holds step 1's values");
    H5Sclose(sp);
    H5Dclose(ds);

    if ((ds = H5Dopen2(nf, "/stream/g/U", H5P_DEFAULT)) < 0) {
        printf("  FAIL  /stream/g/U missing\n");
        return 1;
    }
    sp = H5Dget_space(ds);
    H5Sget_simple_extent_dims(sp, dims, NULL);
    at = H5Aopen(ds, "first_step", H5P_DEFAULT);
    CHECK(dims[0] == 3 && at >= 0 && H5Aread(at, H5T_NATIVE_UINT64, &first) >= 0 && first == 1,
          "/stream/g/U, first written in step 1, has 3 rows and first_step = 1");
    ok = H5Dread(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, uv) >= 0;
    for (s = 0; ok && s < 3; s++)
        ok = uv[s][0] == (s + 1) + 0.5 && uv[s][1] == (s + 1) + 0.25;
    CHECK(ok, "and its rows hold steps 1-3");
    if (at >= 0)
        H5Aclose(at);
    H5Sclose(sp);
    H5Dclose(ds);

    CHECK(H5Lexists(nf, "/stream/grow", H5P_DEFAULT) <= 0, "an extendible dataset gets no overlay");
    H5Fclose(nf);
    if (!getenv("T_OVERLAY_KEEP")) /* set it to look at the file with h5dump or H5Web */
        unlink(FNAME);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
