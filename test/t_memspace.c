/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Dataset writes whose MEMORY dataspace carries a selection.
 *
 * The capture path read mem_type_id and file_space_id but never
 * mem_space_id: it memcpy'd the leading npoints(file_selection) elements
 * straight out of the caller's buffer. That is what the caller asked for only
 * when the memory selection IS those leading elements -- which every existing
 * test happens to build (H5Screate_simple() sized to match, no selection
 * applied), and which the ordinary ghosted-array idiom is not. Writing the
 * interior of a haloed buffer captured the halo instead, with no error: an
 * 8-element dataset written from offset 4 of a 16-element buffer replayed as
 * elements 0..7.
 *
 * What makes it worse than an ordinary wrong answer is the direction:
 * bracketing the write in a step is what corrupted it, so the failure
 * inverted M1's own byte-identity promise. That is why every case here is
 * asserted the same way -- run the identical write twice, once unbracketed
 * (pure pass-through, the native behavior) and once inside a step, and
 * require the two to agree. A capture bug then shows up as a disagreement
 * with HDF5 itself rather than against a hand-computed expectation this test
 * could get wrong in the same way the connector did.
 *
 * Cases:
 *   1. 1-D hyperslab over the interior of a larger buffer -- the original
 *      failure, and the ordinary ghost-cell pattern.
 *   2. A strided 1-D memory selection (every other element), which is more
 *      than one flat run, so a single-run gather is not enough.
 *   3. A 2-D memory selection whose flat runs are genuinely discontiguous
 *      (a column), the shape that needs run-by-run gathering.
 *   4. H5S_ALL and an unselected simple space still round-trip, confirming
 *      the packed fast path is intact rather than accidentally disabled.
 *   5. A point selection is REFUSED, not silently mis-captured. Gathering it
 *      is describable but H5VL__stream_space_flat_runs() reports no runs for
 *      it, and the capture path's rule is to refuse rather than guess --
 *      under-capturing writes wrong values into a file where nothing
 *      downstream can detect it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define MEMLEN 64

static int nerrors = 0;

/* One write, performed either bracketed in a step or not, read back through
 * the *native* connector so the check never depends on the code under test.
 * Returns 0 and fills out[] on success, -1 if the write itself was refused. */
static int
run_case(const char *fname, int in_step, int rank, const hsize_t *dims, hid_t mspace, const int *mem,
         int n_elem, int *out)
{
    hid_t    fapl = -1, fid = -1, ds = -1, fspace = -1, vol;
    uint64_t lid = 1;
    int      ret = -1;

    if ((vol = H5VL_stream_register()) < 0)
        return -1;
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol, NULL) < 0)
        goto done;
    if ((fid = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0)
        goto done;

    if (in_step && H5Fbegin_step(fid, 1, &lid, 0) < 0)
        goto done;

    if ((fspace = H5Screate_simple(rank, dims, NULL)) < 0)
        goto done;
    if ((ds = H5Dcreate2(fid, "/data", H5T_NATIVE_INT, fspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0)
        goto done;

    /* The write under test. A refusal is a legitimate outcome for a selection
     * the capture path cannot describe (case 5), so it is reported rather
     * than treated as a test failure here. */
    if (H5Dwrite(ds, H5T_NATIVE_INT, mspace, H5S_ALL, H5P_DEFAULT, mem) < 0) {
        H5Dclose(ds);
        ds = -1;
        if (in_step)
            H5Fend_step(fid);
        ret = -2; /* refused */
        goto done;
    }

    H5Dclose(ds);
    ds = -1;
    if (in_step && H5Fend_step(fid) < 0)
        goto done;
    H5Fclose(fid);
    fid = -1;

    /* Read back natively: the connector under test is not in this path. */
    {
        hid_t nfapl = H5Pcreate(H5P_FILE_ACCESS);
        hid_t rfid, rds;

        if (nfapl < 0)
            goto done;
        if ((rfid = H5Fopen(fname, H5F_ACC_RDONLY, nfapl)) < 0) {
            H5Pclose(nfapl);
            goto done;
        }
        if ((rds = H5Dopen2(rfid, in_step ? "/step/0/data" : "/data", H5P_DEFAULT)) < 0) {
            H5Fclose(rfid);
            H5Pclose(nfapl);
            goto done;
        }
        if (H5Dread(rds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, out) < 0) {
            H5Dclose(rds);
            H5Fclose(rfid);
            H5Pclose(nfapl);
            goto done;
        }
        H5Dclose(rds);
        H5Fclose(rfid);
        H5Pclose(nfapl);
    }

    (void)n_elem;
    ret = 0;

done:
    if (ds >= 0)
        H5Dclose(ds);
    if (fspace >= 0)
        H5Sclose(fspace);
    if (fid >= 0)
        H5Fclose(fid);
    if (fapl >= 0)
        H5Pclose(fapl);
    return ret;
}

/* The load-bearing assertion: bracketed and unbracketed must agree. */
static void
check_agrees(const char *label, int rank, const hsize_t *dims, hid_t mspace, const int *mem, int n_elem)
{
    int *pt   = (int *)calloc((size_t)n_elem, sizeof(int));
    int *step = (int *)calloc((size_t)n_elem, sizeof(int));
    int  rp, rs, i;

    if (!pt || !step) {
        printf("  %-28s FAIL (out of memory)\n", label);
        nerrors++;
        free(pt);
        free(step);
        return;
    }

    rp = run_case("t_memspace_pt.h5", 0, rank, dims, mspace, mem, n_elem, pt);
    rs = run_case("t_memspace_step.h5", 1, rank, dims, mspace, mem, n_elem, step);

    if (rp != 0) {
        printf("  %-28s FAIL (pass-through write itself failed -- test bug)\n", label);
        nerrors++;
    }
    else if (rs != 0) {
        printf("  %-28s FAIL (captured write failed where pass-through succeeded)\n", label);
        nerrors++;
    }
    else {
        for (i = 0; i < n_elem; i++)
            if (pt[i] != step[i]) {
                printf("  %-28s FAIL at element %d: pass-through %d, captured %d\n", label, i, pt[i],
                       step[i]);
                nerrors++;
                break;
            }
        if (i == n_elem)
            printf("  %-28s ok\n", label);
    }

    free(pt);
    free(step);
}

int
main(void)
{
    int mem[MEMLEN];
    int i;

    for (i = 0; i < MEMLEN; i++)
        mem[i] = 1000 + i;

    printf("memory-selection capture\n");

    /* 1. Interior of a haloed buffer -- the original failure. */
    {
        hsize_t dims[1]  = {8};
        hsize_t md[1]    = {MEMLEN};
        hsize_t off[1]   = {4};
        hsize_t cnt[1]   = {8};
        hid_t   ms       = H5Screate_simple(1, md, NULL);

        H5Sselect_hyperslab(ms, H5S_SELECT_SET, off, NULL, cnt, NULL);
        check_agrees("1-D interior hyperslab", 1, dims, ms, mem, 8);
        H5Sclose(ms);
    }

    /* 2. Strided -- more than one flat run. */
    {
        hsize_t dims[1] = {8};
        hsize_t md[1]   = {MEMLEN};
        hsize_t off[1]  = {1};
        hsize_t str[1]  = {2};
        hsize_t cnt[1]  = {8};
        hid_t   ms      = H5Screate_simple(1, md, NULL);

        H5Sselect_hyperslab(ms, H5S_SELECT_SET, off, str, cnt, NULL);
        check_agrees("1-D strided selection", 1, dims, ms, mem, 8);
        H5Sclose(ms);
    }

    /* 3. A 2-D column: genuinely discontiguous flat runs. */
    {
        hsize_t dims[1]  = {6};
        hsize_t md[2]    = {6, 8};
        hsize_t off[2]   = {0, 3};
        hsize_t cnt[2]   = {6, 1};
        hid_t   ms       = H5Screate_simple(2, md, NULL);

        H5Sselect_hyperslab(ms, H5S_SELECT_SET, off, NULL, cnt, NULL);
        check_agrees("2-D column selection", 1, dims, ms, mem, 6);
        H5Sclose(ms);
    }

    /* 4. The packed fast path must still work. */
    {
        hsize_t dims[1] = {8};
        hsize_t md[1]   = {8};
        hid_t   ms      = H5Screate_simple(1, md, NULL);

        check_agrees("unselected simple space", 1, dims, ms, mem, 8);
        H5Sclose(ms);
        check_agrees("H5S_ALL memory space", 1, dims, H5S_ALL, mem, 8);
    }

    /* 5. A point selection must be refused, not mis-captured. */
    {
        hsize_t dims[1]    = {4};
        hsize_t md[1]      = {MEMLEN};
        hsize_t coords[4]  = {2, 9, 17, 30};
        hid_t   ms         = H5Screate_simple(1, md, NULL);
        int     out[4];
        int     r;

        H5Sselect_elements(ms, H5S_SELECT_SET, 4, coords);
        /* The refusal is the expected result, so its error stack is noise
         * here -- suppress it rather than have a passing run print a
         * diagnostic that looks like a failure. */
        H5E_BEGIN_TRY
        {
            r = run_case("t_memspace_pts.h5", 1, 1, dims, ms, mem, 4, out);
        }
        H5E_END_TRY
        if (r == -2)
            printf("  %-28s ok (refused, not mis-captured)\n", "point selection");
        else {
            printf("  %-28s FAIL (expected refusal, got %s)\n", "point selection",
                   r == 0 ? "a silent capture" : "an unexpected error");
            nerrors++;
        }
        H5Sclose(ms);
    }

    if (nerrors) {
        printf("t_memspace: %d FAILURE(S)\n", nerrors);
        return 1;
    }
    printf("t_memspace: all cases passed\n");
    return 0;
}
