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
 *   /grow extendible 1-D: a growing series gets no overlay.
 *   /det  a detector frame stack [nP][2][3], created empty and extended by
 *         the frames each step writes: one in steps 0 and 1, none in 2, two
 *         in 3. /stream/det is [4][2][3] -- a row per frame, not per step --
 *         and each row holds its frame, though no step's copy holds them all.
 *
 * Each is an NXdata group, /stream<path>, whose "data" is the view and whose
 * "step" axis says which step each row came from; /stream is an NXentry
 * whose @default is the first of them. So NeXus tools (silx, H5Web) plot it.
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

#define FR 2
#define FC 3

static int
frame_val(int k, int e)
{
    return 1000 * k + e;
}

/* Extend /det to nframes and write frames first..nframes-1. */
static int
write_frames(hid_t det, int first, int nframes)
{
    hsize_t ext[3] = {(hsize_t)nframes, FR, FC}, start[3] = {(hsize_t)first, 0, 0};
    hsize_t count[3] = {(hsize_t)(nframes - first), FR, FC};
    int     buf[4 * FR * FC], k, e;
    hid_t   fs, ms;

    for (k = first; k < nframes; k++)
        for (e = 0; e < FR * FC; e++)
            buf[(k - first) * FR * FC + e] = frame_val(k, e);
    if (H5Dset_extent(det, ext) < 0 || (fs = H5Dget_space(det)) < 0 ||
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, start, NULL, count, NULL) < 0 ||
        (ms = H5Screate_simple(3, count, NULL)) < 0 ||
        H5Dwrite(det, H5T_NATIVE_INT, ms, fs, H5P_DEFAULT, buf) < 0)
        return -1;
    H5Sclose(ms);
    return H5Sclose(fs);
}

/* A string attribute's value (scalar, or element i of an array), or "". */
static const char *
str_attr(hid_t nf, const char *obj, const char *name, int i)
{
    static char out[64];
    hid_t       a, st, sp;
    char       *v[8] = {NULL};
    hssize_t    n;

    out[0] = '\0';
    if ((a = H5Aopen_by_name(nf, obj, name, H5P_DEFAULT, H5P_DEFAULT)) < 0)
        return out;
    st = H5Tcopy(H5T_C_S1);
    H5Tset_size(st, H5T_VARIABLE);
    H5Tset_cset(st, H5T_CSET_UTF8); /* as NeXus writers (h5py) store them */
    sp = H5Aget_space(a);
    n  = H5Sget_simple_extent_npoints(sp);
    if (n > 0 && n <= 8 && i < n && H5Aread(a, st, v) >= 0) {
        snprintf(out, sizeof(out), "%s", v[i] ? v[i] : "");
        H5Treclaim(st, sp, H5P_DEFAULT, v);
    }
    H5Sclose(sp);
    H5Tclose(st);
    H5Aclose(a);
    return out;
}

/* /stream<path>/step: its n values match want. */
static int
axis_is(hid_t nf, const char *path, const uint64_t *want, int n)
{
    char     name[128];
    uint64_t got[8];
    hsize_t  d = 0;
    hid_t    ds, sp;
    int      i, ok;

    snprintf(name, sizeof(name), "/stream%s/step", path);
    if ((ds = H5Dopen2(nf, name, H5P_DEFAULT)) < 0)
        return 0;
    sp = H5Dget_space(ds);
    H5Sget_simple_extent_dims(sp, &d, NULL);
    ok = d == (hsize_t)n && n <= 8 && H5Dread(ds, H5T_NATIVE_UINT64, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) >= 0;
    for (i = 0; ok && i < n; i++)
        ok = got[i] == want[i];
    H5Sclose(sp);
    H5Dclose(ds);
    return ok;
}

static int
write_file(int overlay)
{
    H5VL_stream_config_t cfg;
    hid_t                fapl, fid, s3, s2, sg, dcpl, t, u = H5I_INVALID_HID, g = H5I_INVALID_HID;
    hid_t                sd, ddcpl, det = H5I_INVALID_HID;
    hsize_t              d0[3] = {0, FR, FC}, dmax[3] = {H5S_UNLIMITED, FR, FC}, dchunk[3] = {1, FR, FC};
    hsize_t              n3 = 3, n2 = 2, one = 1, unlim = H5S_UNLIMITED;
    int                  tv[3], gv = 0, s, i;
    double               uv[2];

    H5VL_stream_config_init(&cfg);
    cfg.overlay = overlay;
    fapl        = H5Pcreate(H5P_FILE_ACCESS);
    dcpl        = H5Pcreate(H5P_DATASET_CREATE);
    if (H5Pset_fapl_stream(fapl, &cfg) < 0 || (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (s3 = H5Screate_simple(1, &n3, NULL)) < 0 || (s2 = H5Screate_simple(1, &n2, NULL)) < 0 ||
        (sg = H5Screate_simple(1, &one, &unlim)) < 0 || H5Pset_chunk(dcpl, 1, &one) < 0 ||
        (sd = H5Screate_simple(3, d0, dmax)) < 0 || (ddcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
        H5Pset_chunk(ddcpl, 3, dchunk) < 0)
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
        if (s == 0 &&
            ((det = H5Dcreate2(fid, "/det", H5T_NATIVE_INT, sd, H5P_DEFAULT, ddcpl, H5P_DEFAULT)) < 0 ||
             write_frames(det, 0, 1) < 0))
            return -1;
        if ((s == 1 && write_frames(det, 1, 2) < 0) || (s == 3 && write_frames(det, 2, 4) < 0))
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
    H5Dclose(det);
    H5Sclose(sd);
    H5Pclose(ddcpl);
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

    if ((ds = H5Dopen2(nf, "/stream/T/data", H5P_DEFAULT)) < 0) {
        printf("  FAIL  /stream/T/data missing\n");
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

    if ((ds = H5Dopen2(nf, "/stream/g/U/data", H5P_DEFAULT)) < 0) {
        printf("  FAIL  /stream/g/U/data missing\n");
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

    CHECK(H5Lexists(nf, "/stream/grow", H5P_DEFAULT) <= 0, "an extendible 1-D dataset gets no overlay");

    /* NeXus: what silx and H5Web follow to a default plot. */
    CHECK(!strcmp(str_attr(nf, "/stream", "NX_class", 0), "NXentry") &&
              !strcmp(str_attr(nf, "/stream", "default", 0), "T"),
          "/stream is an NXentry whose @default is T, the first dataset covered");
    CHECK(!strcmp(str_attr(nf, "/stream/T", "NX_class", 0), "NXdata") &&
              !strcmp(str_attr(nf, "/stream/T", "signal", 0), "data") &&
              !strcmp(str_attr(nf, "/stream/T", "axes", 0), "step") &&
              !strcmp(str_attr(nf, "/stream/T", "axes", 1), ".") &&
              !strcmp(str_attr(nf, "/stream/T/data", "interpretation", 0), "spectrum"),
          "/stream/T is an NXdata: @signal data, @axes [step, .], a spectrum per row");
    {
        const uint64_t t_steps[] = {0, 1, 2, 3}, u_steps[] = {1, 2, 3}, det_steps[] = {0, 1, 3, 3};

        CHECK(axis_is(nf, "/T", t_steps, 4) && axis_is(nf, "/g/U", u_steps, 3),
              "its step axis is 0-3, and /g/U's is 1-3");
        CHECK(axis_is(nf, "/det", det_steps, 4) &&
                  !strcmp(str_attr(nf, "/stream/det/data", "interpretation", 0), "image") &&
                  !strcmp(str_attr(nf, "/stream/det", "axes", 2), "."),
              "/stream/det's axis gives each frame's step (0, 1, 3, 3), and a frame is an image");
    }

    if ((ds = H5Dopen2(nf, "/stream/det/data", H5P_DEFAULT)) < 0) {
        printf("  FAIL  /stream/det/data missing\n");
        return 1;
    }
    {
        hsize_t dd[3] = {0, 0, 0};
        int     dv[4][FR * FC], k, e;

        sp = H5Dget_space(ds);
        H5Sget_simple_extent_dims(sp, dd, NULL);
        CHECK(dd[0] == 4 && dd[1] == FR && dd[2] == FC, "/stream/det is [%llu][%llu][%llu], one row per frame",
              (unsigned long long)dd[0], (unsigned long long)dd[1], (unsigned long long)dd[2]);
        ok = dd[0] == 4 && H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, dv) >= 0;
        for (k = 0; ok && k < 4; k++)
            for (e = 0; e < FR * FC; e++)
                ok = ok && dv[k][e] == frame_val(k, e);
        CHECK(ok, "each row holds its frame, from the step that wrote it (steps 0, 1, 3, 3)");
        H5Sclose(sp);
        H5Dclose(ds);
    }
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
