/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * First real step toward reference support (docs/dev-plan.md Decision #4):
 * an application can now create an H5R reference using the *logical* path it
 * actually wrote to.
 *
 * The problem this closes. Steps land group-based, so an object the
 * application created as "/target" physically lives at "/step/<k>/target".
 * H5Rcreate_object() reaches a connector as an object LOOKUP by name, and
 * H5VL_stream_object_specific() used to pass that name straight through to
 * the under connector -- which knows nothing about logical paths and
 * answered "object 'target' doesn't exist". Measured directly with standalone
 * probes before any of this was written; see that section of dev-plan.md,
 * which this test's cases mirror one for one.
 *
 * What is asserted here:
 *   1. A reference to a target from an ALREADY COMMITTED step succeeds using
 *      the logical path -- the case that used to fail and now works.
 *   2. It resolves to the right object: H5Rget_obj_name() reports the
 *      physical "/step/<k>/target", proving the translation really happened
 *      rather than the call merely not erroring.
 *   3. A reference to a target created in the STILL-OPEN step fails, and is
 *      expected to. Deferred writes mean that object does not exist in the
 *      underlying file until end_step() replays the manifest, so there is
 *      genuinely nothing to point at yet. Asserted explicitly so the
 *      restriction stays a deliberate, documented property instead of
 *      quietly becoming a silent wrong-object bug later.
 *
 * Single process, no transport -- this is purely about path resolution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_ref_path.h5"

int
main(void)
{
    hid_t     vol_id, fapl, fid, space, ds;
    hsize_t   one = 1;
    H5R_ref_t ref;
    char      name[256];
    int       val      = 42;
    int       nerrors  = 0;

    printf("vol-stream: H5R logical-path resolution\n");

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("  FAIL  register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("  FAIL  fapl\n");
        return 1;
    }
    unlink(FNAME);
    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("  FAIL  create\n");
        return 1;
    }

    /* --- Step 0: create /target and commit it. --- */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  begin_step 0\n");
        return 1;
    }
    if ((space = H5Screate_simple(1, &one, NULL)) < 0 ||
        (ds = H5Dcreate2(fid, "/target", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val) < 0) {
        printf("  FAIL  create/write /target\n");
        return 1;
    }
    H5Dclose(ds);
    if (H5Fend_step(fid) < 0) {
        printf("  FAIL  end_step 0\n");
        return 1;
    }

    /* --- Step 1: reference the now-committed target by its logical path. --- */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  begin_step 1\n");
        return 1;
    }

    memset(&ref, 0, sizeof(ref));
    if (H5Rcreate_object(fid, "/target", H5P_DEFAULT, &ref) < 0) {
        printf("  FAIL  H5Rcreate_object on a committed target's logical path\n");
        nerrors++;
    }
    else {
        printf("  ok    H5Rcreate_object accepted the logical path \"/target\"\n");

        /* Resolving to the *physical* path is the proof translation ran. */
        if (H5Rget_obj_name(&ref, H5P_DEFAULT, name, sizeof(name)) < 0) {
            printf("  FAIL  H5Rget_obj_name\n");
            nerrors++;
        }
        else if (strcmp(name, "/step/0/target") != 0) {
            printf("  FAIL  reference resolved to '%s', expected '/step/0/target'\n", name);
            nerrors++;
        }
        else
            printf("  ok    resolved to the physical path '%s'\n", name);

        H5Rdestroy(&ref);
    }

    /* --- Same step: a target that has not been committed yet. --- */
    if ((ds = H5Dcreate2(fid, "/uncommitted", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT,
                         H5P_DEFAULT)) < 0) {
        printf("  FAIL  create /uncommitted\n");
        return 1;
    }
    H5Dclose(ds);

    memset(&ref, 0, sizeof(ref));
    H5E_BEGIN_TRY
    {
        if (H5Rcreate_object(fid, "/uncommitted", H5P_DEFAULT, &ref) >= 0) {
            printf("  FAIL  referencing a same-step target succeeded; it cannot point at anything "
                   "real until end_step() materializes it\n");
            nerrors++;
            H5Rdestroy(&ref);
        }
        else
            printf("  ok    referencing a still-uncommitted same-step target is refused\n");
    }
    H5E_END_TRY

    H5Sclose(space);
    H5Fend_step(fid);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
