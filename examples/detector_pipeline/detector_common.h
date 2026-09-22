/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef DETECTOR_COMMON_H
#define DETECTOR_COMMON_H

#include <stdio.h>
#include <unistd.h>

#define DETECTOR_FNAME "detector_pipeline.h5"

/*
 * The NeXus layout a multi-module detector's own acquisition writes -- image
 * data under an NXdata group, calibration under NXdetector. These paths are
 * the writer's business. pipeline_consumer.c deliberately does NOT use them
 * to shape a subscription: it discovers paths, types and extents from
 * H5Fget_stream_schema() instead, which is the whole point of this example.
 * It uses DETECTOR_IMAGE_PATH only to *recognize* which discovered variable
 * is the image stack, the way a real viewer keys off an NXdata attribute.
 */
#define DETECTOR_IMAGE_PATH     "/entry/data/data"
#define DETECTOR_PIXELMASK_PATH "/entry/instrument/detector/pixel_mask"
#define DETECTOR_FLATFIELD_PATH "/entry/instrument/detector/flatfield"

/*
 * A LAMBDA-class module is 1556x516; this is a 1/4-scale stand-in so the demo
 * runs in a few seconds on a laptop. Four modules stacked in the slow
 * dimension, which is the geometry that makes MODE_ANALYSIS's single-panel
 * subscription a contiguous row band rather than a strided one -- see the
 * README's note on why a *column* band is a different (and worse) case.
 */
#define DETECTOR_MODULES      4
#define DETECTOR_MODULE_ROWS  128
#define DETECTOR_COLS         256
#define DETECTOR_ROWS         (DETECTOR_MODULES * DETECTOR_MODULE_ROWS)
#define DETECTOR_FRAME_ELEMS  ((size_t)DETECTOR_ROWS * DETECTOR_COLS)

#define DETECTOR_NFRAMES 8

/* Photon counts: a sparse scene with a handful of bright Bragg-like spots on
 * an otherwise near-empty frame, which is the regime the RFC's appendix A
 * measures. "Hit" frames carry spots; "blank" frames carry only a few stray
 * counts -- so the hitfinder consumer sees both real matches and real
 * silence over one run rather than one or the other. */
#define DETECTOR_SPOT_VALUE  4000
#define DETECTOR_SPOT_COUNT  24
#define DETECTOR_HIT_THRESHOLD 1000

/* How long the writer waits for consumers before proceeding anyway. A
 * consumer's cumulative idle budget must exceed this -- see
 * pipeline_consumer.c, which sizes its miss counter against it. */
#define DETECTOR_BARRIER_TIMEOUT_MS 15000

/*
 * The first DETECTOR_HIT_FRAMES frames carry signal; every frame at or after
 * that is blank -- a decaying acquisition, not an alternating one. This is
 * what lets MODE_MONITOR's two decisions ("signal established" / "signal
 * lost") both actually fire in a normal run, rather than one of them being
 * merely described and never exercised. It has a real referent, not just a
 * demo-friendly shape: RFC appendix A cites serial femtosecond
 * crystallography, where a crystal's useful diffraction window is short and
 * facilities veto once it closes rather than keep collecting blank frames
 * -- this is that pattern, compressed to DETECTOR_NFRAMES frames.
 */
#define DETECTOR_HIT_FRAMES 4

/*
 * MODE_MONITOR's decision thresholds -- see pipeline_consumer.c. Both small
 * on purpose, so both decisions fire inside the default eight-frame run.
 */
#define DETECTOR_MONITOR_GOOD_HITS   2 /* hits observed before declaring
                                         * signal established              */
#define DETECTOR_MONITOR_MISS_STREAK 2 /* consecutive misses, after signal
                                         * was established, before declaring
                                         * it lost                         */

static int
detector_wait_for_file(const char *path, int timeout_ms)
{
    int waited = 0;

    for (;;) {
        FILE *f = fopen(path, "r");

        if (f) {
            fclose(f);
            return 0;
        }
        if (waited >= timeout_ms)
            return -1;
        usleep(20000);
        waited += 20;
    }
}

/* Frames [0, DETECTOR_HIT_FRAMES) carry signal; every frame at or after that
 * is blank. See DETECTOR_HIT_FRAMES above for why this decays rather than
 * alternates. */
static int
detector_frame_is_hit(int f)
{
    return f < DETECTOR_HIT_FRAMES;
}

#endif /* DETECTOR_COMMON_H */
