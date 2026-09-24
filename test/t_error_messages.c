/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * A failed step-API call says why, on the error stack.
 *
 * Each case makes a call that must fail, then looks on the error stack --
 * before any other HDF5 call, which would clear it -- for the connector's
 * own frame naming the reason. These used to fail with a bare -1 and only
 * HDF5's generic "unable to execute file optional callback" frame.
 *
 * No transport needed.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_error_messages.h5"

static int         nerrors = 0;
static const char *want    = NULL;
static int         found   = 0;

static herr_t
find(unsigned n, const H5E_error2_t *err, void *udata)
{
    (void)n;
    (void)udata;
    if (err && err->desc && strstr(err->desc, want))
        found = 1;
    return 0;
}

/* r is the call's result; the stack is still the call's own here. */
static void
expect(herr_t r, const char *what, const char *text)
{
    want  = text;
    found = 0;
    if (r < 0)
        H5Ewalk2(H5E_DEFAULT, H5E_WALK_DOWNWARD, find, NULL);
    if (r < 0 && found)
        printf("  ok    %s: fails, and says \"...%s...\"\n", what, text);
    else {
        printf("  FAIL  %s: %s\n", what, r >= 0 ? "did not fail" : "no frame with the reason");
        nerrors++;
    }
    H5Eclear2(H5E_DEFAULT);
}

int
main(void)
{
    hid_t    fapl, wfid, rfid, sp, ds, bad;
    hsize_t  n = 4;
    int      vals[4] = {0, 1, 2, 3};
    uint64_t lid     = 7, phys = 0, wall = 0;
    size_t   nlog    = 0;
    herr_t   r;

    printf("vol-stream: a failed step-API call says why\n");
    unlink(FNAME);
    fapl = H5Pcreate(H5P_FILE_ACCESS);
    if (H5Pset_fapl_stream(fapl, NULL) < 0 || (wfid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (sp = H5Screate_simple(1, &n, NULL)) < 0 || H5Fbegin_step(wfid, 1, &lid, 0) < 0 ||
        (ds = H5Dcreate2(wfid, "/x", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(wfid) < 0) {
        printf("  FAIL  setup\n");
        return 1;
    }

    H5E_BEGIN_TRY
    {
        r = H5Fget_logical_steps(wfid, &nlog, NULL);
    }
    H5E_END_TRY
    expect(r, "H5Fget_logical_steps() on a writer", "reader-only");

    H5Dclose(ds);
    H5Fclose(wfid);

    if ((rfid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0 || H5Fbegin_step(rfid, 0, NULL, 0) < 0) {
        printf("  FAIL  reader setup\n");
        return 1;
    }

    H5E_BEGIN_TRY
    {
        r = H5Fbegin_step(rfid, 0, NULL, 0);
    }
    H5E_END_TRY
    expect(r, "a reader stepping past the last step", "no step 1");

    H5E_BEGIN_TRY
    {
        r = H5Fbegin_logical_step(rfid, 12345);
    }
    H5E_END_TRY
    expect(r, "an unknown logical step", "no step carries logical id 12345");

    H5E_BEGIN_TRY
    {
        r = H5Fset_stream_queue_policy(rfid, H5VL_STREAM_QUEUE_BLOCK, 0);
    }
    H5E_END_TRY
    expect(r, "a queue policy on a reader", "writer-only");

    bad = H5Pcreate(H5P_FILE_ACCESS); /* not a DCPL */
    {
        const char *paths[1]  = {"/x"};
        const hid_t spaces[1] = {sp};
        const hid_t plists[1] = {bad};

        H5E_BEGIN_TRY
        {
            r = H5Fsubscribe(rfid, 1, paths, spaces, plists);
        }
        H5E_END_TRY
    }
    expect(r, "H5Fsubscribe() with a FAPL for a DCPL", "not a dataset creation property list");
    H5Pclose(bad);

    /* This file has no transport (VOL_STREAM_NA unset, no na on the FAPL);
     * a build without Mercury says so instead -- both name the transport. */
    H5E_BEGIN_TRY
    {
        r = H5Fwait_step_ready(rfid, 0, &phys, &wall);
    }
    H5E_END_TRY
    expect(r, "H5Fwait_step_ready() without a transport", "transport");

    H5Fclose(rfid);
    H5Sclose(sp);
    H5Pclose(fapl);
    unlink(FNAME);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
