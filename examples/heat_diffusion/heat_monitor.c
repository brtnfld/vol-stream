/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Live subscriber for heat_writer's stream: subscribes to /temperature and
 * prints a small ASCII heatmap plus running stats for every pushed step,
 * exactly as a real in-situ monitor would -- reading the writer's evolving
 * state as it happens, never by re-opening or polling the file (which does
 * not work while the writer holds it open).
 *
 * Grid size (first argument) must match what heat_writer was started with;
 * the two are independent processes and vol-stream has no schema to check
 * this for you.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"
#include "heat_common.h"

static const char HEAT_RAMP[] = " .:-=+*#%@";
#define HEAT_RAMP_LEN ((int)(sizeof(HEAT_RAMP) - 1))

#define HEAT_FRAME_FILE "heat_diffusion_frame.dat"
#define HEAT_FRAME_TMP "heat_diffusion_frame.dat.tmp"

/* gnuplot's `matrix` mode wants whitespace-separated rows, nothing else.
 * Written to a temp file and renamed into place so plot_live.gnuplot's
 * `reread` loop never catches a half-written frame -- rename() is atomic,
 * a partial fwrite() is not. */
static void
heat_write_frame(const double *field, int n)
{
    FILE *f = fopen(HEAT_FRAME_TMP, "w");
    int   i, j;

    if (!f)
        return;
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++)
            fprintf(f, "%s%.4f", j ? " " : "", field[i * n + j]);
        fputc('\n', f);
    }
    fclose(f);
    rename(HEAT_FRAME_TMP, HEAT_FRAME_FILE);
}

static void
heat_print_map(const double *field, int n, double lo, double hi)
{
    int    i, j;
    double span = hi - lo;

    if (span <= 0.0)
        span = 1.0;

    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            double v   = field[i * n + j];
            int    idx = (int)(((v - lo) / span) * (HEAT_RAMP_LEN - 1));

            if (idx < 0)
                idx = 0;
            if (idx >= HEAT_RAMP_LEN)
                idx = HEAT_RAMP_LEN - 1;
            putchar(HEAT_RAMP[idx]);
        }
        putchar('\n');
    }
}

int
main(int argc, char **argv)
{
    int n            = argc > 1 ? atoi(argv[1]) : HEAT_DEFAULT_N;
    int max_steps    = argc > 2 ? atoi(argv[2]) : 0; /* 0 = until the writer stops */
    int step_timeout = argc > 3 ? atoi(argv[3]) : 20000;

    hid_t       vol_id, fapl, fid, sub_space;
    hsize_t     dims[2];
    const char *paths[1] = {HEAT_DATASET};
    double      prev_mean = 0.0;
    int         have_prev = 0;
    int         seen      = 0;

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

    printf("monitor: waiting for %s to start...\n", HEAT_FNAME);
    if (heat_wait_for_file(HEAT_FNAME ".vsgroup", 15000) < 0) {
        fprintf(stderr,
                "monitor: FAIL writer never started (is heat_writer running? VOL_STREAM_NA set?)\n");
        return 1;
    }
    if ((fid = H5Fopen(HEAT_FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        fprintf(stderr, "monitor: FAIL open %s\n", HEAT_FNAME);
        return 1;
    }

    dims[0] = (hsize_t)n;
    dims[1] = (hsize_t)n;
    if ((sub_space = H5Screate_simple(2, dims, NULL)) < 0 || H5Fsubscribe(fid, 1, paths, &sub_space, NULL) < 0) {
        fprintf(stderr, "monitor: FAIL subscribe to %s (grid size must match the writer's)\n",
                HEAT_DATASET);
        return 1;
    }
    H5Sclose(sub_space);
    heat_touch(HEAT_READY_SENTINEL);

    printf("monitor: subscribed to %s (%dx%d) -- watching live\n", HEAT_DATASET, n, n);
    printf("monitor: live gnuplot matrix at ./%s (see plot_live.gnuplot)\n\n", HEAT_FRAME_FILE);

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
            const double *field  = (const double *)buf;
            size_t        count  = size / sizeof(double);
            double        sum = 0.0, lo, hi;
            size_t        i, argmax = 0;

            lo = hi = field[0];
            for (i = 0; i < count; i++) {
                sum += field[i];
                if (field[i] > hi) {
                    hi     = field[i];
                    argmax = i;
                }
                if (field[i] < lo)
                    lo = field[i];
            }

            {
                double mean  = sum / (double)count;
                double delta = have_prev ? mean - prev_mean : 0.0;
                double adelta = delta < 0.0 ? -delta : delta;

                printf("\033[2J\033[H"); /* live redraw: clear + home */
                printf("heat_diffusion: step %d (physical %llu), %zu element(s) [%s]\n\n", seen + 1,
                       (unsigned long long)phys, count, path ? path : "?");
                if (count == (size_t)n * (size_t)n) {
                    heat_print_map(field, n, 0.0, HEAT_HOT_EDGE);
                    heat_write_frame(field, n);
                }
                printf("\nmean=%.4f  max=%.4f (index %zu)  min=%.4f  step-to-step change=%+.5f\n", mean, hi,
                       argmax, lo, delta);
                if (have_prev && adelta < 1e-5)
                    printf("-- looks converged (change below threshold)\n");

                prev_mean = mean;
                have_prev = 1;
            }
        }

        free(path);
        free(buf);
        seen++;
        if (max_steps > 0 && seen >= max_steps)
            break;
    }

    /* Close gracefully, and do it promptly: this must happen while the
     * writer's rendezvous group is still alive, i.e. before the writer's
     * own post-loop settle window elapses and it closes too (see
     * heat_writer.c). Whichever side leaves a group that's already gone
     * hangs retrying against an unreachable peer instead of failing fast
     * -- run_demo.sh bounds this monitor to the writer's own step count
     * precisely so this side finishes first, gracefully, while the group
     * still has two live members. */
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    printf("\nmonitor: observed %d step(s) total\n", seen);
    return 0;
}
