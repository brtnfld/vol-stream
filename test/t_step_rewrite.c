/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Writing the same dataset in successive steps through the handle it was
 * created with -- the most ordinary streaming pattern there is, and the one
 * nothing in this suite exercised until M9's predicate test needed a variable
 * that changes step to step.
 *
 * What it used to do, found 2026-08-15 by running it: nothing. Capture keyed
 * strictly on a dataset wrapper still being a *placeholder*, i.e. created in
 * the step now open and not yet materialized. Once end_step() replayed it the
 * wrapper went live, and every later H5Dwrite through that same handle --
 * inside a step or not -- fell through to the under connector and landed
 * directly on the step-0 copy of the object. Consequences, all silent:
 *
 *   - step 1's data never entered a manifest, so no push ever reached a
 *     subscriber and /step/1/ was created empty (a 0-byte .payload);
 *   - step 0's own copy was overwritten in place, so the step history was
 *     not merely incomplete but retroactively wrong -- /step/0/temp held
 *     the values of whichever step wrote last.
 *
 * Both halves are asserted below, and both are checked through the *native*
 * connector, so this measures what actually landed in the file rather than
 * what the connector believes it did.
 *
 * The fix captures a write to a live dataset inside a step by synthesizing
 * that step's own DsetCreate for the path (see H5VL_stream_dataset_write()),
 * which is exactly what the application would have got by re-creating the
 * dataset each step -- the pattern the connector already handled, and the
 * one dev-plan.md decision #2 describes as successive versions of one named
 * object landing group-based at /step/<n>/.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME  "t_step_rewrite.h5"
#define NELEM  8
#define NSTEPS 3

/* Step s writes s*100 + i, so every step's values are distinguishable and a
 * stale copy is obvious rather than plausible. */
static int
val_for(int s, int i)
{
    return s * 100 + i;
}

static int
check_step(hid_t nfid, int s)
{
    char  path[64];
    hid_t ds;
    int   got[NELEM];
    int   i, rc = 0;

    snprintf(path, sizeof(path), "/step/%d/temp", s);

    if ((ds = H5Dopen2(nfid, path, H5P_DEFAULT)) < 0) {
        printf("  FAIL  %s does not exist -- step %d's write was never captured\n", path, s);
        return 1;
    }
    if (H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) < 0) {
        printf("  FAIL  cannot read %s\n", path);
        H5Dclose(ds);
        return 1;
    }
    for (i = 0; i < NELEM; i++)
        if (got[i] != val_for(s, i)) {
            printf("  FAIL  %s[%d] = %d, expected %d", path, i, got[i], val_for(s, i));
            /* Naming the step whose values these actually are turns "wrong
             * number" into "overwritten by step N", the real diagnosis. */
            if (got[i] % 100 == i)
                printf(" -- these are step %d's values", got[i] / 100);
            printf("\n");
            rc = 1;
            break;
        }

    H5Dclose(ds);
    return rc;
}

int
main(void)
{
    hid_t   vol_id, fapl, fid, space, ds, nfid;
    hsize_t dims = NELEM;
    int     vals[NELEM];
    int     s, i, nerrors = 0;

    printf("vol-stream: rewriting one dataset across steps, through one handle\n");

    unlink(FNAME);

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("  FAIL  register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0) {
        printf("  FAIL  fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("  FAIL  create\n");
        return 1;
    }
    if ((space = H5Screate_simple(1, &dims, NULL)) < 0) {
        printf("  FAIL  dataspace\n");
        return 1;
    }

    /* Created once, in step 0, and deliberately never re-created: the handle
     * stays open across every step, which is how an application actually
     * writes a variable each iteration. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  begin_step 0\n");
        return 1;
    }
    if ((ds = H5Dcreate2(fid, "/temp", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        printf("  FAIL  create /temp\n");
        return 1;
    }
    for (i = 0; i < NELEM; i++)
        vals[i] = val_for(0, i);
    if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0) {
        printf("  FAIL  write step 0\n");
        return 1;
    }
    if (H5Fend_step(fid) < 0) {
        printf("  FAIL  end_step 0\n");
        return 1;
    }

    for (s = 1; s < NSTEPS; s++) {
        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            printf("  FAIL  begin_step %d\n", s);
            return 1;
        }
        for (i = 0; i < NELEM; i++)
            vals[i] = val_for(s, i);
        if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0) {
            printf("  FAIL  write step %d\n", s);
            return 1;
        }
        if (H5Fend_step(fid) < 0) {
            printf("  FAIL  end_step %d\n", s);
            return 1;
        }
    }

    H5Dclose(ds);
    H5Sclose(space);
    H5Fclose(fid);

    /* Through the native connector: what is actually in the file. */
    if ((nfid = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  reopen with the native connector\n");
        return 1;
    }

    for (s = 0; s < NSTEPS; s++)
        nerrors += check_step(nfid, s);

    if (!nerrors)
        printf("  ok    each of %d steps holds its own values at /step/<n>/temp\n", NSTEPS);

    /* A partial write through the same live handle: same capture path, but
     * only part of the extent, so it also pins that the synthesized create
     * carries the dataset's full extent rather than the write's selection. */
    {
        hid_t   pfid, pspace, pds, fspace;
        hsize_t start = 2, count = 3;
        int     part[3] = {901, 902, 903};
        int     got[NELEM];

        H5Fclose(nfid);

        unlink("t_step_rewrite_partial.h5");
        if ((pfid = H5Fcreate("t_step_rewrite_partial.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
            printf("  FAIL  create (partial case)\n");
            return 1;
        }
        if ((pspace = H5Screate_simple(1, &dims, NULL)) < 0) {
            printf("  FAIL  dataspace (partial case)\n");
            return 1;
        }

        if (H5Fbegin_step(pfid, 0, NULL, 0) < 0 ||
            (pds = H5Dcreate2(pfid, "/temp", H5T_NATIVE_INT, pspace, H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT)) < 0) {
            printf("  FAIL  begin/create (partial case)\n");
            return 1;
        }
        for (i = 0; i < NELEM; i++)
            vals[i] = val_for(0, i);
        if (H5Dwrite(pds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 ||
            H5Fend_step(pfid) < 0) {
            printf("  FAIL  write/end step 0 (partial case)\n");
            return 1;
        }

        if (H5Fbegin_step(pfid, 0, NULL, 0) < 0) {
            printf("  FAIL  begin_step 1 (partial case)\n");
            return 1;
        }
        if ((fspace = H5Dget_space(pds)) < 0 ||
            H5Sselect_hyperslab(fspace, H5S_SELECT_SET, &start, NULL, &count, NULL) < 0) {
            printf("  FAIL  hyperslab (partial case)\n");
            return 1;
        }
        {
            hsize_t mdims = count;
            hid_t   mspace = H5Screate_simple(1, &mdims, NULL);

            if (mspace < 0 || H5Dwrite(pds, H5T_NATIVE_INT, mspace, fspace, H5P_DEFAULT, part) < 0) {
                printf("  FAIL  partial write\n");
                return 1;
            }
            H5Sclose(mspace);
        }
        H5Sclose(fspace);
        if (H5Fend_step(pfid) < 0) {
            printf("  FAIL  end_step 1 (partial case)\n");
            return 1;
        }

        H5Dclose(pds);
        H5Sclose(pspace);
        H5Fclose(pfid);

        if ((nfid = H5Fopen("t_step_rewrite_partial.h5", H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
            printf("  FAIL  reopen (partial case)\n");
            return 1;
        }
        if ((pds = H5Dopen2(nfid, "/step/1/temp", H5P_DEFAULT)) < 0) {
            printf("  FAIL  /step/1/temp missing after a partial write to a live dataset\n");
            nerrors++;
        }
        else {
            int bad = 0;

            if (H5Dread(pds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) < 0) {
                printf("  FAIL  cannot read /step/1/temp (partial case)\n");
                nerrors++;
            }
            else {
                /* The extent must be the dataset's, and the three written
                 * elements must be at their own indices. Elements outside the
                 * selection are simply unwritten in this step's copy -- the
                 * step carries what the step wrote. */
                for (i = 0; i < (int)count; i++)
                    if (got[(int)start + i] != part[i]) {
                        printf("  FAIL  /step/1/temp[%d] = %d, expected %d\n", (int)start + i,
                               got[(int)start + i], part[i]);
                        bad = 1;
                        break;
                    }
                if (bad)
                    nerrors++;
                else
                    printf("  ok    a partial write to a live dataset lands at its own indices in its own "
                           "step\n");
            }
            H5Dclose(pds);
        }
    }

    H5Fclose(nfid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
