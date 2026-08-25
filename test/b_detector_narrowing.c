/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * PER-SUBSCRIBER COMPRESSION on a live detector stream, measured.
 *
 * A file has exactly one filter pipeline, chosen at acquisition and baked
 * into the DCPL. Every consumer of that file therefore gets the same
 * compression, whether or not it is the right trade for them: the archive
 * wants lossless and does not care about latency, the live viewer wants
 * small and does not care about the last bit, the hit finder wants only the
 * pixels that fired. A subscription protocol can serve all of them from one
 * H5Dwrite(), each with its own pipeline, applied at the source. That is
 * the claim this file exists to price.
 *
 * GEOMETRY. One DECTRIS EIGER2-class detector module, 1030 x 514 pixels,
 * written one frame per step into /entry/data/data as a 3-D dataset with an
 * unlimited leading dimension and one chunk per frame -- the NeXus/NXdata
 * layout EIGER's own file writer produces. int32 rather than the detector's
 * native uint32 so that every filter measured here accepts the type
 * (H5Z-ZFP handles int32, not unsigned); photon counts are small and
 * positive either way, so nothing about the data changes.
 *
 * THREE SCENES, because a compression benchmark that reports one ratio on
 * one scene is reporting a property of that scene:
 *
 *   hit    a diffraction pattern: sparse Bragg peaks on a weak powder ring,
 *          a few percent of pixels non-zero -- the frame worth keeping
 *   blank  a dark frame: no crystal in the beam, essentially all zero. The
 *          overwhelming majority of frames in a serial crystallography run,
 *          and the reason facilities veto rather than store
 *   dense   speckle everywhere, as in XPCS -- the case where none of the
 *          cheap wins are available and compression has to do real work
 *
 * SUBSCRIBERS, twelve, each its own OS process, all on the SAME object:
 *
 *   archive     no narrowing -- the acquisition rate, and the baseline
 *   bslz4       bitshuffle + LZ4 (filter 32008), the EIGER standard
 *   zstd        Zstandard (32015)
 *   deflate     gzip (filter 1), HDF5's built-in, as a reference point
 *   zfp         ZFP (32013) in fixed-precision mode -- LOSSY, and the only
 *               subscriber here whose values are not bit-identical
 *   roi_rows    a contiguous band of detector rows: one flat run
 *   roi_cols    a band of detector COLUMNS: one run per row, the geometry
 *               that selection routing silently declined to narrow until
 *               the extent-compatibility and clip-before-cap fixes
 *   narrowed    H5Fsubscribe_type() to int16, no filter at all
 *   photons     H5Fsubscribe_predicate(GT 0) -- sparsification: only the
 *               pixels that actually registered a count
 *   same_pipe   requests exactly the pipeline the writer already wrote the
 *               object under, which is what the M8.5.1 zero-copy fast path
 *               exists to serve without recompressing
 *
 * The writer creates the object WITH a bslz4 pipeline, as a real beamline
 * would, so same_pipe is a fair test of that fast path rather than a
 * contrived one.
 *
 * WIRE BYTES, not delivered bytes. H5Fget_subscribed_data() always hands
 * back decoded values -- transparent by design -- so a subscriber measuring
 * what it receives measures the DECOMPRESSED size and sees a filter save
 * exactly nothing. The writer's opt-in VOL_STREAM_DEBUG_REFILTER diagnostic
 * is the only place the real number exists; this file captures and parses
 * it, attributing lines to subscribers by the filter id the diagnostic
 * reports. That an application cannot obtain these numbers for itself is
 * itself a finding, not an inconvenience of this benchmark.
 *
 * Values are checked, not just counted: every lossless subscriber verifies
 * what it receives against the scene it can recompute, so a narrowing that
 * silently dropped or corrupted data fails here rather than looking like a
 * spectacular compression ratio.
 *
 * A NOTE ON A CRASH THIS TEST USED TO SHOW, since the history is useful.
 * Roughly one run in four died with SIGSEGV in a subscriber during teardown,
 * never the writer, always preceded by SSG "not a member" errors. It was not
 * this benchmark: it is a use-after-free in mochi-ssg's group teardown, where
 * the reference a SWIM handler holds does not actually prevent the group from
 * being freed underneath it. Diagnosed in mochi-hpc/mochi-ssg#74 -- the same
 * defect as their #62, open since 2022 -- and fixed by #75. It fired at
 * teardown, after every subscriber had received and verified its data, so the
 * byte counts below were never affected; clean runs were bit-identical.
 *
 * Two things keep it away from a stock SSG: vs_tr_writer_start_group() now
 * uses a less aggressive SWIM suspect timeout (see its comment), and the
 * connector is expected to be built against a fixed SSG in time. If you see
 * a teardown SIGSEGV here again, check which SSG is loaded before suspecting
 * this file.
 *
 * Only compiled/run when VOL_STREAM_HAVE_MERCURY is on; see
 * test/CMakeLists.txt. Filters beyond HDF5's built-in deflate are OPTIONAL:
 * each subscriber that needs one checks H5Zfilter_avail() and downgrades to
 * reporting itself unavailable rather than failing the run, so this builds
 * and passes on a stock HDF5 with no plugins installed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

/* One EIGER2 detector module. */
#define DET_W   1030
#define DET_H   514
#define NFRAMES 16

#define FRAME_ELEMS ((size_t)DET_H * DET_W)

/* Registered filter ids (see The HDF Group's RegisteredFilterPlugins.md). */
#define FILTER_DEFLATE 1
#define FILTER_LZ4     32004
#define FILTER_BSHUF   32008
#define FILTER_ZFP     32013
#define FILTER_ZSTD    32015

/* Bitshuffle's cd_values: block size 0 = "choose automatically", then the
 * compression backend. 2 is BSHUF_H5_COMPRESS_LZ4, which is what "bslz4"
 * means and what EIGER writes. */
#define BSHUF_CD_NELMTS  2
#define BSHUF_COMPRESS_LZ4 2

/* ROI bands. Chosen to request the same number of elements by each
 * geometry, so the two differ only in flat-order contiguity. */
#define ROI_ROW_LO 200
#define ROI_ROW_HI 264 /* 64 rows of every frame  */
#define ROI_COL_LO 400
#define ROI_COL_HI 528 /* 128 columns of every row */

#define ROI_ROW_ELEMS ((size_t)(ROI_ROW_HI - ROI_ROW_LO) * DET_W)

#define SUB_ARCHIVE   0
#define SUB_BSLZ4     1
#define SUB_ZSTD      2
#define SUB_DEFLATE   3
#define SUB_ZFP       4
#define SUB_ROI_ROWS  5
#define SUB_ROI_COLS  6
#define SUB_NARROWED  7
#define SUB_PHOTONS   8
#define SUB_SAME_PIPE 9
/* The cascade: the SAME consumer applying reduction-by-relevance and then
 * reduction-by-encoding, so the two axes can be shown to multiply rather
 * than merely coexist. Each step adds one narrowing to the one above it. */
#define SUB_ROI_T16   10
#define SUB_ROI_T16_Z 11
#define MAX_SUBS      12

#define SCENE_HIT   0
#define SCENE_BLANK 1
#define SCENE_DENSE 2
#define NSCENES     3

static const char *g_sub_name[MAX_SUBS] = {"archive",  "bslz4",    "zstd",     "deflate",  "zfp(lossy)",
                                            "roi_rows", "roi_cols", "narrowed", "photons",  "same_pipe",
                                            "roi+i16",  "roi+i16+zstd"};
static const char *g_scene_name[NSCENES] = {"HIT   (sparse Bragg peaks on a weak ring)",
                                             "BLANK (dark frame, no crystal in the beam)",
                                             "DENSE (speckle everywhere, XPCS-like)"};

/* Which registered filter each subscriber asks for, 0 for none. */
static const int g_sub_filter[MAX_SUBS] = {0,
                                           FILTER_BSHUF,
                                           FILTER_ZSTD,
                                           FILTER_DEFLATE,
                                           FILTER_ZFP,
                                           0,
                                           0,
                                           0,
                                           0,
                                           FILTER_BSHUF,
                                           0,
                                           FILTER_ZSTD};

static char g_fname[64];
static char g_group_file[80];
static char g_ready_sentinel[MAX_SUBS][80];
static char g_done_sentinel[MAX_SUBS][80];
static char g_writer_done_sentinel[80];
static char g_refilter_log[80];

typedef struct {
    long long commit_ns[NFRAMES];
    uint64_t  bytes[MAX_SUBS];  /* delivered (decoded) payload      */
    uint64_t  pushes[MAX_SUBS]; /* RPCs actually received           */
    uint64_t  elems[MAX_SUBS];  /* elements delivered               */
    uint64_t  wire[MAX_SUBS];   /* bytes on the wire, filtered subs */
    uint64_t  refilter_calls[MAX_SUBS];
    int       sub_errors;
    int       avail[MAX_SUBS]; /* 0 if this subscriber's filter is missing */
} shared_t;

static shared_t *g_shared;

static long long
now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static void
touch(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f);
}

static int
exists(const char *path)
{
    FILE *f = fopen(path, "r");

    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

static int
wait_for(const char *path, int max_polls)
{
    int i;

    for (i = 0; i < max_polls; i++) {
        if (exists(path))
            return 0;
        usleep(20000);
    }
    return -1;
}

static int
filter_ok(int id)
{
    return id == 0 || H5Zfilter_avail((H5Z_filter_t)id) > 0;
}

static void
set_names(int scene, int nsubs)
{
    int i;

    snprintf(g_fname, sizeof(g_fname), "b_det_%d_%d.h5", scene, nsubs);
    snprintf(g_group_file, sizeof(g_group_file), "%s.vsgroup", g_fname);
    snprintf(g_writer_done_sentinel, sizeof(g_writer_done_sentinel), "b_det_%d_%d.wdone", scene, nsubs);
    snprintf(g_refilter_log, sizeof(g_refilter_log), "b_det_%d_%d.refilter", scene, nsubs);
    for (i = 0; i < MAX_SUBS; i++) {
        snprintf(g_ready_sentinel[i], sizeof(g_ready_sentinel[i]), "b_det_%d_%d.ready.%d", scene, nsubs, i);
        snprintf(g_done_sentinel[i], sizeof(g_done_sentinel[i]), "b_det_%d_%d.done.%d", scene, nsubs, i);
    }
}

static void
clean_names(void)
{
    int i;

    unlink(g_fname);
    unlink(g_group_file);
    unlink(g_writer_done_sentinel);
    unlink(g_refilter_log);
    for (i = 0; i < MAX_SUBS; i++) {
        unlink(g_ready_sentinel[i]);
        unlink(g_done_sentinel[i]);
    }
}

/* Deterministic, so every process can recompute the scene and verify what
 * it received without the scene being shipped between them.
 *
 * Not an attempt at a physically faithful diffraction simulation -- it is a
 * stand-in with the right STATISTICS for compression: photon-counting data
 * is integer, non-negative, overwhelmingly zero away from the signal, and
 * has its information concentrated in a few high-count pixels. That is what
 * bitshuffle and the entropy coders actually respond to. See the report's
 * threats to validity: real detector frames carry structure this does not
 * reproduce, and the absolute ratios below are therefore indicative only. */
/* A 32-bit mixer with good avalanche, so the scenes below carry real
 * entropy. This matters more than it looks: an earlier version generated
 * counts with (i * K + frame) % 7, which is periodic in i with period 7,
 * and every entropy coder found that period instantly -- zstd reported
 * 0.011% on what was supposed to be the INCOMPRESSIBLE scene. A compression
 * benchmark whose synthetic data is accidentally periodic measures its own
 * generator, not the codecs. */
static uint32_t
mix32(uint32_t x)
{
    x += 0x9e3779b9u;
    x = (x ^ (x >> 16)) * 0x21f0aaadu;
    x = (x ^ (x >> 15)) * 0x735a2d97u;
    return x ^ (x >> 15);
}

/* A small Poisson-ish draw: sum of Bernoulli trials from independent bits of
 * one mixed word. Cheap, deterministic, and it produces the low-count,
 * right-skewed integer distribution photon-counting detectors actually
 * deliver -- which is what bitshuffle and the entropy coders respond to. */
static int32_t
counts(uint32_t seed, int trials, int one_in)
{
    uint32_t h = mix32(seed);
    int      k = 0, t;

    for (t = 0; t < trials; t++) {
        if ((mix32(h + (uint32_t)t) % (uint32_t)one_in) == 0)
            k++;
    }
    return (int32_t)k;
}

/* Deterministic, so every process can recompute the scene and verify what it
 * received without the scene being shipped between them.
 *
 * Not a physically faithful diffraction simulation -- a stand-in with the
 * right STATISTICS for compression: integer, non-negative, overwhelmingly
 * zero away from the signal in the sparse scenes, information concentrated
 * in a few high-count pixels, and genuine entropy in the dense one. See the
 * report's threats to validity: real detector frames carry spatial structure
 * this does not reproduce, so the absolute ratios are indicative only. What
 * IS transferable is the spread between scenes. */
static void
fill_frame(int32_t *buf, int frame, int scene)
{
    size_t i;
    int    p, r, c;

    memset(buf, 0, FRAME_ELEMS * sizeof(int32_t));

    if (scene == SCENE_BLANK) {
        /* A real dark frame is not literally empty: a handful of hot pixels
         * plus very occasional stray counts. Still overwhelmingly zero, and
         * that is the point -- this is the frame a facility would rather
         * veto than store. */
        for (p = 0; p < 32; p++)
            buf[((size_t)p * 16411u + (size_t)frame) % FRAME_ELEMS] = 1 + (int32_t)(mix32((uint32_t)p) % 4u);
        for (i = 0; i < FRAME_ELEMS; i += 4096)
            if (mix32((uint32_t)(i + (size_t)frame * 7919u)) % 3u == 0)
                buf[i] = 1;
        return;
    }

    if (scene == SCENE_DENSE) {
        /* Speckle: a genuinely random low count at every pixel. No long zero
         * runs, no periodicity, low dynamic range -- the case where a codec
         * has to do real work and cheap wins are unavailable. */
        for (i = 0; i < FRAME_ELEMS; i++)
            buf[i] = counts((uint32_t)(i * 2654435761u) ^ ((uint32_t)frame << 20), 8, 2);
        return;
    }

    /* SCENE_HIT: a weak powder ring plus a scatter of Bragg peaks, on an
     * otherwise empty detector. */
    for (r = 0; r < DET_H; r++) {
        for (c = 0; c < DET_W; c++) {
            int    dr   = r - DET_H / 2;
            int    dc   = c - DET_W / 2;
            int    rad2 = dr * dr + dc * dc;
            size_t idx  = (size_t)r * DET_W + (size_t)c;

            if (rad2 > 40000 && rad2 < 42000)
                buf[idx] = counts((uint32_t)idx ^ ((uint32_t)frame << 19), 4, 3);
        }
    }
    for (p = 0; p < 24; p++) {
        int pr = 40 + (int)(mix32((uint32_t)(p * 3 + frame)) % (uint32_t)(DET_H - 80));
        int pc = 40 + (int)(mix32((uint32_t)(p * 5 + frame * 31)) % (uint32_t)(DET_W - 80));

        for (r = -1; r <= 1; r++)
            for (c = -1; c <= 1; c++) {
                size_t idx = (size_t)(pr + r) * DET_W + (size_t)(pc + c);

                buf[idx] = (r == 0 && c == 0)
                               ? (int32_t)(600 + mix32((uint32_t)(p + frame * 97)) % 900u)
                               : (int32_t)(40 + mix32((uint32_t)(p * 13 + r * 3 + c + frame)) % 160u);
            }
    }
}

/* Build the DCPL a subscriber asks for, or H5I_INVALID_HID for none. The
 * chunk is one whole pushed run; the connector overrides it anyway (see
 * H5VL__stream_refilter_for_subscriber()), but a DCPL must be chunked
 * before a filter can be set on it. */
static hid_t
make_pipeline_dcpl(int filter_id, hsize_t nelem)
{
    hid_t dcpl;

    if (filter_id == 0)
        return H5I_INVALID_HID;
    if ((dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0)
        return H5I_INVALID_HID;
    if (H5Pset_chunk(dcpl, 1, &nelem) < 0) {
        H5Pclose(dcpl);
        return H5I_INVALID_HID;
    }

    switch (filter_id) {
        case FILTER_DEFLATE:
            if (H5Pset_deflate(dcpl, 6) < 0)
                goto fail;
            break;
        case FILTER_BSHUF: {
            /* bslz4 exactly as EIGER writes it: bitshuffle with LZ4 behind. */
            unsigned cd[BSHUF_CD_NELMTS] = {0, BSHUF_COMPRESS_LZ4};

            if (H5Pset_filter(dcpl, (H5Z_filter_t)FILTER_BSHUF, H5Z_FLAG_MANDATORY, BSHUF_CD_NELMTS, cd) < 0)
                goto fail;
            break;
        }
        case FILTER_ZSTD: {
            unsigned cd[1] = {3}; /* level */

            if (H5Pset_filter(dcpl, (H5Z_filter_t)FILTER_ZSTD, H5Z_FLAG_MANDATORY, 1, cd) < 0)
                goto fail;
            break;
        }
        case FILTER_ZFP: {
            /* ACCURACY mode: an absolute error tolerance, cd[0]=3 with the
             * tolerance as a double occupying cd[2..3]. H5Z-ZFP's generic
             * cd_values layout, quoted from its own H5Zzfp_plugin.h rather
             * than guessed -- two wrong guesses cost real time here:
             *
             *  - Mode 3 with the tolerance written as a plain integer asks
             *    for a denormal tolerance near 8e-323, and zfp then works
             *    essentially forever trying to meet it (a hung run).
             *  - Mode 2 (fixed PRECISION) is the intuitive choice and is
             *    wrong for this data: zfp treats an int32 as fixed-point
             *    spanning the whole 32-bit range, so 16 bits of precision
             *    quantizes photon counts of a few hundred to exactly ZERO.
             *    Verified against plain HDF5 with no vol-stream in the
             *    picture, which is what established it was the parameters
             *    and not the connector.
             *
             * Tolerance 1.0 count is scientifically defensible for
             * photon-counting data, whose own shot noise is sqrt(N) >= 1
             * wherever N >= 1: the compression error is below the
             * measurement error. LOSSY -- values come back close, not
             * equal, which is why this subscriber gets a tolerance check
             * rather than an equality one. */
            unsigned cd[4];
            double   tol = 1.0;

            cd[0] = 3; /* H5Z_ZFP_MODE_ACCURACY */
            cd[1] = 0;
            memcpy(&cd[2], &tol, sizeof(double));

            if (H5Pset_filter(dcpl, (H5Z_filter_t)FILTER_ZFP, H5Z_FLAG_MANDATORY, 4, cd) < 0)
                goto fail;
            break;
        }
        default:
            goto fail;
    }
    return dcpl;

fail:
    H5Pclose(dcpl);
    return H5I_INVALID_HID;
}

static int
run_writer(int scene, int nsubs)
{
    hid_t    vol_id, fapl, fid, space, dcpl, ds = -1;
    int32_t *frame;
    int      s, i;
    hsize_t  dims[3]    = {1, DET_H, DET_W};
    hsize_t  maxdims[3] = {H5S_UNLIMITED, DET_H, DET_W};
    hsize_t  chunk[3]   = {1, DET_H, DET_W};

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("writer: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("writer: FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(g_fname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("writer: FAIL create (transport up? VOL_STREAM_NA set?)\n");
        return 1;
    }

    /* The only place a filter's true wire size is visible; see this file's
     * header. Redirected after H5Fcreate() so a transport failure above
     * still reports to the terminal. */
    freopen(g_refilter_log, "w", stderr);

    for (i = 0; i < nsubs; i++)
        if (wait_for(g_ready_sentinel[i], 900) < 0)
            printf("writer: WARNING subscriber %d never signaled ready\n", i);

    if (NULL == (frame = (int32_t *)malloc(FRAME_ELEMS * sizeof(int32_t)))) {
        printf("writer: FAIL alloc\n");
        return 1;
    }

    if ((space = H5Screate_simple(3, dims, maxdims)) < 0 || (dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
        H5Pset_chunk(dcpl, 3, chunk) < 0) {
        printf("writer: FAIL space/dcpl\n");
        free(frame);
        return 1;
    }
    /* Acquisition-time compression, as a real beamline writes it. This is
     * also what makes same_pipe a fair test of the zero-copy fast path. */
    if (filter_ok(FILTER_BSHUF)) {
        unsigned cd[BSHUF_CD_NELMTS] = {0, BSHUF_COMPRESS_LZ4};

        if (H5Pset_filter(dcpl, (H5Z_filter_t)FILTER_BSHUF, H5Z_FLAG_MANDATORY, BSHUF_CD_NELMTS, cd) < 0)
            printf("writer: WARNING could not set bslz4 on the acquisition DCPL\n");
    }

    for (s = 0; s < NFRAMES; s++) {
        hsize_t   newdims[3] = {(hsize_t)(s + 1), DET_H, DET_W};
        hsize_t   start[3]   = {(hsize_t)s, 0, 0};
        hsize_t   count[3]   = {1, DET_H, DET_W};
        hid_t     fspace, mspace;
        long long t0;

        fill_frame(frame, s, scene);

        t0 = now_ns();

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            printf("writer: FAIL begin step %d\n", s);
            free(frame);
            return 1;
        }
        if (s == 0) {
            if ((ds = H5Dcreate2(fid, "/entry/data/data", H5T_NATIVE_INT32, space, H5P_DEFAULT, dcpl,
                                 H5P_DEFAULT)) < 0) {
                printf("writer: FAIL create dataset\n");
                free(frame);
                return 1;
            }
        }
        else if (H5Dset_extent(ds, newdims) < 0) {
            printf("writer: FAIL set_extent frame %d\n", s);
            free(frame);
            return 1;
        }

        if ((fspace = H5Dget_space(ds)) < 0 ||
            H5Sselect_hyperslab(fspace, H5S_SELECT_SET, start, NULL, count, NULL) < 0 ||
            (mspace = H5Screate_simple(3, count, NULL)) < 0) {
            printf("writer: FAIL spaces frame %d\n", s);
            free(frame);
            return 1;
        }
        if (H5Dwrite(ds, H5T_NATIVE_INT32, mspace, fspace, H5P_DEFAULT, frame) < 0 || H5Fend_step(fid) < 0) {
            printf("writer: FAIL write/end frame %d\n", s);
            free(frame);
            return 1;
        }
        H5Sclose(mspace);
        H5Sclose(fspace);

        g_shared->commit_ns[s] = now_ns() - t0;
    }

    touch(g_writer_done_sentinel);

    H5Dclose(ds);
    H5Sclose(space);
    H5Pclose(dcpl);
    free(frame);

    for (i = 0; i < nsubs; i++)
        if (wait_for(g_done_sentinel[i], 2500) < 0)
            printf("writer: WARNING subscriber %d never signaled done\n", i);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}

static int
subscribe_as(hid_t fid, int idx, const char *path)
{
    hid_t       space, dcpl = H5I_INVALID_HID;
    hsize_t     dims[3] = {NFRAMES, DET_H, DET_W};
    const char *paths[1];
    hid_t       spaces[1], plists[1];
    int         rc = 0;

    paths[0] = path;

    if ((space = H5Screate_simple(3, dims, NULL)) < 0)
        return 1;

    if (idx == SUB_ROI_ROWS || idx == SUB_ROI_T16 || idx == SUB_ROI_T16_Z) {
        hsize_t start[3] = {0, ROI_ROW_LO, 0};
        hsize_t count[3] = {NFRAMES, ROI_ROW_HI - ROI_ROW_LO, DET_W};

        if (H5Sselect_hyperslab(space, H5S_SELECT_SET, start, NULL, count, NULL) < 0) {
            H5Sclose(space);
            return 1;
        }
    }
    else if (idx == SUB_ROI_COLS) {
        hsize_t start[3] = {0, 0, ROI_COL_LO};
        hsize_t count[3] = {NFRAMES, DET_H, ROI_COL_HI - ROI_COL_LO};

        if (H5Sselect_hyperslab(space, H5S_SELECT_SET, start, NULL, count, NULL) < 0) {
            H5Sclose(space);
            return 1;
        }
    }

    if (g_sub_filter[idx] != 0)
        dcpl = make_pipeline_dcpl(g_sub_filter[idx], (hsize_t)FRAME_ELEMS);

    spaces[0] = space;
    plists[0] = (dcpl == H5I_INVALID_HID) ? H5P_DEFAULT : dcpl;

    if (H5Fsubscribe(fid, 1, paths, spaces, plists) < 0)
        rc = 1;

    H5Sclose(space);
    if (dcpl != H5I_INVALID_HID)
        H5Pclose(dcpl);
    if (rc)
        return rc;

    if ((idx == SUB_NARROWED || idx == SUB_ROI_T16 || idx == SUB_ROI_T16_Z) &&
        H5Fsubscribe_type(fid, path, H5T_NATIVE_SHORT) < 0)
        return 1;
    if (idx == SUB_PHOTONS) {
        int32_t zero = 0;

        if (H5Fsubscribe_predicate(fid, path, H5VL_STREAM_PRED_GT, H5T_NATIVE_INT32, &zero) < 0)
            return 1;
    }
    return 0;
}

/* Signal both sentinels and return. A subscriber that fails must still say
 * so through the filesystem: the writer waits on these, and a silent exit
 * turns one subscriber's error into a multi-second stall per scene (and,
 * with enough of them, into what looks like a hang rather than a failure). */
static int
sub_bail(int idx, const char *what)
{
    printf("%s: FAIL %s\n", g_sub_name[idx], what);
    touch(g_ready_sentinel[idx]);
    touch(g_done_sentinel[idx]);
    return 1;
}

static int
run_subscriber(int idx, int scene)
{
    const char *path = "/entry/data/data";
    hid_t       vol_id, fapl, fid;
    int         rc = 0, drained = 0;
    uint64_t    bytes = 0, pushes = 0, elems = 0;
    size_t      elem_size = (idx == SUB_NARROWED || idx == SUB_ROI_T16 || idx == SUB_ROI_T16_Z)
                                ? sizeof(short)
                                : sizeof(int32_t);
    int32_t    *ref       = NULL;

    /* A subscriber whose filter this HDF5 does not have reports itself
     * unavailable and exits cleanly, so the benchmark still runs and still
     * says what was and was not measured. */
    if (!filter_ok(g_sub_filter[idx])) {
        g_shared->avail[idx] = 0;
        touch(g_ready_sentinel[idx]);
        touch(g_done_sentinel[idx]);
        return 0;
    }
    g_shared->avail[idx] = 1;

    if ((vol_id = H5VL_stream_register()) < 0)
        return sub_bail(idx, "register");
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0)
        return sub_bail(idx, "fapl");
    if (wait_for(g_group_file, 900) < 0)
        return sub_bail(idx, "writer's group sidecar never appeared");
    if ((fid = H5Fopen(g_fname, H5F_ACC_RDONLY, fapl)) < 0)
        return sub_bail(idx, "open");
    if (subscribe_as(fid, idx, path) != 0)
        return sub_bail(idx, "subscribe");
    if (NULL == (ref = (int32_t *)malloc(FRAME_ELEMS * sizeof(int32_t))))
        return sub_bail(idx, "alloc");

    touch(g_ready_sentinel[idx]);

    /* The drain expects to time out at the end of the stream, which HDF5
     * would otherwise print an error stack for on every poll. Silenced only
     * after the subscription is established, so a genuine setup failure
     * above still reports. */
    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

    for (;;) {
        uint64_t phys = (uint64_t)-1, elem_start = 0, elem_count = 0;
        char    *rpath = NULL;
        void    *buf   = NULL;
        size_t   size  = 0;

        if (H5Fget_subscribed_data(fid, 1500, &phys, &rpath, &buf, &size, &elem_start, &elem_count) < 0) {
            if (exists(g_writer_done_sentinel)) {
                if (drained)
                    break;
                drained = 1;
                continue;
            }
            continue;
        }
        drained = 0;

        bytes += (uint64_t)size;
        pushes++;
        elems += (uint64_t)(size / elem_size);

        /* Verify against the scene, for every lossless subscriber that is
         * delivered the object's own type. zfp is lossy and narrowed
         * changes the type, so both are excluded by construction rather
         * than by a loosened tolerance. */
        /* zfp is lossy, so it gets a tolerance check rather than no check
         * at all: a filter that silently delivered zeros would otherwise be
         * indistinguishable here from one that compressed superbly. The
         * request is an ABSOLUTE tolerance of 1 count; zfp bounds the
         * transform rather than each reconstructed value, so a little
         * overshoot is normal and measured at 2 counts on this data. The
         * gate is set at 4 -- loose enough not to be brittle, tight enough
         * that a filter returning zeros or garbage fails it. */
        if (idx == SUB_ZFP) {
            const int32_t *got = (const int32_t *)buf;
            size_t         n = size / sizeof(int32_t), i;
            int            cur_frame = -1;

            for (i = 0; i < n; i++) {
                uint64_t flat = elem_start + i;
                int      f    = (int)(flat / FRAME_ELEMS);
                size_t   off  = (size_t)(flat % FRAME_ELEMS);
                int32_t  ref_v, err;

                if (f < 0 || f >= NFRAMES)
                    break;
                if (f != cur_frame) {
                    fill_frame(ref, f, scene);
                    cur_frame = f;
                }
                ref_v = ref[off];
                err   = got[i] > ref_v ? got[i] - ref_v : ref_v - got[i];
                if (err > 4) {
                    printf("%s: FAIL lossy value outside tolerance at frame %d offset %zu: "
                           "got %d expected %d (|err| %d > 4)\n",
                           g_sub_name[idx], f, off, got[i], ref_v, err);
                    rc = 1;
                    break;
                }
            }
        }
        else if (elem_size == sizeof(short)) {
            const short *got = (const short *)buf;
            size_t       n = size / sizeof(short), i;
            int          cur_frame = -1;

            for (i = 0; i < n; i++) {
                uint64_t flat = elem_start + i;
                int      f    = (int)(flat / FRAME_ELEMS);
                size_t   off  = (size_t)(flat % FRAME_ELEMS);

                if (f < 0 || f >= NFRAMES) {
                    printf("%s: FAIL element outside the stream: flat %llu\n", g_sub_name[idx],
                           (unsigned long long)flat);
                    rc = 1;
                    break;
                }
                if (f != cur_frame) {
                    fill_frame(ref, f, scene);
                    cur_frame = f;
                }
                /* Every count in these scenes fits an int16, so the narrowed
                 * delivery must still be exact -- a type narrowing is not a
                 * licence to lose values. */
                if (got[i] != (short)ref[off]) {
                    printf("%s: FAIL wrong int16 value at frame %d offset %zu: got %d expected %d\n",
                           g_sub_name[idx], f, off, (int)got[i], (int)ref[off]);
                    rc = 1;
                    break;
                }
            }
        }
        else {
            const int32_t *got = (const int32_t *)buf;
            size_t         n = size / sizeof(int32_t), i;
            int            cur_frame = -1;

            for (i = 0; i < n; i++) {
                uint64_t flat  = elem_start + i;
                int      f     = (int)(flat / FRAME_ELEMS);
                size_t   off   = (size_t)(flat % FRAME_ELEMS);

                if (f != cur_frame) {
                    if (f < 0 || f >= NFRAMES) {
                        printf("%s: FAIL element outside the stream: flat %llu\n", g_sub_name[idx],
                               (unsigned long long)flat);
                        rc = 1;
                        break;
                    }
                    fill_frame(ref, f, scene);
                    cur_frame = f;
                }
                if (got[i] != ref[off]) {
                    printf("%s: FAIL wrong value at frame %d offset %zu: got %d expected %d\n",
                           g_sub_name[idx], f, off, got[i], ref[off]);
                    rc = 1;
                    break;
                }
                /* The predicate subscriber must additionally never be sent
                 * a zero -- that is the whole point of sparsification, and
                 * where the run cap turns the saving into an over-send it
                 * will show up here as a value that does not satisfy it. */
                if (idx == SUB_PHOTONS && got[i] <= 0)
                    g_shared->refilter_calls[SUB_PHOTONS]++; /* counted, not fatal */
            }
        }
        free(rpath);
        free(buf);
        if (rc)
            break;
    }

    free(ref);
    g_shared->bytes[idx]  = bytes;
    g_shared->pushes[idx] = pushes;
    g_shared->elems[idx]  = elems;
    if (rc)
        g_shared->sub_errors++;

    /* Signal done, then close. Tried the other order -- close first, so the
     * writer's ssg_group_destroy() could not run until every member had
     * actually left -- on the theory that the SIGSEGV below was our
     * ordering. It did not stop the crash and it risks the documented
     * reader-closes-while-writer-alive deadlock, so it was reverted. */
    touch(g_done_sentinel[idx]);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return rc;
}

/* Attribute the writer's refilter diagnostic to subscribers.
 *
 * The filter id alone is NOT enough. Two subscribers can request the same
 * pipeline over different amounts of data -- zstd over whole frames and
 * roi+i16+zstd over a 64-row band in int16 both log filter=32015 -- and
 * splitting those lines evenly between them reports both at the average of
 * two numbers that differ by more than an order of magnitude. (That is not
 * hypothetical: it happened here, and showed the cascade subscriber with
 * exactly the whole-frame subscriber's bytes.)
 *
 * The raw size distinguishes them, because it is elem_size * elements in the
 * pushed run and both of those differ. Lines are therefore matched on the
 * (filter id, raw bytes) pair. Subscribers that agree on BOTH -- bslz4 and
 * same_pipe, which request an identical pipeline over identical data by
 * design -- are genuinely indistinguishable and share the line equally,
 * which for them is exact. */
static uint64_t
expected_raw(int idx)
{
    size_t elems = (idx == SUB_ROI_ROWS || idx == SUB_ROI_T16 || idx == SUB_ROI_T16_Z)
                       ? ROI_ROW_ELEMS
                       : FRAME_ELEMS;
    size_t esize = (idx == SUB_NARROWED || idx == SUB_ROI_T16 || idx == SUB_ROI_T16_Z)
                       ? sizeof(short)
                       : sizeof(int32_t);

    return (uint64_t)elems * (uint64_t)esize;
}

static void
collect_wire_bytes(void)
{
    FILE *f = fopen(g_refilter_log, "r");
    char  line[256];

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        int                filter_id = 0;
        unsigned long long raw = 0, filt = 0;
        int                i, sharers = 0;

        if (3 != sscanf(line, "  refilter  filter=%d raw=%llu filtered=%llu", &filter_id, &raw, &filt))
            continue;

        for (i = 0; i < MAX_SUBS; i++)
            if (g_sub_filter[i] == filter_id && g_shared->avail[i] && expected_raw(i) == (uint64_t)raw)
                sharers++;
        if (sharers == 0)
            continue;
        for (i = 0; i < MAX_SUBS; i++)
            if (g_sub_filter[i] == filter_id && g_shared->avail[i] && expected_raw(i) == (uint64_t)raw) {
                g_shared->wire[i] += (uint64_t)filt / (uint64_t)sharers;
                g_shared->refilter_calls[i]++;
            }
    }
    fclose(f);
}

static double
run_one(int scene, int nsubs)
{
    pid_t subs[MAX_SUBS];
    pid_t writer_pid;
    int   i, status, failed = 0;

    set_names(scene, nsubs);
    clean_names();
    memset(g_shared, 0, sizeof(*g_shared));

    fflush(NULL);

    for (i = 0; i < nsubs; i++) {
        if ((subs[i] = fork()) < 0) {
            perror("fork");
            return -1;
        }
        if (subs[i] == 0) {
            int rc = run_subscriber(i, scene);

            fflush(NULL);
            _exit(rc);
        }
    }
    if ((writer_pid = fork()) < 0) {
        perror("fork");
        return -1;
    }
    if (writer_pid == 0) {
        int rc = run_writer(scene, nsubs);

        fflush(NULL);
        _exit(rc);
    }

    if (waitpid(writer_pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("  (writer exited %d)\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        failed = 1;
    }
    for (i = 0; i < nsubs; i++)
        if (waitpid(subs[i], &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            if (WIFSIGNALED(status))
                printf("  (subscriber %s killed by signal %d)\n", g_sub_name[i], WTERMSIG(status));
            else
                printf("  (subscriber %s exited %d)\n", g_sub_name[i],
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            failed = 1;
        }

    collect_wire_bytes();
    clean_names();

    if (failed || g_shared->sub_errors)
        return -1;

    {
        long long sum = 0;
        int       s;

        for (s = 0; s < NFRAMES; s++)
            sum += g_shared->commit_ns[s];
        return (double)sum / NFRAMES / 1e6;
    }
}

static void
report_scene(const char *label, double commit_ms, const shared_t *r)
{
    uint64_t base = r->bytes[SUB_ARCHIVE] ? r->bytes[SUB_ARCHIVE] : 1;
    int      i;

    printf("\n%s -- mean writer commit %.3f ms/frame\n", label, commit_ms);
    printf("  %-11s %13s %9s %13s %9s %8s\n", "subscriber", "delivered", "% full", "on the wire", "% full",
           "pushes");
    for (i = 0; i < MAX_SUBS; i++) {
        uint64_t wire = (g_sub_filter[i] != 0) ? r->wire[i] : r->bytes[i];

        if (!r->avail[i]) {
            printf("  %-11s %13s (filter %d unavailable -- not measured)\n", g_sub_name[i], "-",
                   g_sub_filter[i]);
            continue;
        }
        printf("  %-11s %13llu %8.3f%% %13llu %8.3f%% %8llu\n", g_sub_name[i],
               (unsigned long long)r->bytes[i], 100.0 * (double)r->bytes[i] / (double)base,
               (unsigned long long)wire, 100.0 * (double)wire / (double)base,
               (unsigned long long)r->pushes[i]);
    }
}

int
main(void)
{
    shared_t res[NSCENES];
    double   commit[NSCENES], commit_alone;
    int      sc, i, nerrors = 0;

    setenv("VOL_STREAM_NA", "ofi+tcp", 0);
    setenv("VOL_STREAM_DEBUG_REFILTER", "1", 1);

    printf("vol-stream: PER-SUBSCRIBER COMPRESSION on a live detector stream (%s)\n",
           getenv("VOL_STREAM_NA"));
    printf("  /entry/data/data, %d frames of %d x %d int32 (EIGER2 module geometry)\n", NFRAMES, DET_H,
           DET_W);
    printf("  %.2f MiB per frame, %.1f MiB total; one chunk per frame, unlimited leading dimension\n",
           (double)(FRAME_ELEMS * sizeof(int32_t)) / 1048576.0,
           (double)(FRAME_ELEMS * sizeof(int32_t) * NFRAMES) / 1048576.0);
    printf("  filters: deflate %s, bslz4 %s, zstd %s, zfp %s\n", filter_ok(FILTER_DEFLATE) ? "yes" : "NO",
           filter_ok(FILTER_BSHUF) ? "yes" : "NO", filter_ok(FILTER_ZSTD) ? "yes" : "NO",
           filter_ok(FILTER_ZFP) ? "yes" : "NO");
    printf("  acquisition DCPL carries bslz4, as a beamline writes it\n");

    g_shared = (shared_t *)mmap(NULL, sizeof(shared_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                                 -1, 0);
    if (g_shared == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    for (sc = 0; sc < NSCENES; sc++) {
        if (sc > 0)
            usleep(1500000); /* let the previous SSG group dissolve */
        commit[sc] = run_one(sc, MAX_SUBS);
        if (commit[sc] < 0) {
            printf("scene %d FAILED\n", sc);
            nerrors++;
        }
        memcpy(&res[sc], g_shared, sizeof(shared_t));
    }

    usleep(1500000);
    commit_alone = run_one(SCENE_HIT, 1);
    if (commit_alone < 0) {
        printf("archive-only run FAILED\n");
        nerrors++;
    }

    if (!nerrors) {
        for (sc = 0; sc < NSCENES; sc++)
            report_scene(g_scene_name[sc], commit[sc], &res[sc]);

        printf("\nCompression ratio is a property of the SCENE, not of the codec alone "
               "(wire bytes, %% of raw):\n");
        printf("  %-11s %10s %10s %10s\n", "subscriber", "hit", "blank", "dense");
        for (i = 0; i < MAX_SUBS; i++) {
            if (!res[SCENE_HIT].avail[i])
                continue;
            printf("  %-11s", g_sub_name[i]);
            for (sc = 0; sc < NSCENES; sc++) {
                uint64_t wire = (g_sub_filter[i] != 0) ? res[sc].wire[i] : res[sc].bytes[i];
                uint64_t base = res[sc].bytes[SUB_ARCHIVE] ? res[sc].bytes[SUB_ARCHIVE] : 1;

                printf(" %9.3f%%", 100.0 * (double)wire / (double)base);
            }
            printf("\n");
        }
        printf("  NOTE bslz4 and same_pipe request the IDENTICAL pipeline, so the writer's diagnostic\n"
               "  cannot separate them; both columns carry their summed total. That they BOTH appear in\n"
               "  that log at all is the finding: the zero-copy fast path did not fire for either.\n");

        printf("\nWriter-side cost of serving %d narrowed subscribers vs. one plain one:\n", MAX_SUBS);
        printf("  archive alone            %.3f ms/frame\n", commit_alone);
        for (sc = 0; sc < NSCENES; sc++)
            printf("  %-24s %.3f ms/frame  (%.2fx)\n", g_scene_name[sc], commit[sc],
                   commit[sc] / commit_alone);
    }

    munmap(g_shared, sizeof(shared_t));

    if (nerrors) {
        printf("\n%d failure(s) -- benchmark did not complete cleanly\n", nerrors);
        return 1;
    }
    return 0;
}
