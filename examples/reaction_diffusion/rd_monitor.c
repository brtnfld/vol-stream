/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Live subscriber for rd_writer's stream: subscribes to /V only (the
 * reaction product -- the field that actually shows the pattern; U is
 * roughly its complement and less visually informative) and redraws an
 * ASCII heatmap plus stats for every pushed step. Unlike heat_monitor,
 * this one contrast-stretches each frame to V's own min/max rather than a
 * fixed 0..100 range, since V has no fixed physical bound the way a
 * temperature does.
 *
 * Grid size (first argument) must match what rd_writer was started with.
 *
 * --float: ask the writer to deliver /V as 4-byte float instead of the
 * 8-byte double it is stored as (H5Fsubscribe_type). The FILE keeps full
 * double precision for the checkpoint; only this subscriber's stream is
 * narrowed, and the conversion happens on the writer before marshaling, so
 * the wire payload really is halved rather than merely cast on arrival.
 * Exactly the "full fidelity to the archive, reduced precision to the live
 * viz, from one end_step()" case -- and for a heatmap, float is already more
 * precision than the terminal can show.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"
#include "rd_common.h"

static const char RD_RAMP[] = " .:-=+*#%@";
#define RD_RAMP_LEN ((int)(sizeof(RD_RAMP) - 1))

/* One element, whichever width the stream delivered. Keeping the rendering
 * and stats code width-agnostic is the whole accommodation --float needs. */
static double
rd_at(const void *field, int as_float, size_t i)
{
    return as_float ? (double)((const float *)field)[i] : ((const double *)field)[i];
}

#define RD_FRAME_FILE "reaction_diffusion_frame.dat"
#define RD_FRAME_TMP "reaction_diffusion_frame.dat.tmp"

static void
rd_print_map(const void *field, int as_float, int n, double lo, double hi)
{
    int    i, j;
    double span = hi - lo;

    if (span <= 0.0)
        span = 1.0;

    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            double v   = rd_at(field, as_float, (size_t)i * n + j);
            int    idx = (int)(((v - lo) / span) * (RD_RAMP_LEN - 1));

            if (idx < 0)
                idx = 0;
            if (idx >= RD_RAMP_LEN)
                idx = RD_RAMP_LEN - 1;
            putchar(RD_RAMP[idx]);
        }
        putchar('\n');
    }
}

/* Written to a temp file and renamed into place, same reasoning as
 * heat_monitor.c's heat_write_frame(): rename() is atomic, a partial
 * fwrite() during plot_live.gnuplot's reread is not. */
static void
rd_write_frame(const void *field, int as_float, int n)
{
    FILE *f = fopen(RD_FRAME_TMP, "w");
    int   i, j;

    if (!f)
        return;
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++)
            fprintf(f, "%s%.4f", j ? " " : "", rd_at(field, as_float, (size_t)i * n + j));
        fputc('\n', f);
    }
    fclose(f);
    rename(RD_FRAME_TMP, RD_FRAME_FILE);
}

int
main(int argc, char **argv)
{
    int n = RD_DEFAULT_N, max_steps = 0, step_timeout = 20000;
    int as_float = 0, positional = 0, a;

    hid_t       vol_id, fapl, fid, sub_space;
    hsize_t     dims[2];
    const char *paths[1] = {RD_DATASET_V};
    int         seen     = 0;

    for (a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--float") == 0)
            as_float = 1;
        else
            switch (positional++) {
                case 0: n = atoi(argv[a]); break;
                case 1: max_steps = atoi(argv[a]); break;
                case 2: step_timeout = atoi(argv[a]); break;
                default: break;
            }
    }

    setenv("VOL_STREAM_NA", "na+sm", 0);

    if ((vol_id = H5VL_stream_register()) < 0) {
        fprintf(stderr, "monitor: FAIL register vol-stream\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        fprintf(stderr, "monitor: FAIL fapl\n");
        return 1;
    }

    printf("monitor: waiting for %s to start...\n", RD_FNAME);
    if (rd_wait_for_file(RD_FNAME ".vsgroup", 15000) < 0) {
        fprintf(stderr, "monitor: FAIL writer never started (is rd_writer running? VOL_STREAM_NA set?)\n");
        return 1;
    }
    if ((fid = H5Fopen(RD_FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        fprintf(stderr, "monitor: FAIL open %s\n", RD_FNAME);
        return 1;
    }

    dims[0] = (hsize_t)n;
    dims[1] = (hsize_t)n;
    if ((sub_space = H5Screate_simple(2, dims, NULL)) < 0 || H5Fsubscribe(fid, 1, paths, &sub_space, NULL) < 0) {
        fprintf(stderr, "monitor: FAIL subscribe to %s (grid size must match the writer's)\n",
                RD_DATASET_V);
        return 1;
    }
    H5Sclose(sub_space);

    /* The narrowing, applied AFTER the subscription it narrows -- a later
     * H5Fsubscribe() on the same path would clear it, so the order matters. */
    if (as_float && H5Fsubscribe_type(fid, RD_DATASET_V, H5T_NATIVE_FLOAT) < 0) {
        /* Not fatal, deliberately: the stream still works at the dataset's
         * own precision, and returning here would strand a writer that is
         * waiting for this subscriber. The consumer loop below reports the
         * width actually delivered rather than trusting the request. */
        fprintf(stderr, "monitor: WARNING could not narrow %s to float; taking double\n", RD_DATASET_V);
        as_float = 0;
    }

    rd_touch(RD_READY_SENTINEL);

    printf("monitor: subscribed to %s (%dx%d) -- watching live\n", RD_DATASET_V, n, n);
    if (as_float)
        printf("monitor: requested 4-byte float delivery; the file keeps 8-byte double\n");
    printf("monitor: live gnuplot matrix at ./%s (see plot_live.gnuplot)\n\n", RD_FRAME_FILE);

    for (;;) {
        uint64_t phys = 0, elem_start = 0, elem_count = 0;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;

        if (H5Fget_subscribed_data(fid, (uint64_t)step_timeout, &phys, &path, &buf, &size, &elem_start,
                                    &elem_count) < 0) {
            printf("monitor: no further data (writer finished or idle) after %d step(s)\n", seen);
            break;
        }

        if (size == 0) {
            free(path);
            free(buf);
            continue;
        }

        {
            /* Trust the bytes actually delivered, not the request: a writer
             * that could not convert sends the dataset's own type instead
             * (over-send never under-send), so infer the width from size and
             * element count rather than assuming --float was honored. */
            int           got_float = (elem_count > 0 && size / (size_t)elem_count == sizeof(float));
            const void   *field     = buf;
            size_t        count     = size / (got_float ? sizeof(float) : sizeof(double));
            double        sum = 0.0, lo, hi;
            size_t        i;

            if (seen == 0 && as_float)
                printf("monitor: delivered %zu bytes for %llu elements -> %s (double would be %zu)\n\n",
                       size, (unsigned long long)elem_count, got_float ? "float, halved" : "double",
                       (size_t)elem_count * sizeof(double));

            lo = hi = rd_at(field, got_float, 0);
            for (i = 0; i < count; i++) {
                double x = rd_at(field, got_float, i);

                sum += x;
                if (x > hi)
                    hi = x;
                if (x < lo)
                    lo = x;
            }

            {
                double mean = sum / (double)count;

                printf("\033[2J\033[H"); /* live redraw: clear + home */
                printf("reaction_diffusion: step %d (physical %llu), %zu element(s) [%s]\n\n", seen + 1,
                       (unsigned long long)phys, count, path ? path : "?");
                if (count == (size_t)n * (size_t)n) {
                    rd_print_map(field, got_float, n, lo, hi);
                    rd_write_frame(field, got_float, n);
                }
                printf("\nV: mean=%.4f  max=%.4f  min=%.4f  (auto-contrast range shown above)\n", mean, hi,
                       lo);
            }
        }

        free(path);
        free(buf);
        seen++;
        if (max_steps > 0 && seen >= max_steps)
            break;
    }

    /* Close gracefully, and do it promptly: see heat_monitor.c's comment
     * on why this must happen while the writer's rendezvous group is
     * still alive (i.e. before its post-loop settle window elapses) --
     * run_demo.sh bounds this monitor to the writer's own step count for
     * exactly that reason. */
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    printf("\nmonitor: observed %d step(s) total\n", seen);
    return 0;
}
