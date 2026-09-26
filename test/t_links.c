/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Links made in a step go into the stream.
 *
 * A NeXus writer links /entry/data/data to its detector dataset, usually in
 * the step that creates the dataset. That went straight to the live
 * namespace, where the target does not exist -- a dataset lives only under
 * /step/<k>/ -- so the hard link failed and a soft link dangled.
 *
 *   step 0  /entry/instrument/detector/data created and written (frame 0);
 *           /entry/data/data a hard link to it, /entry/data/soft a soft one
 *   step 1  the detector dataset rewritten (frame 1)
 *   step 2  only /other
 *
 * Natively, /step/0 has both links on frame 0, /step/1 the hard link on
 * frame 1 (carried, since step 1 wrote the target), /step/2 none; the live
 * namespace has no link. A connector reader opening /entry/data/data gets
 * frame 0, 1, 1 at steps 0, 1, 2. The /stream overlay covers the dataset
 * once, not again under the link's name.
 *
 * No transport needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_links.h5"
#define DET   "/entry/instrument/detector/data"
#define N     4

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

/* First element of dataset path through f, or -1 (the error stack quiet). */
static int
first(hid_t f, const char *path)
{
    hid_t ds;
    int   v[N] = {-1};

    H5E_BEGIN_TRY
    {
        ds = H5Dopen2(f, path, H5P_DEFAULT);
    }
    H5E_END_TRY
    if (ds < 0)
        return -1;
    if (H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, v) < 0)
        v[0] = -1;
    H5Dclose(ds);
    return v[0];
}

static H5L_type_t
link_type(hid_t f, const char *path)
{
    H5L_info2_t li;
    herr_t      r;

    H5E_BEGIN_TRY
    {
        r = H5Lget_info2(f, path, &li, H5P_DEFAULT);
    }
    H5E_END_TRY
    return r < 0 ? H5L_TYPE_ERROR : li.type;
}

static int
write_file(hid_t fapl)
{
    hsize_t n = N;
    int     v[N], i;
    hid_t   fid, lcpl, sp, det, other;

    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (lcpl = H5Pcreate(H5P_LINK_CREATE)) < 0 || H5Pset_create_intermediate_group(lcpl, 1) < 0 ||
        (sp = H5Screate_simple(1, &n, NULL)) < 0)
        return -1;
    H5Gclose(H5Gcreate2(fid, "/entry/data", lcpl, H5P_DEFAULT, H5P_DEFAULT));

    for (i = 0; i < N; i++)
        v[i] = 100 + i;
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (det = H5Dcreate2(fid, DET, H5T_NATIVE_INT, sp, lcpl, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(det, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, v) < 0) {
        printf("  FAIL  step 0 setup\n");
        return -1;
    }
    CHECK(H5Lcreate_hard(fid, DET, fid, "/entry/data/data", H5P_DEFAULT, H5P_DEFAULT) >= 0 &&
              H5Lcreate_soft(DET, fid, "/entry/data/soft", H5P_DEFAULT, H5P_DEFAULT) >= 0,
          "a hard and a soft link to a dataset created in the open step are accepted");
    if (H5Fend_step(fid) < 0)
        return -1;

    for (i = 0; i < N; i++)
        v[i] = 200 + i;
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || H5Dwrite(det, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, v) < 0 ||
        H5Fend_step(fid) < 0)
        return -1;

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (other = H5Dcreate2(fid, "/other", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(other, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, v) < 0 || H5Fend_step(fid) < 0)
        return -1;

    H5Dclose(other);
    H5Dclose(det);
    H5Sclose(sp);
    H5Pclose(lcpl);
    return H5Fclose(fid);
}

int
main(void)
{
    H5VL_stream_config_t cfg;
    hid_t                fapl, nf, rf;
    int                  s, want[3] = {100, 200, 200}, ok = 1, exists_ok = 1;

    printf("vol-stream: links made in a step go into the stream\n");
    unlink(FNAME);
    unsetenv("VOL_STREAM_OVERLAY");
    H5VL_stream_config_init(&cfg);
    cfg.overlay = 1;
    fapl        = H5Pcreate(H5P_FILE_ACCESS);
    if (H5Pset_fapl_stream(fapl, &cfg) < 0 || write_file(fapl) < 0) {
        printf("  FAIL  write\n");
        return 1;
    }

    if ((nf = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  native open\n");
        return 1;
    }
    CHECK(link_type(nf, "/step/0/entry/data/data") == H5L_TYPE_HARD && first(nf, "/step/0/entry/data/data") == 100,
          "/step/0/entry/data/data is a hard link to frame 0");
    CHECK(link_type(nf, "/step/0/entry/data/soft") == H5L_TYPE_SOFT && first(nf, "/step/0/entry/data/soft") == 100,
          "/step/0/entry/data/soft is a soft link that resolves to frame 0");
    CHECK(first(nf, "/step/1/entry/data/data") == 200, "step 1 rewrote the target, and carries the link to frame 1");
    CHECK(link_type(nf, "/step/2/entry/data/data") == H5L_TYPE_ERROR, "step 2 did not write it, and has no link");
    CHECK(link_type(nf, "/entry/data/data") == H5L_TYPE_ERROR && link_type(nf, "/entry/data/soft") == H5L_TYPE_ERROR,
          "the live namespace has no link, dangling or otherwise");
    CHECK(H5Lexists(nf, "/stream/entry/instrument/detector/data", H5P_DEFAULT) > 0 &&
              link_type(nf, "/stream/entry/data/data") == H5L_TYPE_ERROR,
          "the overlay covers the dataset once, not again under the link's name");
    H5Fclose(nf);

    if ((rf = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("  FAIL  reader open\n");
        return 1;
    }
    for (s = 0; s < 3; s++) {
        int got;

        if (H5Fbegin_step(rf, 0, NULL, 0) < 0) {
            printf("  FAIL  reader step %d\n", s);
            return 1;
        }
        got = first(rf, "/entry/data/data");
        if (got != want[s]) {
            printf("  FAIL  a reader at step %d opened /entry/data/data and got %d, expected %d\n", s, got, want[s]);
            ok = 0;
        }
        /* H5Lexists() resolves to the step as opens do: /other exists from
         * step 2 only; the link, the dataset and the live group throughout. */
        if (H5Lexists(rf, "/entry/data/data", H5P_DEFAULT) <= 0 || H5Lexists(rf, DET, H5P_DEFAULT) <= 0 ||
            H5Lexists(rf, "/entry/data", H5P_DEFAULT) <= 0 ||
            (H5Lexists(rf, "/other", H5P_DEFAULT) > 0) != (s == 2)) {
            printf("  FAIL  at step %d a reader's H5Lexists() answers wrongly\n", s);
            exists_ok = 0;
        }
    }
    CHECK(ok, "a connector reader opening /entry/data/data gets frame 0, 1, 1 at steps 0, 1, 2");
    CHECK(exists_ok, "and its H5Lexists() resolves to the step too (/other only from step 2)");
    H5Fclose(rf);
    H5Pclose(fapl);
    unlink(FNAME);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
