/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * A multi-module detector acquisition, written the way a beamline writes one:
 * NeXus-shaped HDF5, one frame per step, calibration written once.
 *
 * This is the writer half of the use case in RFC-VOLSTREAM-2026-001 section
 * A.2 ("What the Domain Already Deploys"). The comparison it is built against
 * is the deployed pipeline at PETRA III P02.2, where a multi-module LAMBDA
 * detector streams into ASAP::O and the *structure* of the NeXus file travels
 * as a second, separate stream-metadata channel for a distinct NeXus-writer
 * service to reassemble.
 *
 * The point of this program is what it does NOT do. It never describes its
 * own layout to anybody. There is no schema channel, no structure message,
 * no side agreement with the consumers -- it calls H5Dcreate2() and
 * H5Dwrite() and nothing else. Every consumer in pipeline_consumer.c learns
 * the paths, datatypes and extents from H5Fget_stream_schema(), which is
 * answered out of the encoded H5Tencode()/H5Sencode2() bytes this writer's
 * steps already carry. One channel, and no reassembly service.
 *
 * Three objects, deliberately on two different cadences:
 *
 *   /entry/data/data                              every step  (the frames)
 *   /entry/instrument/detector/pixel_mask         step 0 only (calibration)
 *   /entry/instrument/detector/flatfield          step 0 only (calibration)
 *
 * The calibration arriving once while frames arrive continuously is the
 * concrete form of RFC section 1.5's fourth capability -- but note the
 * narrower claim A.2 actually makes: independent cadence on its own is
 * expressible in other systems too (ASAP::O sources can open separate
 * streams). What is not, is independent cadence with an explicit dependency,
 * i.e. a consumer resolving a given frame against the correct calibration.
 * This example shows the cadence; it does not by itself prove the
 * dependency, and the README says so.
 *
 * Module geometry: DETECTOR_MODULES panels stacked in the slow dimension,
 * each written as its own hyperslab into one logical frame -- the shape
 * P02.2 gets from twelve substreams synchronized into a dataset. It is a
 * SINGLE writer process here, not M ranks: this example is about the
 * consumer side, and the real M-by-N case is test/t_parallel.c. The
 * per-module writes are what make MODE_ANALYSIS's single-panel subscription
 * a truthful stand-in for the stitcher's per-panel input.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"
#include "detector_common.h"

#define N_CONSUMERS 4

/* Bright spots scattered across the frame, rotating with the frame index so
 * successive hits are not identical. Deterministic, so a run is reproducible.
 */
static void
fill_frame(int32_t *frame, int f)
{
    int hit = detector_frame_is_hit(f);
    int i;

    memset(frame, 0, DETECTOR_FRAME_ELEMS * sizeof(int32_t));

    if (!hit) {
        /* A blank frame is not perfectly empty -- a few stray counts, below
         * the hit threshold, so the hitfinder's silence is a real decision
         * about values rather than a decision about an all-zero buffer. */
        for (i = 0; i < 8; i++)
            frame[((size_t)(i * 7919 + f * 104729)) % DETECTOR_FRAME_ELEMS] = 3;
        return;
    }

    for (i = 0; i < DETECTOR_SPOT_COUNT; i++) {
        size_t at = ((size_t)(i * 2654435761u + (unsigned)f * 40503u)) % DETECTOR_FRAME_ELEMS;

        frame[at] = DETECTOR_SPOT_VALUE;
    }
}

/*
 * pixel_mask and flatfield, created and written once inside step 0 and never
 * touched again -- the other cadence in this stream. Both are 2-D over the
 * full module stack, which is how a detector ships them: one calibration
 * plane covering every panel.
 *
 * Created with the group-creation intermediate property set, since
 * /entry/instrument/detector does not exist yet and nothing else in this
 * program creates it.
 */
static herr_t
detector_write_calibration(hid_t fid)
{
    hsize_t  cdims[2] = {DETECTOR_ROWS, DETECTOR_COLS};
    hid_t    space = H5I_INVALID_HID, lcpl = H5I_INVALID_HID;
    hid_t    mask = H5I_INVALID_HID, flat = H5I_INVALID_HID;
    uint8_t *maskbuf = NULL;
    float   *flatbuf = NULL;
    herr_t   ret     = -1;
    size_t   i;

    if (NULL == (maskbuf = (uint8_t *)calloc(DETECTOR_FRAME_ELEMS, sizeof(uint8_t))) ||
        NULL == (flatbuf = (float *)calloc(DETECTOR_FRAME_ELEMS, sizeof(float)))) {
        fprintf(stderr, "writer: FAIL allocate calibration\n");
        goto done;
    }
    /* A scattering of dead pixels, and a gentle per-module gain trim -- the
     * values do not matter to the demo, only that these are real objects of
     * their own types (uint8 and float32, neither of them the image's
     * int32), so the schema a consumer discovers is genuinely heterogeneous
     * rather than three copies of one description. */
    for (i = 0; i < DETECTOR_FRAME_ELEMS; i++) {
        maskbuf[i] = (i % 9973) == 0 ? 1 : 0;
        flatbuf[i] = 1.0f + 0.01f * (float)((i / DETECTOR_COLS) / DETECTOR_MODULE_ROWS);
    }

    if ((space = H5Screate_simple(2, cdims, NULL)) < 0)
        goto done;
    if ((lcpl = H5Pcreate(H5P_LINK_CREATE)) < 0 || H5Pset_create_intermediate_group(lcpl, 1) < 0)
        goto done;

    if ((mask = H5Dcreate2(fid, DETECTOR_PIXELMASK_PATH, H5T_NATIVE_UINT8, space, lcpl, H5P_DEFAULT,
                            H5P_DEFAULT)) < 0) {
        fprintf(stderr, "writer: FAIL create %s\n", DETECTOR_PIXELMASK_PATH);
        goto done;
    }
    if (H5Dwrite(mask, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT, maskbuf) < 0)
        goto done;

    if ((flat = H5Dcreate2(fid, DETECTOR_FLATFIELD_PATH, H5T_NATIVE_FLOAT, space, lcpl, H5P_DEFAULT,
                            H5P_DEFAULT)) < 0) {
        fprintf(stderr, "writer: FAIL create %s\n", DETECTOR_FLATFIELD_PATH);
        goto done;
    }
    if (H5Dwrite(flat, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, flatbuf) < 0)
        goto done;

    printf("writer: calibration written once (pixel_mask uint8, flatfield float32) -- not resent\n");
    ret = 0;

done:
    if (mask != H5I_INVALID_HID)
        H5Dclose(mask);
    if (flat != H5I_INVALID_HID)
        H5Dclose(flat);
    if (space != H5I_INVALID_HID)
        H5Sclose(space);
    if (lcpl != H5I_INVALID_HID)
        H5Pclose(lcpl);
    free(maskbuf);
    free(flatbuf);
    if (ret < 0)
        fprintf(stderr, "writer: FAIL write calibration\n");
    return ret;
}

int
main(int argc, char **argv)
{
    int nframes  = argc > 1 ? atoi(argv[1]) : DETECTOR_NFRAMES;
    int delay_ms = argc > 2 ? atoi(argv[2]) : 500;

    hid_t    vol_id, fapl, fid;
    hid_t    image = H5I_INVALID_HID, file_space = H5I_INVALID_HID;
    hid_t    frame_space = H5I_INVALID_HID, dcpl = H5I_INVALID_HID;
    int32_t *frame;
    int      f, m;

    hsize_t img_dims[3]  = {0, DETECTOR_ROWS, DETECTOR_COLS};
    hsize_t img_max[3]   = {H5S_UNLIMITED, DETECTOR_ROWS, DETECTOR_COLS};
    hsize_t img_chunk[3] = {1, DETECTOR_ROWS, DETECTOR_COLS};

    setvbuf(stdout, NULL, _IOLBF, 0);

    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(DETECTOR_FNAME);
    unlink(DETECTOR_FNAME ".vsgroup");

    if (NULL == (frame = (int32_t *)calloc(DETECTOR_FRAME_ELEMS, sizeof(int32_t)))) {
        fprintf(stderr, "writer: FAIL allocate frame\n");
        return 1;
    }

    if ((vol_id = H5VL_stream_register()) < 0) {
        fprintf(stderr, "writer: FAIL register vol-stream\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        fprintf(stderr, "writer: FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(DETECTOR_FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        fprintf(stderr, "writer: FAIL create %s (transport up? VOL_STREAM_NA set?)\n", DETECTOR_FNAME);
        return 1;
    }

    printf("writer: %d frame(s), %d modules of %dx%d -> %dx%d int32 per frame (%zu KiB raw)\n", nframes,
           DETECTOR_MODULES, DETECTOR_MODULE_ROWS, DETECTOR_COLS, DETECTOR_ROWS, DETECTOR_COLS,
           (DETECTOR_FRAME_ELEMS * sizeof(int32_t)) / 1024);
    printf("writer: NeXus layout at %s -- structure is NEVER announced separately\n", DETECTOR_IMAGE_PATH);
    printf("writer: waiting up to %ds for consumers (run pipeline_consumer in other terminals)...\n",
           DETECTOR_BARRIER_TIMEOUT_MS / 1000);

    if (H5Fwait_subscribers(fid, N_CONSUMERS, DETECTOR_BARRIER_TIMEOUT_MS) < 0)
        printf("writer: proceeding without all %d consumers (fewer is fine for this demo)\n", N_CONSUMERS);
    else
        printf("writer: all %d consumers attached, starting acquisition\n", N_CONSUMERS);

    /* One chunk per frame -- the layout EIGER's own writer produces, and the
     * one that makes a per-frame push exactly one chunk. */
    if ((dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 || H5Pset_chunk(dcpl, 3, img_chunk) < 0) {
        fprintf(stderr, "writer: FAIL image dcpl\n");
        return 1;
    }
    if ((file_space = H5Screate_simple(3, img_dims, img_max)) < 0) {
        fprintf(stderr, "writer: FAIL image dataspace\n");
        return 1;
    }

    for (f = 0; f < nframes; f++) {
        hsize_t start[3]  = {(hsize_t)f, 0, 0};
        hsize_t count[3]  = {1, DETECTOR_MODULE_ROWS, DETECTOR_COLS};
        hsize_t ext[3]    = {(hsize_t)f + 1, DETECTOR_ROWS, DETECTOR_COLS};
        hsize_t mdims[2]  = {DETECTOR_MODULE_ROWS, DETECTOR_COLS};

        fill_frame(frame, f);

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            fprintf(stderr, "writer: FAIL begin_step %d\n", f);
            return 1;
        }

        if (f == 0) {
            if ((image = H5Dcreate2(fid, DETECTOR_IMAGE_PATH, H5T_NATIVE_INT32, file_space, H5P_DEFAULT,
                                     dcpl, H5P_DEFAULT)) < 0) {
                fprintf(stderr, "writer: FAIL create %s\n", DETECTOR_IMAGE_PATH);
                return 1;
            }
            /* Calibration: written inside step 0 only, so it is part of the
             * stream (an object written outside a step passes straight
             * through to the file and never appears in the schema -- see
             * H5Fbegin_step()'s note). Frames continue every step after
             * this; these two never appear again. */
            if (detector_write_calibration(fid) < 0)
                return 1;
        }

        if (H5Dset_extent(image, ext) < 0) {
            fprintf(stderr, "writer: FAIL extend to frame %d\n", f);
            return 1;
        }
        H5Sclose(file_space);
        if ((file_space = H5Dget_space(image)) < 0) {
            fprintf(stderr, "writer: FAIL reacquire file space\n");
            return 1;
        }
        if ((frame_space = H5Screate_simple(2, mdims, NULL)) < 0) {
            fprintf(stderr, "writer: FAIL module memspace\n");
            return 1;
        }

        /* Each module writes its own panel into the one logical frame --
         * DETECTOR_MODULES separate H5Dwrite() calls, as many as there are
         * detector modules, rather than one whole-frame write. */
        for (m = 0; m < DETECTOR_MODULES; m++) {
            start[1] = (hsize_t)m * DETECTOR_MODULE_ROWS;
            if (H5Sselect_hyperslab(file_space, H5S_SELECT_SET, start, NULL, count, NULL) < 0) {
                fprintf(stderr, "writer: FAIL select module %d\n", m);
                return 1;
            }
            if (H5Dwrite(image, H5T_NATIVE_INT32, frame_space, file_space, H5P_DEFAULT,
                          frame + (size_t)m * DETECTOR_MODULE_ROWS * DETECTOR_COLS) < 0) {
                fprintf(stderr, "writer: FAIL write module %d of frame %d\n", m, f);
                return 1;
            }
        }
        H5Sclose(frame_space);
        frame_space = H5I_INVALID_HID;

        if (H5Fend_step(fid) < 0) {
            fprintf(stderr, "writer: FAIL end_step %d\n", f);
            return 1;
        }

        printf("writer: frame %d/%d  %s  (%d module writes, %zu KiB raw)\n", f + 1, nframes,
               detector_frame_is_hit(f) ? "HIT  " : "blank", DETECTOR_MODULES,
               (DETECTOR_FRAME_ELEMS * sizeof(int32_t)) / 1024);
        if (delay_ms > 0)
            usleep((useconds_t)delay_ms * 1000);
    }

    printf("writer: acquisition done, settling before close...\n");
    usleep(1500000); /* outlive consumers still draining -- same settle window the other examples use */

    if (image != H5I_INVALID_HID)
        H5Dclose(image);
    if (file_space != H5I_INVALID_HID)
        H5Sclose(file_space);
    H5Pclose(dcpl);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    free(frame);

    printf("writer: done -- %d frame(s) streamed, and %s is a real NeXus-shaped file\n", nframes,
           DETECTOR_FNAME);
    return 0;
}
