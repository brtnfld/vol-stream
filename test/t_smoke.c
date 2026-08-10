/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M0 smoke test.
 *
 * Three things, and nothing more ambitious than that:
 *
 *   1. The connector loads and is actually the one in use.
 *   2. A round trip of data through it returns what went in.
 *   3. The step operations are registered and discoverable via
 *      H5VLquery_optional(), and calling them is harmless.
 *
 * The real M0 exit gate is HDF5's own test/API suite run through the
 * connector -- see test/run_api_suite.sh.  This program only fails fast with a
 * readable message when something basic is wrong, because a broken plugin
 * otherwise surfaces as an inscrutable failure deep inside that suite.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FILENAME "t_smoke.h5"
#define DSET     "data"
#define NELEM    64

static int nerrors = 0;

#define CHECK(expr, what)                                                                                    \
    do {                                                                                                     \
        if ((expr) < 0) {                                                                                    \
            printf("  FAIL  %s (%s:%d)\n", (what), __FILE__, __LINE__);                                      \
            nerrors++;                                                                                       \
            goto done;                                                                                       \
        }                                                                                                    \
    } while (0)

#define EXPECT(cond, what)                                                                                   \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            printf("  FAIL  %s (%s:%d)\n", (what), __FILE__, __LINE__);                                      \
            nerrors++;                                                                                       \
        }                                                                                                    \
        else                                                                                                 \
            printf("  ok    %s\n", (what));                                                                  \
    } while (0)

/* Confirm the file really is using vol-stream and not silently falling back to
 * the native connector, which would make every other check meaningless. */
static void
check_connector_in_use(hid_t fid)
{
    char  name[64] = "";
    hid_t conn     = H5I_INVALID_HID;

    if ((conn = H5VLget_connector_id(fid)) < 0) {
        printf("  FAIL  H5VLget_connector_id\n");
        nerrors++;
        return;
    }
    if (H5VLget_connector_name(fid, name, sizeof(name)) < 0) {
        printf("  FAIL  H5VLget_connector_name\n");
        nerrors++;
        H5VLclose(conn);
        return;
    }

    EXPECT(strcmp(name, H5VL_STREAM_NAME) == 0, "connector in use is vol-stream");
    if (strcmp(name, H5VL_STREAM_NAME) != 0)
        printf("        (got \"%s\", wanted \"%s\")\n", name, H5VL_STREAM_NAME);

    H5VLclose(conn);
}

/* The step ops must be registered and reported, including the collective flag:
 * that reporting is what substitutes for a streaming capability flag, which
 * would need a library change. */
static void
check_step_ops_discoverable(hid_t fid)
{
    struct {
        const char *op_name;
        const char *label;
        int         expect_collective;
    } ops[] = {
        {H5VL_STREAM_OP_BEGIN_STEP, "begin_step registered", 1},
        {H5VL_STREAM_OP_END_STEP, "end_step registered", 1},
        {H5VL_STREAM_OP_STEP_STATUS, "step_status registered", 0},
        {H5VL_STREAM_OP_SUBSCRIBE, "subscribe registered", 0},
    };

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        int      op_val = -1;
        uint64_t flags  = 0;

        if (H5VLfind_opt_operation(H5VL_SUBCLS_FILE, ops[i].op_name, &op_val) < 0) {
            printf("  FAIL  %s (not in the optional-op registry)\n", ops[i].label);
            nerrors++;
            continue;
        }
        if (H5VLquery_optional(fid, H5VL_SUBCLS_FILE, op_val, &flags) < 0) {
            printf("  FAIL  %s (H5VLquery_optional)\n", ops[i].label);
            nerrors++;
            continue;
        }

        EXPECT(flags & H5VL_OPT_QUERY_SUPPORTED, ops[i].label);

        if (ops[i].expect_collective)
            EXPECT(flags & H5VL_OPT_QUERY_COLLECTIVE, "  ...and reports itself collective");
    }
}

int
main(void)
{
    hid_t   fapl = H5I_INVALID_HID, fid = H5I_INVALID_HID;
    hid_t   space = H5I_INVALID_HID, dset = H5I_INVALID_HID;
    hsize_t dims[1] = {NELEM};
    int     wbuf[NELEM], rbuf[NELEM];
    hid_t   vol_id = H5I_INVALID_HID;

    H5F_step_status_t status = (H5F_step_status_t)-1;

    printf("vol-stream M0 smoke test\n");

    for (int i = 0; i < NELEM; i++)
        wbuf[i] = i * 3 - 1;
    memset(rbuf, 0, sizeof(rbuf));

    /* Register explicitly rather than relying on HDF5_VOL_CONNECTOR, so this
     * test is meaningful whether or not the environment is set. */
    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("  FAIL  H5VL_stream_register\n");
        return 1;
    }
    printf("  ok    connector registered\n");

    CHECK(fapl = H5Pcreate(H5P_FILE_ACCESS), "H5Pcreate(FAPL)");
    CHECK(H5Pset_vol(fapl, vol_id, NULL), "H5Pset_vol");

    CHECK(fid = H5Fcreate(FILENAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl), "H5Fcreate");

    check_connector_in_use(fid);
    check_step_ops_discoverable(fid);

    /* Step status before any step has been opened. */
    CHECK(H5Fstep_status(fid, &status), "H5Fstep_status");
    EXPECT(status == H5F_STEP_NOT_IN_STEP, "step status is NOT_IN_STEP before begin_step");

    /* M0: bracketing is accepted and does nothing.  The point of exercising it
     * here is that the plumbing from the public wrapper through
     * H5VLfile_optional_op to the connector callback works end to end. */
    {
        const uint64_t logical[2] = {500, 550};

        CHECK(H5Fbegin_step(fid, 2, logical, 0), "H5Fbegin_step with logical ids");
        printf("  ok    H5Fbegin_step accepted logical ids\n");
    }

    CHECK(space = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
    CHECK(dset = H5Dcreate2(fid, DSET, H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
          "H5Dcreate2");
    CHECK(H5Dwrite(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, wbuf), "H5Dwrite");

    CHECK(H5Fend_step(fid), "H5Fend_step");
    printf("  ok    H5Fend_step accepted\n");

    CHECK(H5Dread(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rbuf), "H5Dread");
    EXPECT(memcmp(wbuf, rbuf, sizeof(wbuf)) == 0, "data round-trips through the connector");

    /* An empty subscription is the degenerate case and must not be an error. */
    CHECK(H5Fsubscribe(fid, 0, NULL, NULL, NULL), "H5Fsubscribe (empty)");
    printf("  ok    H5Fsubscribe accepted\n");

done:
    if (dset > 0)
        H5Dclose(dset);
    if (space > 0)
        H5Sclose(space);
    if (fid > 0)
        H5Fclose(fid);
    if (fapl > 0)
        H5Pclose(fapl);
    if (vol_id > 0)
        H5VLclose(vol_id);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
