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
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"
#include "rd_common.h"

static const char RD_RAMP[] = " .:-=+*#%@";
#define RD_RAMP_LEN ((int)(sizeof(RD_RAMP) - 1))

#define RD_FRAME_FILE "reaction_diffusion_frame.dat"
#define RD_FRAME_TMP "reaction_diffusion_frame.dat.tmp"

static void
rd_print_map(const double *field, int n, double lo, double hi)
{
    int    i, j;
    double span = hi - lo;

    if (span <= 0.0)
        span = 1.0;

    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            double v   = field[i * n + j];
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
rd_write_frame(const double *field, int n)
{
    FILE *f = fopen(RD_FRAME_TMP, "w");
    int   i, j;

    if (!f)
        return;
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++)
            fprintf(f, "%s%.4f", j ? " " : "", field[i * n + j]);
        fputc('\n', f);
    }
    fclose(f);
    rename(RD_FRAME_TMP, RD_FRAME_FILE);
}

int
main(int argc, char **argv)
{
    int n            = argc > 1 ? atoi(argv[1]) : RD_DEFAULT_N;
    int max_steps    = argc > 2 ? atoi(argv[2]) : 0; /* 0 = until the writer stops */
    int step_timeout = argc > 3 ? atoi(argv[3]) : 20000;

    hid_t       vol_id, fapl, fid, sub_space;
    hsize_t     dims[2];
    const char *paths[1] = {RD_DATASET_V};
    int         seen     = 0;

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
    rd_touch(RD_READY_SENTINEL);

    printf("monitor: subscribed to %s (%dx%d) -- watching live\n", RD_DATASET_V, n, n);
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
            const double *field = (const double *)buf;
            size_t        count = size / sizeof(double);
            double        sum = 0.0, lo, hi;
            size_t        i;

            lo = hi = field[0];
            for (i = 0; i < count; i++) {
                sum += field[i];
                if (field[i] > hi)
                    hi = field[i];
                if (field[i] < lo)
                    lo = field[i];
            }

            {
                double mean = sum / (double)count;

                printf("\033[2J\033[H"); /* live redraw: clear + home */
                printf("reaction_diffusion: step %d (physical %llu), %zu element(s) [%s]\n\n", seen + 1,
                       (unsigned long long)phys, count, path ? path : "?");
                if (count == (size_t)n * (size_t)n) {
                    rd_print_map(field, n, lo, hi);
                    rd_write_frame(field, n);
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
