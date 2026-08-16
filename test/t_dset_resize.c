/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * H5Dset_extent() on a LIVE dataset (materialized by an earlier step),
 * called from inside a later step -- the ADIOS2-style "growing time-series
 * array" pattern, appending to an unlimited-dimension dataset once per
 * step. Nothing in the suite exercised this until now; every existing
 * resizable-adjacent test writes a fixed-shape dataset.
 *
 * What it used to do, found by actually running it: silently corrupt an
 * EARLIER, already-committed step. H5VL__stream_replay_manifest() rewires
 * a placeholder's under_object to its own real /step/<n>/ copy only once,
 * the step that first materializes it (H5VL__stream_dset_capture()'s
 * comment) -- every later step captures its own separate synthesized copy
 * instead of touching that one again. But H5Dset_extent is not part of
 * that capture machinery: H5VL_stream_dataset_specific() was an
 * unconditional passthrough straight to under_object, so calling it on the
 * still-open handle in step 1 reached through to step 0's own committed
 * real object and resized it in place, then again in step 2, then step 3.
 * Measured directly: /step/0/grow ended up holding the FINAL extent (8),
 * not the extent (2) it had when step 0 committed -- retroactively
 * rewriting history that had already landed, the same class of bug
 * t_step_rewrite.c pins for plain writes, but reached through a dataset-
 * specific op instead of a write.
 *
 * Fixed by declining rather than silently corrupting: H5VL_stream_dataset_
 * specific() now refuses (clear HDF5 error, no data touched) whenever
 * H5VL__stream_dset_capture() says this object's write would be captured
 * -- i.e. a LIVE dataset materialized by an earlier step, mid a later open
 * step -- the same predicate the write path already uses, extended to
 * cover the dataset-specific ops that bypass it entirely. Both halves are
 * checked below: the earlier step's data must survive the attempt
 * untouched, and the documented working alternative -- re-creating the
 * dataset each step, which is what a shape that changes over time already
 * uses successfully elsewhere in this suite -- must still round-trip.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME_GROW    "t_dset_resize_grow.h5"
#define FNAME_RECREATE "t_dset_resize_recreate.h5"
#define NSTEPS 4

static int nerrors = 0;

/* --------------------------------------------------------------------
 * Case 1: H5Dset_extent() on a live handle across steps is refused, and
 * the step that already committed is left exactly as it was.
 * -------------------------------------------------------------------- */
static void
case_resize_declined(hid_t vol_id)
{
    hid_t   fapl, fid, space, dcpl, ds, nfid;
    hsize_t dims = 2, maxdims = H5S_UNLIMITED, chunk = 2;
    int     s;

    printf("case 1: H5Dset_extent() on a live cross-step handle\n");

    unlink(FNAME_GROW);

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

    /* Step 0: create with 2 elements, write {0, 1}. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (ds = H5Dcreate2(fid, "/grow", H5T_NATIVE_INT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0) {
        printf("  FAIL  begin/create step 0\n");
        nerrors++;
        return;
    }
    {
        int vals[2] = {0, 1};
        if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0) {
            printf("  FAIL  write/end step 0\n");
            nerrors++;
            return;
        }
    }

    /* Steps 1..NSTEPS-1: try to grow the same live handle. Must be refused,
     * every time, with no side effect on the file. */
    for (s = 1; s < NSTEPS; s++) {
        hsize_t newdims = (hsize_t)(s + 1) * 2;
        herr_t  rc;

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            printf("  FAIL  begin_step %d\n", s);
            nerrors++;
            return;
        }

        H5E_BEGIN_TRY
        {
            rc = H5Dset_extent(ds, &newdims);
        }
        H5E_END_TRY

        if (rc >= 0) {
            printf("  FAIL  H5Dset_extent on a live cross-step handle succeeded at step %d -- it should "
                   "have been refused, since it would silently mutate an earlier committed step\n",
                   s);
            nerrors++;
        }

        /* end_step still must succeed: refusing the resize is not supposed
         * to poison the rest of the step machinery. */
        if (H5Fend_step(fid) < 0) {
            printf("  FAIL  end_step %d after the declined resize\n", s);
            nerrors++;
        }
    }

    H5Dclose(ds);
    H5Sclose(space);
    H5Pclose(dcpl);
    H5Fclose(fid);

    /* The load-bearing check: step 0's own committed copy must be exactly
     * what it was when step 0 ended, untouched by every later refused
     * attempt. */
    if ((nfid = H5Fopen(FNAME_GROW, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  reopen natively\n");
        nerrors++;
    }
    else {
        hid_t   sds, ssp;
        hsize_t sdims[1] = {0};
        int     got[2];

        if ((sds = H5Dopen2(nfid, "/step/0/grow", H5P_DEFAULT)) < 0) {
            printf("  FAIL  /step/0/grow missing\n");
            nerrors++;
        }
        else {
            ssp = H5Dget_space(sds);
            H5Sget_simple_extent_dims(ssp, sdims, NULL);
            H5Sclose(ssp);

            if (sdims[0] != 2) {
                printf("  FAIL  /step/0/grow extent = %llu, expected 2 -- a declined resize still leaked "
                       "through\n",
                       (unsigned long long)sdims[0]);
                nerrors++;
            }
            else if (H5Dread(sds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) < 0 || got[0] != 0 ||
                     got[1] != 1) {
                printf("  FAIL  /step/0/grow values corrupted\n");
                nerrors++;
            }
            else
                printf("  ok    /step/0/grow stayed at extent 2, values {0,1}, through %d declined "
                       "resize attempts\n",
                       NSTEPS - 1);
            H5Dclose(sds);
        }
        H5Fclose(nfid);
    }

    H5Pclose(fapl);
} /* end case_resize_declined() */

/* --------------------------------------------------------------------
 * Case 2: the documented working alternative -- re-create the dataset
 * each step at its new size -- round-trips correctly. This is exactly
 * dev-plan.md decision #2's model (successive versions of one named
 * object, landing group-based at /step/<n>/), applied to a shape that
 * grows every step instead of staying fixed.
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
        hsize_t n = (hsize_t)(s + 1) * 2;
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
        int     expect_n = (s + 1) * 2;

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

int
main(void)
{
    hid_t vol_id;

    printf("vol-stream: resizable datasets across steps\n");

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("FAIL register\n");
        return 1;
    }

    case_resize_declined(vol_id);
    case_recreate_each_step(vol_id);

    H5VLclose(vol_id);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
} /* end main() */
