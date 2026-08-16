/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * PoC writer: vol-stream stacked ON TOP of VFD SWMR.
 *
 * Deliberately builds WITHOUT the transport. Everything the companion reader
 * observes travels through the file, which is the whole point -- see
 * README.md.
 *
 * The stacking needs no connector changes: H5VL_stream_file_create() copies
 * the application's FAPL and overrides only the VOL, so a VFD SWMR
 * configuration set here reaches the native connector untouched.
 *
 * argv[1]: 1 = VFD SWMR on (default), 0 = control arm without it.
 */

#include "vs_swmr_common.h"
#include "H5VLstream.h"

int
main(int argc, char **argv)
{
    int enable_swmr = (argc > 1) ? atoi(argv[1]) : 1;

    hid_t   vol_id = H5I_INVALID_HID, fapl = H5I_INVALID_HID, fcpl = H5I_INVALID_HID;
    hid_t   fid = H5I_INVALID_HID, space = H5I_INVALID_HID, dcpl = H5I_INVALID_HID;
    hid_t   ds = H5I_INVALID_HID;
    hsize_t dims = VS_SWMR_CHUNK, maxdims = H5S_UNLIMITED, chunk = VS_SWMR_CHUNK;
    int    *vals = NULL;
    int     s, rc = 1;

    /* No transport. This PoC is about the file path only. */
    unsetenv("VOL_STREAM_NA");

    unlink(VS_SWMR_FNAME);
    unlink(VS_SWMR_SHADOW);

    if ((vol_id = H5VL_stream_register()) < 0) {
        fprintf(stderr, "writer: cannot register vol-stream\n");
        goto done;
    }
    if ((fcpl = vs_swmr_fcpl()) < 0) {
        fprintf(stderr, "writer: cannot build FCPL (paged allocation)\n");
        goto done;
    }
    if ((fapl = vs_swmr_fapl(1 /* writer */, enable_swmr)) < 0) {
        fprintf(stderr, "writer: cannot build FAPL\n");
        goto done;
    }

    /* vol-stream on top of whatever VFD the FAPL already names. */
    if (H5Pset_vol(fapl, vol_id, NULL) < 0) {
        fprintf(stderr, "writer: cannot stack vol-stream\n");
        goto done;
    }

    if ((fid = H5Fcreate(VS_SWMR_FNAME, H5F_ACC_TRUNC, fcpl, fapl)) < 0) {
        fprintf(stderr, "writer: H5Fcreate failed (swmr=%d)\n", enable_swmr);
        goto done;
    }
    printf("writer: created %s, VFD SWMR %s, no transport\n", VS_SWMR_FNAME,
           enable_swmr ? "ON" : "OFF");
    fflush(stdout);

    if (NULL == (vals = (int *)malloc((size_t)VS_SWMR_CHUNK * sizeof(int)))) {
        fprintf(stderr, "writer: out of memory\n");
        goto done;
    }
    if ((space = H5Screate_simple(1, &dims, &maxdims)) < 0)
        goto done;
    if ((dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 || H5Pset_chunk(dcpl, 1, &chunk) < 0)
        goto done;

    for (s = 0; s < VS_SWMR_NSTEPS; s++) {
        hsize_t  newdims = (hsize_t)(s + 1) * VS_SWMR_CHUNK;
        hsize_t  start = (hsize_t)s * VS_SWMR_CHUNK, count = VS_SWMR_CHUNK;
        hid_t    fspace, mspace;
        uint64_t logical = (uint64_t)s;
        int      i;

        for (i = 0; i < VS_SWMR_CHUNK; i++)
            vals[i] = (int)start + i;

        if (H5Fbegin_step(fid, 1, &logical, 0) < 0) {
            fprintf(stderr, "writer: begin_step %d failed\n", s);
            goto done;
        }

        if (s == 0) {
            if ((ds = H5Dcreate2(fid, VS_SWMR_DSET, H5T_NATIVE_INT, space, H5P_DEFAULT, dcpl,
                                  H5P_DEFAULT)) < 0)
                goto done;
        }
        else if (H5Dset_extent(ds, &newdims) < 0)
            goto done;

        /* Tail-only write; the connector carries the previous step forward,
         * so each step is still a complete snapshot -- which is what the
         * reader's element counts confirm. */
        if ((fspace = H5Dget_space(ds)) < 0 ||
            H5Sselect_hyperslab(fspace, H5S_SELECT_SET, &start, NULL, &count, NULL) < 0 ||
            (mspace = H5Screate_simple(1, &count, NULL)) < 0)
            goto done;
        if (H5Dwrite(ds, H5T_NATIVE_INT, mspace, fspace, H5P_DEFAULT, vals) < 0)
            goto done;
        H5Sclose(mspace);
        H5Sclose(fspace);

        if (H5Fend_step(fid) < 0) {
            fprintf(stderr, "writer: end_step %d failed\n", s);
            goto done;
        }

        /* end_step() makes the step DURABLE; end_tick() makes it VISIBLE to a
         * SWMR reader now rather than at the next tick boundary. This pairing
         * is the point of the demonstration. */
        if (enable_swmr && H5Fvfd_swmr_end_tick(fid) < 0) {
            fprintf(stderr, "writer: H5Fvfd_swmr_end_tick failed at step %d\n", s);
            goto done;
        }

        printf("writer: step %2d committed and published\n", s + 1);
        fflush(stdout);
        usleep(600000); /* 0.6s, longer than one 0.4s tick */
    }

    rc = 0;

done:
    if (rc != 0)
        H5Eprint2(H5E_DEFAULT, stderr);
    if (ds >= 0)
        H5Dclose(ds);
    if (dcpl >= 0)
        H5Pclose(dcpl);
    if (space >= 0)
        H5Sclose(space);
    if (fid >= 0)
        H5Fclose(fid);
    if (fapl >= 0)
        H5Pclose(fapl);
    if (fcpl >= 0)
        H5Pclose(fcpl);
    if (vol_id >= 0)
        H5VLclose(vol_id);
    free(vals);

    printf("writer: exiting rc=%d\n", rc);
    return rc;
}
