/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * H5Dset_extent() on a dataset created in the step still open.
 *
 * Every NeXus detector writer creates its frame stack empty -- [0, i, j],
 * unlimited in the first dimension -- and extends it before writing the first
 * frame, all in the first step. That H5Dset_extent() used to fail with a bare
 * -1: a dataset created in the open step is a placeholder with no real object
 * under it. examples/detector_pipeline's writer did exactly this, so it
 * failed on frame 0; CI first ran it through examples/silx_live_view.
 *
 * Checked through the native connector: /step/0 holds one row, /step/1 two,
 * with the values written. Shrinking a placeholder fails and says why.
 *
 * No transport needed.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_placeholder_extend.h5"
#define ROWS  4
#define COLS  6
#define PANEL 2 /* rows per module write, as detector_writer does */

static int nerrors = 0;

static int
val(int f, int r, int c)
{
    return f * 1000 + r * COLS + c;
}

static int found;

static herr_t
find(unsigned n, const H5E_error2_t *err, void *udata)
{
    (void)n;
    if (err && err->desc && strstr(err->desc, (const char *)udata))
        found = 1;
    return 0;
}

/* Extend `ds` to f+1 frames and write frame f in module-sized panels. */
static int
write_frame(hid_t ds, int f)
{
    hsize_t ext[3]   = {(hsize_t)f + 1, ROWS, COLS};
    hsize_t count[3] = {1, PANEL, COLS};
    hsize_t mdims[2] = {PANEL, COLS};
    int     buf[PANEL * COLS];
    hid_t   fs, ms;
    int     m, r, c;

    if (H5Dset_extent(ds, ext) < 0) {
        printf("  FAIL  H5Dset_extent to %d frame(s)\n", f + 1);
        return -1;
    }
    if ((fs = H5Dget_space(ds)) < 0 || (ms = H5Screate_simple(2, mdims, NULL)) < 0)
        return -1;
    for (m = 0; m < ROWS / PANEL; m++) {
        hsize_t start[3] = {(hsize_t)f, (hsize_t)m * PANEL, 0};

        for (r = 0; r < PANEL; r++)
            for (c = 0; c < COLS; c++)
                buf[r * COLS + c] = val(f, m * PANEL + r, c);
        if (H5Sselect_hyperslab(fs, H5S_SELECT_SET, start, NULL, count, NULL) < 0 ||
            H5Dwrite(ds, H5T_NATIVE_INT, ms, fs, H5P_DEFAULT, buf) < 0) {
            printf("  FAIL  write frame %d module %d\n", f, m);
            return -1;
        }
    }
    H5Sclose(ms);
    H5Sclose(fs);
    return 0;
}

static void
check_step(hid_t nfid, int s)
{
    char    path[64];
    hsize_t dims[3];
    int     got[(ROWS * COLS) * 2];
    hid_t   ds, sp;
    int     f, r, c;

    snprintf(path, sizeof(path), "/step/%d/entry/data/data", s);
    if ((ds = H5Dopen2(nfid, path, H5P_DEFAULT)) < 0 || (sp = H5Dget_space(ds)) < 0 ||
        H5Sget_simple_extent_dims(sp, dims, NULL) != 3) {
        printf("  FAIL  %s missing or not rank 3\n", path);
        nerrors++;
        return;
    }
    if (dims[0] != (hsize_t)s + 1 || dims[1] != ROWS || dims[2] != COLS) {
        printf("  FAIL  %s is [%llu,%llu,%llu], expected [%d,%d,%d]\n", path, (unsigned long long)dims[0],
               (unsigned long long)dims[1], (unsigned long long)dims[2], s + 1, ROWS, COLS);
        nerrors++;
    }
    else if (H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) < 0) {
        printf("  FAIL  read %s\n", path);
        nerrors++;
    }
    else {
        /* Only this step's frame is checked: earlier rows are carried
         * forward or unwritten depending on the connector's copy policy. */
        f = s;
        for (r = 0; r < ROWS; r++)
            for (c = 0; c < COLS; c++)
                if (got[(f * ROWS + r) * COLS + c] != val(f, r, c)) {
                    printf("  FAIL  %s[%d][%d][%d] = %d, expected %d\n", path, f, r, c,
                           got[(f * ROWS + r) * COLS + c], val(f, r, c));
                    nerrors++;
                    H5Sclose(sp);
                    H5Dclose(ds);
                    return;
                }
        printf("  ok    %s is [%d,%d,%d] with frame %d as written\n", path, s + 1, ROWS, COLS, s);
    }
    H5Sclose(sp);
    H5Dclose(ds);
}

int
main(void)
{
    hsize_t dims0[3] = {0, ROWS, COLS}, max[3] = {H5S_UNLIMITED, ROWS, COLS}, chunk[3] = {1, ROWS, COLS};
    hsize_t two[1] = {2}, umax[1] = {H5S_UNLIMITED}, one[1] = {1};
    hid_t   fapl, fid, sp, dcpl, dcpl1, ds, ds2, lcpl, nfid, sp1;
    herr_t  r;

    printf("vol-stream: H5Dset_extent() on a dataset created in the open step\n");
    unlink(FNAME);

    fapl = H5Pcreate(H5P_FILE_ACCESS);
    if (H5Pset_fapl_stream(fapl, NULL) < 0 || (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (sp = H5Screate_simple(3, dims0, max)) < 0 || (dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
        H5Pset_chunk(dcpl, 3, chunk) < 0 || (lcpl = H5Pcreate(H5P_LINK_CREATE)) < 0 ||
        H5Pset_create_intermediate_group(lcpl, 1) < 0) {
        printf("  FAIL  setup\n");
        return 1;
    }

    /* Step 0: create empty, extend, write -- the detector writer's frame 0. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (ds = H5Dcreate2(fid, "/entry/data/data", H5T_NATIVE_INT, sp, lcpl, dcpl, H5P_DEFAULT)) < 0 ||
        write_frame(ds, 0) < 0) {
        printf("  FAIL  step 0\n");
        return 1;
    }

    /* Shrinking a placeholder is refused, and says so. */
    if ((sp1 = H5Screate_simple(1, two, umax)) < 0 || (dcpl1 = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
        H5Pset_chunk(dcpl1, 1, one) < 0 ||
        (ds2 = H5Dcreate2(fid, "/shrink", H5T_NATIVE_INT, sp1, H5P_DEFAULT, dcpl1, H5P_DEFAULT)) < 0) {
        printf("  FAIL  create /shrink\n");
        return 1;
    }
    H5E_BEGIN_TRY
    {
        r = H5Dset_extent(ds2, one);
    }
    H5E_END_TRY
    found = 0;
    if (r < 0)
        H5Ewalk2(H5E_DEFAULT, H5E_WALK_DOWNWARD, find, (void *)"shrinking");
    if (r < 0 && found)
        printf("  ok    shrinking a dataset created in the open step fails, and says why\n");
    else {
        printf("  FAIL  shrinking a placeholder: %s\n", r >= 0 ? "did not fail" : "no frame with the reason");
        nerrors++;
    }
    H5Eclear2(H5E_DEFAULT);
    H5Dclose(ds2);

    if (H5Fend_step(fid) < 0) {
        printf("  FAIL  end_step 0\n");
        return 1;
    }

    /* Step 1: the live-dataset path, which already worked. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || write_frame(ds, 1) < 0 || H5Fend_step(fid) < 0) {
        printf("  FAIL  step 1\n");
        return 1;
    }

    H5Dclose(ds);
    H5Fclose(fid);

    if ((nfid = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  native open\n");
        return 1;
    }
    check_step(nfid, 0);
    check_step(nfid, 1);
    H5Fclose(nfid);

    H5Pclose(lcpl);
    H5Pclose(dcpl1);
    H5Pclose(dcpl);
    H5Sclose(sp1);
    H5Sclose(sp);
    H5Pclose(fapl);
    unlink(FNAME);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
