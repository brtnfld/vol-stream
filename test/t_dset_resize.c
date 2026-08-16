/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * H5Dset_extent() on a LIVE dataset (materialized by an earlier step),
 * called from inside a later step -- the ADIOS2-style "growing time-series
 * array" pattern, appending to an unlimited-dimension dataset once per
 * step, through the same handle the whole run.
 *
 * What it used to do, found by actually running it: silently corrupt an
 * EARLIER, already-committed step. H5VL__stream_replay_manifest() rewires
 * a placeholder's under_object to its own real /step/<n>/ copy only once,
 * the step that first materializes it -- every later step captures its own
 * separate synthesized copy instead of touching that one again. But
 * H5Dset_extent had no capture path of its own: it was an unconditional
 * passthrough straight to under_object, so calling it on the still-open
 * handle in step 1 reached through to step 0's own committed real object
 * and resized it in place, then again in step 2, then step 3. Measured
 * directly: /step/0/grow ended up holding the FINAL extent (8), not the
 * extent (2) it had when step 0 committed.
 *
 * First fixed by declining outright (a real, working resize was a bigger
 * change than the moment called for). Now built for real: H5VL_stream_
 * dataset_specific()'s SET_EXTENT handling defers the resize into this
 * handle's own pending_resize_space rather than touching the real object,
 * exactly like a write to this same live object is already deferred
 * (H5VL_stream_dataset_write()). H5VL_stream_dataset_get()'s GET_SPACE
 * override makes H5Dget_space() reflect the new shape immediately, matching
 * plain HDF5's contract, and H5VL__stream_step_create_index() uses the
 * override -- not the real, never-actually-resized object -- when a write
 * synthesizes this step's own copy. Case 1 below is the exit gate for this:
 * the previously-declined pattern now round-trips correctly, with the
 * earlier step still exactly as it was.
 *
 * Case 3 found a second gap while building the above, and closes it too:
 * each step's own synthesized copy is created fresh and populated only
 * from THIS step's own captured write, so a resize followed by a PARTIAL
 * write (only the new tail -- the leaner, ADIOS2-idiomatic append pattern;
 * benchmark/adios2_compare/adios2_bench.cpp does not actually use it
 * itself, it writes a full rewrite each step too, but this is the shape
 * of variable ADIOS2's own SetShape()/SetSelection() is meant for) used
 * to leave the carried-forward portion at HDF5's own fill value rather
 * than the previous step's real values. Not corruption (a different,
 * earlier step's data was never touched, and a well-defined, discoverable
 * fill value was not silently wrong data) -- but not a complete self-
 * sufficient snapshot either.
 *
 * Closed by H5VL__stream_carry_forward_resized(), which runs once per
 * synthesized create after the main replay loop finishes: it finds the
 * previous step's own committed copy for the same path (H5VL__stream_
 * path_index_resolve(), already populated by every writer-side replay for
 * reference resolution -- no new bookkeeping needed) and reads its values
 * forward into the region this step's own write does not cover. Gated by
 * a zero-waste check (this step's total written elements must equal
 * exactly the growth, new extent minus old) so a full rewrite (case 1,
 * already complete) never pays for a copy it does not need. Consequence
 * worth knowing, not just correctness: this is also what turns the
 * O(N^2) total-bytes-per-run cost a full rewrite pays (case 1, and
 * test/b_stream_grow.c's own benchmark) into O(N) for the tail-append
 * pattern -- measured directly in docs/dev-plan.md's benchmark section,
 * where the relative advantage grows with how long the stream runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME_GROW     "t_dset_resize_grow.h5"
#define FNAME_RECREATE "t_dset_resize_recreate.h5"
#define FNAME_PARTIAL  "t_dset_resize_partial.h5"
#define NSTEPS         4
#define CHUNK          2

static int nerrors = 0;

/* --------------------------------------------------------------------
 * Case 1: H5Dset_extent() + a full rewrite (0..n-1) every step, through
 * ONE live handle -- the pattern that used to be declined, now the exit
 * gate for real support. Checks the same two things the decline-era test
 * did (earlier step untouched, H5Dget_space right after set_extent) plus
 * the new positive case: every step's own values are correct.
 * -------------------------------------------------------------------- */
static void
case_resize_full_rewrite(hid_t vol_id)
{
    hid_t   fapl, fid, space, dcpl, ds, nfid;
    hsize_t dims = CHUNK, maxdims = H5S_UNLIMITED, chunk = CHUNK;
    int     s, i, *vals;

    printf("case 1: H5Dset_extent() + full rewrite through one live cross-step handle\n");

    unlink(FNAME_GROW);

    if (NULL == (vals = (int *)malloc((size_t)NSTEPS * CHUNK * sizeof(int)))) {
        printf("  FAIL  alloc\n");
        nerrors++;
        return;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0) {
        printf("  FAIL  fapl\n");
        nerrors++;
        return;
    }
    if ((fid = H5Fcreate(FNAME_GROW, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("  FAIL  create\n");
        nerrors++;
        return;
    }
    if ((space = H5Screate_simple(1, &dims, &maxdims)) < 0 || (dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
        H5Pset_chunk(dcpl, 1, &chunk) < 0) {
        printf("  FAIL  space/dcpl\n");
        nerrors++;
        return;
    }

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (ds = H5Dcreate2(fid, "/grow", H5T_NATIVE_INT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0) {
        printf("  FAIL  begin/create step 0\n");
        nerrors++;
        return;
    }
    for (i = 0; i < CHUNK; i++)
        vals[i] = i;
    if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
        printf("  FAIL  write/end step 0\n");
        nerrors++;
        return;
    }

    for (s = 1; s < NSTEPS; s++) {
        hsize_t newdims = (hsize_t)(s + 1) * CHUNK;
        hid_t   qspace;
        hsize_t qdims[1] = {0};

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            printf("  FAIL  begin_step %d\n", s);
            nerrors++;
            return;
        }
        if (H5Dset_extent(ds, &newdims) < 0) {
            printf("  FAIL  H5Dset_extent at step %d\n", s);
            nerrors++;
            H5Fend_step(fid);
            continue;
        }

        /* H5Dget_space() must reflect the resize immediately, matching
         * plain HDF5's own contract for H5Dset_extent -- the caller's next
         * move is almost always to build a selection against it. */
        if ((qspace = H5Dget_space(ds)) < 0 || H5Sget_simple_extent_dims(qspace, qdims, NULL) < 0 ||
            qdims[0] != newdims) {
            printf("  FAIL  H5Dget_space after set_extent at step %d\n", s);
            nerrors++;
        }
        if (qspace >= 0)
            H5Sclose(qspace);

        for (i = 0; i < (int)newdims; i++)
            vals[i] = i;
        if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
            printf("  FAIL  write/end step %d\n", s);
            nerrors++;
        }
    }

    H5Dclose(ds);
    H5Sclose(space);
    H5Pclose(dcpl);
    H5Fclose(fid);
    free(vals);

    if ((nfid = H5Fopen(FNAME_GROW, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  reopen natively\n");
        nerrors++;
        return;
    }
    for (s = 0; s < NSTEPS; s++) {
        char    path[64];
        hid_t   sds, ssp;
        hsize_t sdims[1] = {0};
        int     got[16], bad = 0;
        int     expect_n = (s + 1) * CHUNK;

        snprintf(path, sizeof(path), "/step/%d/grow", s);
        if ((sds = H5Dopen2(nfid, path, H5P_DEFAULT)) < 0) {
            printf("  FAIL  %s missing\n", path);
            nerrors++;
            continue;
        }
        ssp = H5Dget_space(sds);
        H5Sget_simple_extent_dims(ssp, sdims, NULL);
        H5Sclose(ssp);

        if ((int)sdims[0] != expect_n) {
            printf("  FAIL  %s extent = %d, expected %d\n", path, (int)sdims[0], expect_n);
            nerrors++;
        }
        else if (H5Dread(sds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) < 0) {
            printf("  FAIL  read %s\n", path);
            nerrors++;
        }
        else {
            for (i = 0; i < expect_n; i++)
                if (got[i] != i) {
                    bad = 1;
                    break;
                }
            if (bad) {
                printf("  FAIL  %s[%d] = %d, expected %d\n", path, i, got[i], i);
                nerrors++;
            }
            else
                printf("  ok    %s: extent %d, values 0..%d correct -- step 0 undisturbed by every "
                       "later resize\n",
                       path, expect_n, expect_n - 1);
        }
        H5Dclose(sds);
    }
    H5Fclose(nfid);
    H5Pclose(fapl);
} /* end case_resize_full_rewrite() */

/* --------------------------------------------------------------------
 * Case 2: the other working alternative -- re-create the dataset each
 * step at its new size -- still round-trips correctly. dev-plan.md
 * decision #2's model (successive versions of one named object, landing
 * group-based at /step/<n>/), applied to a shape that grows every step.
 * -------------------------------------------------------------------- */
static void
case_recreate_each_step(hid_t vol_id)
{
    hid_t fapl, fid, nfid;
    int   s, i;

    printf("case 2: re-creating the dataset each step at its new size\n");

    unlink(FNAME_RECREATE);

    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0) {
        printf("  FAIL  fapl\n");
        nerrors++;
        return;
    }
    if ((fid = H5Fcreate(FNAME_RECREATE, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("  FAIL  create\n");
        nerrors++;
        return;
    }

    for (s = 0; s < NSTEPS; s++) {
        hid_t   space, ds;
        hsize_t n = (hsize_t)(s + 1) * CHUNK;
        int    *vals;

        if ((vals = (int *)malloc(n * sizeof(int))) == NULL) {
            printf("  FAIL  alloc step %d\n", s);
            nerrors++;
            return;
        }
        for (i = 0; i < (int)n; i++)
            vals[i] = i;

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || (space = H5Screate_simple(1, &n, NULL)) < 0 ||
            (ds = H5Dcreate2(fid, "/grow", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) <
                0) {
            printf("  FAIL  begin/create step %d\n", s);
            nerrors++;
            free(vals);
            return;
        }
        if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
            printf("  FAIL  write/end step %d\n", s);
            nerrors++;
        }
        H5Dclose(ds);
        H5Sclose(space);
        free(vals);
    }

    H5Fclose(fid);
    H5Pclose(fapl);

    if ((nfid = H5Fopen(FNAME_RECREATE, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  reopen natively\n");
        nerrors++;
        return;
    }

    for (s = 0; s < NSTEPS; s++) {
        char    path[64];
        hid_t   sds, ssp;
        hsize_t sdims[1] = {0};
        int     got[16], bad = 0;
        int     expect_n = (s + 1) * CHUNK;

        snprintf(path, sizeof(path), "/step/%d/grow", s);
        if ((sds = H5Dopen2(nfid, path, H5P_DEFAULT)) < 0) {
            printf("  FAIL  %s missing\n", path);
            nerrors++;
            continue;
        }
        ssp = H5Dget_space(sds);
        H5Sget_simple_extent_dims(ssp, sdims, NULL);
        H5Sclose(ssp);

        if ((int)sdims[0] != expect_n) {
            printf("  FAIL  %s extent = %d, expected %d\n", path, (int)sdims[0], expect_n);
            nerrors++;
        }
        else if (H5Dread(sds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) < 0) {
            printf("  FAIL  read %s\n", path);
            nerrors++;
        }
        else {
            for (i = 0; i < expect_n; i++)
                if (got[i] != i) {
                    bad = 1;
                    break;
                }
            if (bad) {
                printf("  FAIL  %s[%d] = %d, expected %d\n", path, i, got[i], i);
                nerrors++;
            }
            else
                printf("  ok    %s: extent %d, values 0..%d correct\n", path, expect_n, expect_n - 1);
        }
        H5Dclose(sds);
    }

    H5Fclose(nfid);
} /* end case_recreate_each_step() */

/* --------------------------------------------------------------------
 * Case 3: H5Dset_extent() + writing only the NEW tail slice (not a full
 * rewrite) through one live handle -- the O(N) alternative to case 1's
 * O(N^2) full rewrite. Used to leave the carried-forward portion at
 * HDF5's own fill value (each step's synthesized copy was created fresh,
 * populated only from that step's own write); H5VL__stream_carry_
 * forward_resized() now fills it in from the previous step's own
 * committed copy, so this asserts the CLOSED state -- a complete,
 * correct snapshot at every step, not just this step's own tail. The
 * per-step print below still reports if the old gap ever reopens (a
 * regression would show the old "documented gap" message instead of the
 * "ALSO correct now" one, not a hard failure -- see its own comment).
 * -------------------------------------------------------------------- */
static void
case_resize_partial_write_gap(hid_t vol_id)
{
    hid_t   fapl, fid, space, dcpl, ds, nfid;
    hsize_t dims = CHUNK, maxdims = H5S_UNLIMITED, chunk = CHUNK;
    int     s;

    printf("case 3: H5Dset_extent() + a partial (tail-only) write -- the O(N) pattern\n");

    unlink(FNAME_PARTIAL);

    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0) {
        printf("  FAIL  fapl\n");
        nerrors++;
        return;
    }
    if ((fid = H5Fcreate(FNAME_PARTIAL, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("  FAIL  create\n");
        nerrors++;
        return;
    }
    if ((space = H5Screate_simple(1, &dims, &maxdims)) < 0 || (dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
        H5Pset_chunk(dcpl, 1, &chunk) < 0) {
        printf("  FAIL  space/dcpl\n");
        nerrors++;
        return;
    }
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (ds = H5Dcreate2(fid, "/grow", H5T_NATIVE_INT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0) {
        printf("  FAIL  begin/create step 0\n");
        nerrors++;
        return;
    }
    {
        int vals[CHUNK] = {0, 1};
        if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
            printf("  FAIL  write/end step 0\n");
            nerrors++;
            return;
        }
    }

    for (s = 1; s < NSTEPS; s++) {
        hsize_t newdims = (hsize_t)(s + 1) * CHUNK;
        hsize_t start = (hsize_t)s * CHUNK, count = CHUNK;
        int     vals[CHUNK];
        hid_t   fspace, mspace;
        int     i;

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || H5Dset_extent(ds, &newdims) < 0) {
            printf("  FAIL  begin/set_extent %d\n", s);
            nerrors++;
            return;
        }
        for (i = 0; i < CHUNK; i++)
            vals[i] = (int)start + i;

        if ((fspace = H5Dget_space(ds)) < 0 ||
            H5Sselect_hyperslab(fspace, H5S_SELECT_SET, &start, NULL, &count, NULL) < 0) {
            printf("  FAIL  hyperslab %d\n", s);
            nerrors++;
            return;
        }
        mspace = H5Screate_simple(1, &count, NULL);
        if (H5Dwrite(ds, H5T_NATIVE_INT, mspace, fspace, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
            printf("  FAIL  write/end step %d\n", s);
            nerrors++;
        }
        H5Sclose(mspace);
        H5Sclose(fspace);
    }

    H5Dclose(ds);
    H5Sclose(space);
    H5Pclose(dcpl);
    H5Fclose(fid);
    H5Pclose(fapl);

    if ((nfid = H5Fopen(FNAME_PARTIAL, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  reopen natively\n");
        nerrors++;
        return;
    }
    /* Step 0 (the only full write) must still be exactly correct. Steps
     * 1..N-1: the new tail is correct; the carried-forward head is the
     * documented fill-value gap, not a FAIL -- asserted explicitly so a
     * future fix (or regression) shows up as a real, deliberate change
     * here rather than a surprise. */
    for (s = 0; s < NSTEPS; s++) {
        char    path[64];
        hid_t   sds;
        int     got[16];
        int     expect_n = (s + 1) * CHUNK;
        int     tail_ok = 1, head_carried = 1;
        int     i;

        snprintf(path, sizeof(path), "/step/%d/grow", s);
        if ((sds = H5Dopen2(nfid, path, H5P_DEFAULT)) < 0) {
            printf("  FAIL  %s missing\n", path);
            nerrors++;
            continue;
        }
        if (H5Dread(sds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) < 0) {
            printf("  FAIL  read %s\n", path);
            nerrors++;
            H5Dclose(sds);
            continue;
        }
        H5Dclose(sds);

        for (i = s * CHUNK; i < expect_n; i++)
            if (got[i] != i)
                tail_ok = 0;
        if (!tail_ok) {
            printf("  FAIL  %s: this step's OWN tail write is wrong -- that would be a real regression\n",
                   path);
            nerrors++;
            continue;
        }

        if (s == 0) {
            printf("  ok    %s: the only full write, correct\n", path);
            continue;
        }

        for (i = 0; i < s * CHUNK; i++)
            if (got[i] != i)
                head_carried = 0;

        if (head_carried)
            printf("  ok    %s: carried-forward head correct -- H5VL__stream_carry_forward_resized() "
                   "filled it in from the previous step\n",
                   path);
        else {
            /* This gap was real (and tolerated) once; it is now closed and
             * shipped (H5VL__stream_carry_forward_resized()), so a
             * reappearance here is a genuine regression, not the
             * documented-boundary case this used to be. */
            printf("  FAIL  %s: carried-forward head at fill value -- the closed gap has reopened\n", path);
            nerrors++;
        }
    }
    H5Fclose(nfid);
} /* end case_resize_partial_write_gap() */

int
main(void)
{
    hid_t vol_id;

    printf("vol-stream: resizable datasets across steps\n");

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("FAIL register\n");
        return 1;
    }

    case_resize_full_rewrite(vol_id);
    case_recreate_each_step(vol_id);
    case_resize_partial_write_gap(vol_id);

    H5VLclose(vol_id);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
} /* end main() */
