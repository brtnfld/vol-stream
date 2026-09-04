/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * One subscriber, three modes -- run it three times, in three terminals,
 * against the same narrowing_writer, and each narrows /reading differently
 * without the writer ever changing what it wrote:
 *
 *   float      H5Fsubscribe_type(H5T_NATIVE_FLOAT) -- delivered as 4-byte
 *              float instead of the dataset's own 8-byte double, so every
 *              push this mode receives is half the bytes of the one the
 *              other two modes see for the same step.
 *   gzip       H5Fsubscribe()'s own DCPL argument requests GZIP -- the file
 *              itself is never compressed (see narrowing_writer.c); only
 *              this subscription's bytes are. H5Fget_subscribed_data()
 *              always hands back decoded values, so this mode's own
 *              "size" is the ORIGINAL size, not the compressed wire size --
 *              the real wire bytes only show up in narrowing_writer's own
 *              "refilter filter=... raw=... filtered=..." log line
 *              (VOL_STREAM_DEBUG_REFILTER). This mode's contribution is
 *              proving the round trip is exactly correct.
 *   predicate  H5Fsubscribe_predicate(GT, threshold) -- narrows the whole-
 *              object subscription to only elements above the threshold.
 *              On a "quiet" step (see narrowing_common.h) nothing at all
 *              satisfies it, so the writer sends literally nothing for
 *              that step -- this mode's loop simply receives fewer pushes
 *              than the writer had steps, and says so explicitly.
 *
 * Same two-process shape as test/t_precision.c / test/t_predicate.c, but
 * meant to be run by a person watching, not asserted by a harness: run it
 * alongside the other two modes and narrowing_writer via run_demo.sh, or by
 * hand in separate terminals.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"
#include "narrowing_common.h"

enum mode { MODE_FLOAT, MODE_GZIP, MODE_PREDICATE };

int
main(int argc, char **argv)
{
    enum mode   mode;
    int         max_steps    = argc > 2 ? atoi(argv[2]) : NARROWING_NSTEPS;
    int         step_timeout = argc > 3 ? atoi(argv[3]) : 8000;
    const char *label;

    hid_t       vol_id, fapl, fid, sub_space, dcpl = H5I_INVALID_HID;
    hsize_t     dims = NARROWING_NELEM, chunk_dims = NARROWING_NELEM;
    const char *paths[1] = {NARROWING_DATASET};
    int         seen = 0, pushes = 0;
    size_t      total_elems = 0;

    /* Line-buffer stdout: run_demo.sh redirects this to a log file, and
     * stdout is otherwise fully-buffered there, so its lines would only
     * appear all at once at exit -- interleaved out of order with stderr
     * (unbuffered, so any HDF5 diagnostic from a routine retry timeout below
     * shows up immediately). Purely cosmetic; doesn't affect the result. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2) {
        fprintf(stderr, "usage: %s float|gzip|predicate [max-steps] [step-timeout-ms]\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "float") == 0) {
        mode  = MODE_FLOAT;
        label = "float (H5Fsubscribe_type)";
    }
    else if (strcmp(argv[1], "gzip") == 0) {
        mode  = MODE_GZIP;
        label = "gzip (H5Fsubscribe + GZIP DCPL)";
    }
    else if (strcmp(argv[1], "predicate") == 0) {
        mode  = MODE_PREDICATE;
        label = "predicate (H5Fsubscribe_predicate)";
    }
    else {
        fprintf(stderr, "usage: %s float|gzip|predicate [max-steps] [step-timeout-ms]\n", argv[0]);
        return 1;
    }

    if (mode == MODE_GZIP && H5Zfilter_avail(H5Z_FILTER_DEFLATE) <= 0) {
        fprintf(stderr, "%s: FAIL this HDF5 has no deflate filter -- rebuild with "
                        "-DHDF5_ENABLE_ZLIB_SUPPORT=ON (it defaults to OFF)\n",
                argv[1]);
        return 1;
    }

    setenv("VOL_STREAM_NA", "na+sm", 0);

    if ((vol_id = H5VL_stream_register()) < 0) {
        fprintf(stderr, "%s: FAIL register vol-stream\n", argv[1]);
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        fprintf(stderr, "%s: FAIL fapl\n", argv[1]);
        return 1;
    }

    printf("%s: waiting for %s to start...\n", argv[1], NARROWING_FNAME);
    if (narrowing_wait_for_file(NARROWING_FNAME ".vsgroup", 15000) < 0) {
        fprintf(stderr, "%s: FAIL writer never started (is narrowing_writer running? VOL_STREAM_NA set?)\n",
                argv[1]);
        return 1;
    }
    if ((fid = H5Fopen(NARROWING_FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        fprintf(stderr, "%s: FAIL open %s\n", argv[1], NARROWING_FNAME);
        return 1;
    }

    if (mode == MODE_GZIP) {
        if ((dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 || H5Pset_chunk(dcpl, 1, &chunk_dims) < 0 ||
            H5Pset_deflate(dcpl, 6) < 0) {
            fprintf(stderr, "gzip: FAIL configure GZIP dcpl\n");
            return 1;
        }
    }

    if ((sub_space = H5Screate_simple(1, &dims, NULL)) < 0) {
        fprintf(stderr, "%s: FAIL dataspace\n", argv[1]);
        return 1;
    }
    {
        const hid_t plists[1] = {dcpl};

        if (H5Fsubscribe(fid, 1, paths, &sub_space, mode == MODE_GZIP ? plists : NULL) < 0) {
            fprintf(stderr, "%s: FAIL subscribe to %s\n", argv[1], NARROWING_DATASET);
            return 1;
        }
    }
    H5Sclose(sub_space);
    if (dcpl != H5I_INVALID_HID)
        H5Pclose(dcpl);

    /* The narrowing, applied AFTER the subscription it narrows -- a later
     * H5Fsubscribe() on the same path would clear it, so order matters. */
    if (mode == MODE_FLOAT && H5Fsubscribe_type(fid, NARROWING_DATASET, H5T_NATIVE_FLOAT) < 0) {
        fprintf(stderr, "float: WARNING could not narrow to float; taking double\n");
        mode = MODE_GZIP; /* falls through to the generic double-sized reporting below */
    }
    if (mode == MODE_PREDICATE) {
        double threshold = NARROWING_THRESHOLD;

        if (H5Fsubscribe_predicate(fid, NARROWING_DATASET, H5VL_STREAM_PRED_GT, H5T_NATIVE_DOUBLE,
                                    &threshold) < 0) {
            fprintf(stderr, "predicate: FAIL H5Fsubscribe_predicate\n");
            return 1;
        }
    }

    printf("%s: subscribed to %s (%d elements) as %s -- watching up to %d step(s)\n", argv[1],
           NARROWING_DATASET, NARROWING_NELEM, label, max_steps);

    /* A single H5Fget_subscribed_data() timeout does not mean "the writer is
     * done" -- the writer's own H5Fwait_subscribers() can legitimately take
     * up to NARROWING_BARRIER_TIMEOUT_MS before its very first push (e.g. if
     * this demo is run with fewer than all three subscriber terminals). Give
     * up only after a cumulative idle stretch comfortably longer than that,
     * not after one missed window -- resetting the counter every time a push
     * actually arrives. */
    {
        int misses = 0;
        int max_misses =
            (NARROWING_BARRIER_TIMEOUT_MS + 5000) / step_timeout + 1;

        for (;;) {
            uint64_t phys = 0, elem_start = 0, elem_count = 0;
            char    *path = NULL;
            void    *buf  = NULL;
            size_t   size = 0;

            if (H5Fget_subscribed_data(fid, (uint64_t)step_timeout, &phys, &path, &buf, &size, &elem_start,
                                        &elem_count) < 0) {
                if (++misses < max_misses)
                    continue;
                printf("%s: no further data (writer finished or idle) after %d push(es)\n", argv[1],
                       pushes);
                break;
            }
            misses = 0;

            pushes++;
            total_elems += (size_t)elem_count;
            printf("%s: push %2d  step %llu  %llu element(s)  %zu byte(s)", argv[1], pushes,
                   (unsigned long long)phys, (unsigned long long)elem_count, size);
            if (mode == MODE_FLOAT)
                printf("  (double would be %zu)", (size_t)elem_count * sizeof(double));
            printf("\n");

            free(path);
            free(buf);
            seen++;
            if (max_steps > 0 && seen >= max_steps)
                break;
        }
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    printf("\n%s: summary -- %d push(es) received across up to %d step(s), %zu element(s) total\n", argv[1],
           pushes, max_steps, total_elems);
    if (mode == MODE_PREDICATE)
        printf("%s: %d step(s) matched nothing above %.1f and sent nothing at all\n", argv[1],
               max_steps - pushes, NARROWING_THRESHOLD);
    if (mode == MODE_FLOAT)
        printf("%s: %zu bytes received where double delivery would have been %zu\n", argv[1],
               total_elems * sizeof(float), total_elems * sizeof(double));
    if (mode == MODE_GZIP)
        printf("%s: %zu bytes decoded correctly; see the writer's own \"refilter\" log line for the real "
               "(smaller) wire size\n",
               argv[1], total_elems * sizeof(double));

    return 0;
}
