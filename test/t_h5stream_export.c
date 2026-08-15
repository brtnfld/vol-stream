/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * h5stream's `export` subcommand (M9).
 *
 * export collapses a stream into an ordinary HDF5 file: each object once, at
 * its logical path, taking the newest step that wrote it. That is the same
 * "state as of the latest step" a reader through the connector sees, and the
 * result contains no vol-stream concepts at all.
 *
 * The stream here is built so that a plausible-but-wrong implementation
 * fails:
 *
 *   step 0: /stable = 1, /rewritten = 10
 *   step 1: /rewritten = 20          (same object, new value)
 *   step 2: /rewritten = 30, /late = 3
 *
 * So /rewritten is written in all three steps with a different value each
 * time. An export that took the *first* occurrence, or that emitted one copy
 * per step, would still produce a file full of real data that opens fine --
 * it would just be the wrong data, or the wrong shape. Asserting the value is
 * 30 is what distinguishes "newest wins" from "any version".
 *
 * Also checked:
 *   - No "/step" group survives. The whole point is a file with none of the
 *     connector's layout left in it, and a copy that preserved the step
 *     groups would still round-trip every value correctly.
 *   - Objects written in different steps all appear together, since the
 *     export is a union across steps, not a snapshot of the last one.
 *   - The result opens with the *native* connector, i.e. it is a plain HDF5
 *     file that needs nothing of ours to read.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_h5stream_export.h5"
#define OUTNAME "t_h5stream_export.out.h5"

static int nerrors = 0;

static int
write_val(hid_t fid, const char *name, int value)
{
    hid_t   sp, ds;
    hsize_t one = 1;

    if ((sp = H5Screate_simple(1, &one, NULL)) < 0)
        return -1;
    if ((ds = H5Dcreate2(fid, name, H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        H5Sclose(sp);
        return -1;
    }
    if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value) < 0) {
        H5Dclose(ds);
        H5Sclose(sp);
        return -1;
    }
    H5Dclose(ds);
    H5Sclose(sp);
    return 0;
}

/* Read one int from a dataset in an already-open native file. */
static int
read_val(hid_t fid, const char *name, int *out)
{
    hid_t ds;

    if ((ds = H5Dopen2(fid, name, H5P_DEFAULT)) < 0)
        return -1;
    if (H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, out) < 0) {
        H5Dclose(ds);
        return -1;
    }
    H5Dclose(ds);
    return 0;
}

int
main(int argc, char **argv)
{
    hid_t vol_id, fapl, fid, nfid;
    char  cmd[1024];
    int   v;

    if (argc < 2) {
        printf("  FAIL  usage: %s <path-to-h5stream>\n", argv[0]);
        return 1;
    }

    printf("vol-stream: h5stream export\n");

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("  FAIL  register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("  FAIL  fapl\n");
        return 1;
    }
    unlink(FNAME);
    unlink(OUTNAME);
    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("  FAIL  create\n");
        return 1;
    }

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || write_val(fid, "/stable", 1) < 0 ||
        write_val(fid, "/rewritten", 10) < 0 || H5Fend_step(fid) < 0) {
        printf("  FAIL  step 0\n");
        return 1;
    }
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || write_val(fid, "/rewritten", 20) < 0 ||
        H5Fend_step(fid) < 0) {
        printf("  FAIL  step 1\n");
        return 1;
    }
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || write_val(fid, "/rewritten", 30) < 0 ||
        write_val(fid, "/late", 3) < 0 || H5Fend_step(fid) < 0) {
        printf("  FAIL  step 2\n");
        return 1;
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    snprintf(cmd, sizeof(cmd), "\"%s\" export %s %s > /dev/null 2>&1", argv[1], FNAME, OUTNAME);
    if (system(cmd) != 0) {
        printf("  FAIL  h5stream export exited non-zero\n");
        return 1;
    }

    /* Plain HDF5 from here on -- nothing of ours should be needed. */
    if ((nfid = H5Fopen(OUTNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  exported file does not open with the native connector\n");
        return 1;
    }
    printf("  ok    exported file opens as plain HDF5\n");

    /* The load-bearing check: newest version wins. */
    if (read_val(nfid, "/rewritten", &v) < 0) {
        printf("  FAIL  /rewritten missing from the export\n");
        nerrors++;
    }
    else if (v != 30) {
        printf("  FAIL  /rewritten = %d, expected 30 -- export did not take the newest step "
               "(10 would be the first, 20 the middle)\n",
               v);
        nerrors++;
    }
    else
        printf("  ok    a rewritten object exports at its newest value\n");

    /* Union across steps, not just the last one. */
    if (read_val(nfid, "/stable", &v) == 0 && v == 1)
        printf("  ok    an object written only in an early step still appears\n");
    else {
        printf("  FAIL  /stable missing or wrong -- export is not a union across steps\n");
        nerrors++;
    }
    if (read_val(nfid, "/late", &v) == 0 && v == 3)
        printf("  ok    an object written only in the last step appears\n");
    else {
        printf("  FAIL  /late missing or wrong\n");
        nerrors++;
    }

    /* No connector layout may survive. */
    {
        hid_t g;

        H5E_BEGIN_TRY
        {
            g = H5Gopen2(nfid, "/step", H5P_DEFAULT);
        }
        H5E_END_TRY
        if (g >= 0) {
            printf("  FAIL  exported file still contains a \"/step\" group -- the point of export is a "
                   "file with none of the stream layout left\n");
            nerrors++;
            H5Gclose(g);
        }
        else
            printf("  ok    no \"/step\" layout survives the export\n");
    }

    H5Fclose(nfid);
    unlink(OUTNAME);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
