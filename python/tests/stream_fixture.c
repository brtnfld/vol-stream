/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Writes a small vol-stream file for the Python tests to open: two steps of
 * one 1-D int dataset, "/x". Runs without the transport.
 *
 * usage: stream_fixture <path>
 */

#include <stdio.h>
#include <stdlib.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NX     4
#define NSTEPS 2

int
main(int argc, char **argv)
{
    hid_t   vol_id, fapl, fid, space, ds = H5I_INVALID_HID;
    hsize_t dims = NX;
    int     vals[NX];
    int     s, i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <path>\n", argv[0]);
        return 2;
    }
    unsetenv("VOL_STREAM_NA");

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0 ||
        (fid = H5Fcreate(argv[1], H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (space = H5Screate_simple(1, &dims, NULL)) < 0) {
        fprintf(stderr, "stream_fixture: FAIL setup\n");
        return 1;
    }

    for (s = 0; s < NSTEPS; s++) {
        for (i = 0; i < NX; i++)
            vals[i] = s * 100 + i;
        if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
            (s == 0 &&
             (ds = H5Dcreate2(fid, "/x", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) ||
            H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
            fprintf(stderr, "stream_fixture: FAIL step %d\n", s);
            return 1;
        }
    }

    H5Dclose(ds);
    H5Sclose(space);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}
