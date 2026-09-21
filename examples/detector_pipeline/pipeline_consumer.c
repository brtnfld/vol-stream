/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The consumer half of the RFC section A.2 use case: the four components
 * hanging off one detector in the deployed P02.2 pipeline, each getting its
 * own view of one acquisition, and every one of them discovering the stream's
 * structure rather than being told it.
 *
 *   discover    Ask H5Fget_stream_schema() and print what came back. Does not
 *               subscribe. This is A.2's central claim on its own: a process
 *               that has never opened the file, and has no agreement with the
 *               writer beyond a filename, learns every path, datatype and
 *               extent -- because the writer's steps carry H5Tencode()/
 *               H5Sencode2() bytes and the manifest IS the structure. At
 *               P02.2 this same information is a separate stream-metadata
 *               channel that a distinct NeXus-writer service consumes.
 *   archive     Full fidelity, no narrowing -- the NeXus writer's role.
 *   viewer      Narrowed to int16 and compressed: the live view, which wants
 *               small and tolerates loss of range.
 *   analysis    One detector panel, as a row band -- the stitcher's per-panel
 *               input.
 *   hitfinder   H5Fsubscribe_predicate(GT) -- the veto/Cheetah role, where a
 *               blank frame must cost zero bytes rather than few.
 *
 * What makes this more than a rerun of examples/narrowing_demo is where the
 * shapes come from. narrowing_demo's subscriber builds its dataspace out of
 * NARROWING_NELEM, a constant it shares with the writer through a header --
 * as t_schema.c's comment notes, every example and test in this repo did
 * that, by construction, because before H5Fget_stream_schema() there was no
 * other way. Nothing here does. The only thing this program takes from
 * detector_common.h is the filename, the timeouts, and the image path as a
 * STRING TO MATCH ON -- never a dimension, never an element count, never a
 * datatype. Every buffer it sizes and every selection it builds comes from
 * the ids discovery handed back. Delete the geometry macros from
 * detector_common.h and this file still compiles.
 *
 * Run it alongside detector_writer via run_pipeline.sh, or by hand in
 * separate terminals.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"
#include "detector_common.h"

enum mode { MODE_DISCOVER, MODE_ARCHIVE, MODE_VIEWER, MODE_ANALYSIS, MODE_HITFINDER };

static const char *
class_name(H5T_class_t c)
{
    switch (c) {
        case H5T_INTEGER:
            return "integer";
        case H5T_FLOAT:
            return "float";
        case H5T_STRING:
            return "string";
        case H5T_COMPOUND:
            return "compound";
        case H5T_ENUM:
            return "enum";
        case H5T_OPAQUE:
            return "opaque";
        case H5T_ARRAY:
            return "array";
        case H5T_REFERENCE:
            return "reference";
        default:
            return "other";
    }
}

/* Print one discovered variable the way a viewer's variable list would show
 * it -- entirely from the ids, with nothing known in advance. */
static void
print_var(const H5F_stream_var_t *v)
{
    hsize_t dims[H5S_MAX_RANK];
    int     rank, i;
    size_t  sz = H5Tget_size(v->type_id);

    rank = H5Sget_simple_extent_ndims(v->space_id);
    if (rank < 0)
        rank = 0;
    H5Sget_simple_extent_dims(v->space_id, dims, NULL);

    printf("    %-46s %s%zu", v->path, class_name(H5Tget_class(v->type_id)), sz * 8);
    printf("  [");
    for (i = 0; i < rank; i++)
        printf("%s%llu", i ? " x " : "", (unsigned long long)dims[i]);
    printf("]%s\n", v->is_attr ? "  (attribute)" : "");
}

/* Locate the image stack among the discovered variables. Keys off the path,
 * the way a real NeXus consumer keys off an NXdata attribute -- recognition,
 * not shape. Everything about its geometry still comes from the schema. */
static const H5F_stream_var_t *
find_image(const H5F_stream_var_t *vars, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (!vars[i].is_attr && strcmp(vars[i].path, DETECTOR_IMAGE_PATH) == 0)
            return &vars[i];
    return NULL;
}

int
main(int argc, char **argv)
{
    enum mode   mode;
    const char *label;
    int         module      = argc > 2 ? atoi(argv[2]) : 0;
    int         nmodules    = argc > 3 ? atoi(argv[3]) : DETECTOR_MODULES;
    int         max_pushes  = argc > 4 ? atoi(argv[4]) : DETECTOR_NFRAMES;
    int         step_timeout = argc > 5 ? atoi(argv[5]) : 8000;

    hid_t   vol_id, fapl, fid;
    hid_t   sub_space = H5I_INVALID_HID, dcpl = H5I_INVALID_HID;
    size_t  n_vars = 0;
    H5F_stream_var_t *vars = NULL;
    uint64_t schema_step   = 0;
    const H5F_stream_var_t *img;
    hsize_t  dims[H5S_MAX_RANK];
    int      rank;
    int      pushes = 0;
    size_t   total_bytes = 0, total_elems = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2)
        goto usage;
    if (strcmp(argv[1], "discover") == 0) {
        mode  = MODE_DISCOVER;
        label = "discover (H5Fget_stream_schema only)";
    }
    else if (strcmp(argv[1], "archive") == 0) {
        mode  = MODE_ARCHIVE;
        label = "archive (full fidelity)";
    }
    else if (strcmp(argv[1], "viewer") == 0) {
        mode  = MODE_VIEWER;
        label = "viewer (int16 + deflate)";
    }
    else if (strcmp(argv[1], "analysis") == 0) {
        mode  = MODE_ANALYSIS;
        label = "analysis (one panel)";
    }
    else if (strcmp(argv[1], "hitfinder") == 0) {
        mode  = MODE_HITFINDER;
        label = "hitfinder (predicate GT)";
    }
    else {
    usage:
        fprintf(stderr,
                "usage: %s discover|archive|viewer|analysis|hitfinder "
                "[module] [nmodules] [max-pushes] [step-timeout-ms]\n",
                argv[0]);
        return 1;
    }

    if (mode == MODE_VIEWER && H5Zfilter_avail(H5Z_FILTER_DEFLATE) <= 0) {
        fprintf(stderr, "viewer: FAIL this HDF5 has no deflate filter -- rebuild with "
                        "-DHDF5_ENABLE_ZLIB_SUPPORT=ON (it defaults to OFF)\n");
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

    printf("%s: waiting for %s to start...\n", argv[1], DETECTOR_FNAME);
    if (detector_wait_for_file(DETECTOR_FNAME ".vsgroup", 15000) < 0) {
        fprintf(stderr, "%s: FAIL writer never started (is detector_writer running?)\n", argv[1]);
        return 1;
    }
    if ((fid = H5Fopen(DETECTOR_FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        fprintf(stderr, "%s: FAIL open %s\n", argv[1], DETECTOR_FNAME);
        return 1;
    }

    /* ---- Discovery. Nothing below this point uses a shape constant. ---- */

    printf("%s: asking the writer what this stream carries...\n", argv[1]);
    if (H5Fget_stream_schema(fid, DETECTOR_BARRIER_TIMEOUT_MS + 5000, &schema_step, &n_vars, &vars) < 0) {
        fprintf(stderr, "%s: FAIL H5Fget_stream_schema (writer committed a step yet?)\n", argv[1]);
        return 1;
    }

    printf("%s: schema as of step %llu -- %zu variable(s), discovered, not configured:\n", argv[1],
           (unsigned long long)schema_step, n_vars);
    {
        size_t i;

        for (i = 0; i < n_vars; i++)
            print_var(&vars[i]);
    }

    if (mode == MODE_DISCOVER) {
        printf("\n%s: that is the whole point -- no second metadata channel, no writer service to\n",
               argv[1]);
        printf("%s: reassemble the layout. The step manifest carries the writer's own H5Tencode()/\n",
               argv[1]);
        printf("%s: H5Sencode2() bytes, so the structure travels with the data.\n", argv[1]);
        H5Ffree_stream_schema(n_vars, vars);
        H5Fclose(fid);
        H5Pclose(fapl);
        H5VLclose(vol_id);
        return 0;
    }

    if (NULL == (img = find_image(vars, n_vars))) {
        fprintf(stderr, "%s: FAIL %s not in the discovered schema\n", argv[1], DETECTOR_IMAGE_PATH);
        return 1;
    }
    if ((rank = H5Sget_simple_extent_ndims(img->space_id)) < 2) {
        fprintf(stderr, "%s: FAIL image has rank %d, expected >= 2\n", argv[1], rank);
        return 1;
    }
    H5Sget_simple_extent_dims(img->space_id, dims, NULL);

    /* The subscription selection, built from the DISCOVERED extent. */
    if ((sub_space = H5Scopy(img->space_id)) < 0) {
        fprintf(stderr, "%s: FAIL copy discovered dataspace\n", argv[1]);
        return 1;
    }

    if (mode == MODE_ANALYSIS) {
        /* One panel. The only instrument knowledge here is how many panels
         * the detector has -- which is configuration, not stream structure.
         * The panel's pixel geometry is computed from the discovered row
         * count, so this adapts to whatever the writer is actually sending.
         *
         * A row band is contiguous in a row-major frame. A *column* band of
         * the same area is not, and is the case RFC appendix A's finding F5
         * shows is still over-sent -- 514 runs against a 256-run cap. Panels
         * stacked in the slow dimension is the favourable geometry, and that
         * is stated rather than quietly relied on. */
        hsize_t start[H5S_MAX_RANK] = {0};
        hsize_t count[H5S_MAX_RANK];
        int     i;

        if (nmodules < 1 || module < 0 || module >= nmodules) {
            fprintf(stderr, "analysis: FAIL module %d of %d is out of range\n", module, nmodules);
            return 1;
        }
        if (dims[rank - 2] % (hsize_t)nmodules != 0) {
            fprintf(stderr, "analysis: FAIL discovered %llu rows do not divide into %d modules\n",
                    (unsigned long long)dims[rank - 2], nmodules);
            return 1;
        }

        for (i = 0; i < rank; i++)
            count[i] = dims[i];
        count[rank - 2] = dims[rank - 2] / (hsize_t)nmodules;
        start[rank - 2] = count[rank - 2] * (hsize_t)module;

        if (H5Sselect_hyperslab(sub_space, H5S_SELECT_SET, start, NULL, count, NULL) < 0) {
            fprintf(stderr, "analysis: FAIL select panel %d\n", module);
            return 1;
        }
        printf("analysis: panel %d of %d -- rows [%llu, %llu) of the discovered %llu\n", module, nmodules,
               (unsigned long long)start[rank - 2],
               (unsigned long long)(start[rank - 2] + count[rank - 2]),
               (unsigned long long)dims[rank - 2]);
    }

    if (mode == MODE_VIEWER) {
        /* Chunk the re-filter pipeline over one whole discovered frame. */
        hsize_t chunk[H5S_MAX_RANK];
        int     i;

        for (i = 0; i < rank; i++)
            chunk[i] = dims[i];
        chunk[0] = 1;

        if ((dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 || H5Pset_chunk(dcpl, rank, chunk) < 0 ||
            H5Pset_deflate(dcpl, 6) < 0) {
            fprintf(stderr, "viewer: FAIL configure deflate dcpl\n");
            return 1;
        }
    }

    {
        const char *const paths[1]  = {DETECTOR_IMAGE_PATH};
        const hid_t       spaces[1] = {sub_space};
        const hid_t       plists[1] = {dcpl};

        if (H5Fsubscribe(fid, 1, paths, spaces, mode == MODE_VIEWER ? plists : NULL) < 0) {
            fprintf(stderr, "%s: FAIL subscribe to %s\n", argv[1], DETECTOR_IMAGE_PATH);
            return 1;
        }
    }

    /* Narrowings are applied AFTER the subscription they narrow -- a later
     * H5Fsubscribe() on the same path clears them, so order matters. */
    if (mode == MODE_VIEWER) {
        if (H5Fsubscribe_type(fid, DETECTOR_IMAGE_PATH, H5T_NATIVE_INT16) < 0)
            fprintf(stderr, "viewer: WARNING could not narrow to int16; taking native width\n");
    }
    if (mode == MODE_HITFINDER) {
        int32_t threshold = DETECTOR_HIT_THRESHOLD;

        /* The predicate's type is the image's own DISCOVERED type, not a
         * constant -- H5T_NATIVE_INT32 would be an assumption this program
         * has no business making. */
        if (H5Fsubscribe_predicate(fid, DETECTOR_IMAGE_PATH, H5VL_STREAM_PRED_GT, img->type_id,
                                    &threshold) < 0) {
            fprintf(stderr, "hitfinder: FAIL H5Fsubscribe_predicate\n");
            return 1;
        }
        printf("hitfinder: only elements > %d will be marshaled; a frame with none sends nothing\n",
               DETECTOR_HIT_THRESHOLD);
    }

    printf("%s: subscribed as %s -- watching up to %d push(es)\n", argv[1], label, max_pushes);

    {
        int misses     = 0;
        int max_misses = (DETECTOR_BARRIER_TIMEOUT_MS + 5000) / step_timeout + 1;

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
            total_bytes += size;
            total_elems += (size_t)elem_count;
            printf("%s: push %2d  step %llu  %llu element(s)  %zu byte(s)\n", argv[1], pushes,
                   (unsigned long long)phys, (unsigned long long)elem_count, size);

            free(path);
            free(buf);
            if (max_pushes > 0 && pushes >= max_pushes)
                break;
        }
    }

    printf("\n%s: summary -- %d push(es), %zu element(s), %zu byte(s) delivered\n", argv[1], pushes,
           total_elems, total_bytes);
    if (mode == MODE_VIEWER)
        printf("viewer: delivered bytes are DECODED -- H5Fget_subscribed_data() never hands back\n"
               "viewer: compressed values, so the real wire size is in the writer's own\n"
               "viewer: \"refilter\" line (VOL_STREAM_DEBUG_REFILTER=1). See RFC appendix A, F7.\n");
    if (mode == MODE_HITFINDER)
        printf("hitfinder: frames whose elements were all below %d produced no push at all\n",
               DETECTOR_HIT_THRESHOLD);

    if (sub_space != H5I_INVALID_HID)
        H5Sclose(sub_space);
    if (dcpl != H5I_INVALID_HID)
        H5Pclose(dcpl);
    H5Ffree_stream_schema(n_vars, vars);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    return 0;
}
