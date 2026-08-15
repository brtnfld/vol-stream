/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Registering the connector more than once in a process.
 *
 * Found 2026-08-15 while writing test/t_onion_history.c, which needed to
 * build two streams in one process: the second H5VL_stream_register() failed,
 * and the error was nowhere near the cause --
 *
 *   H5VLregister_connector(): unable to register VOL class
 *     H5VL__register_connector(): unable to init VOL connector
 *       H5VLregister_opt_operation(): can't register dynamic optional
 *         operation: 'vol-stream:begin_step'
 *           H5VL__register_opt_operation(): operation name already exists
 *
 * HDF5's dynamic-operation registry is global to the process and outlives any
 * one registration of a connector, while the connector's init callback runs
 * on every registration -- so the second one collided with the first one's
 * names. The connector was effectively single-use per process: register,
 * close, register again did not work, and neither would two independent
 * components in one program each registering it. Nothing documented that,
 * and nothing caught it, because every test until now registered exactly
 * once.
 *
 * The fix makes init idempotent (H5VL__stream_register_op() recovers the
 * already-assigned value with H5VLfind_opt_operation()). This test pins the
 * behavior directly, without the onion machinery that happened to surface it:
 * two full register/use/close cycles, and -- the part that would catch a
 * fix that merely stopped erroring -- the second cycle really performs a
 * step, so the recovered operation values must be the *working* ones.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME_FMT "t_reregister_%d.h5"

/* One complete register -> create -> step -> close cycle. */
static int
cycle(int n)
{
    hid_t vol_id, fapl, fid, sp, ds;
    char  fname[64];
    int   v = 100 + n;

    snprintf(fname, sizeof(fname), FNAME_FMT, n);
    unlink(fname);

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("  FAIL  cycle %d: register\n", n);
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0) {
        printf("  FAIL  cycle %d: fapl\n", n);
        return 1;
    }
    if ((fid = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("  FAIL  cycle %d: create\n", n);
        return 1;
    }

    /* A real step, not just a file open: begin/end_step go through the very
     * optional-operation values the second registration had to recover. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  cycle %d: begin_step -- the operation values did not survive re-registration\n", n);
        return 1;
    }
    if ((sp = H5Screate(H5S_SCALAR)) < 0 ||
        (ds = H5Dcreate2(fid, "/v", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &v) < 0) {
        printf("  FAIL  cycle %d: write\n", n);
        return 1;
    }
    H5Dclose(ds);
    H5Sclose(sp);
    if (H5Fend_step(fid) < 0) {
        printf("  FAIL  cycle %d: end_step\n", n);
        return 1;
    }

    {
        H5F_step_status_t st;

        if (H5Fstep_status(fid, &st) < 0 || st != H5F_STEP_NOT_IN_STEP) {
            printf("  FAIL  cycle %d: step_status\n", n);
            return 1;
        }
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    /* What actually landed, read with the native connector. */
    {
        hid_t nfid, nds;
        int   got = -1;

        if ((nfid = H5Fopen(fname, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
            printf("  FAIL  cycle %d: reopen natively\n", n);
            return 1;
        }
        if ((nds = H5Dopen2(nfid, "/step/0/v", H5P_DEFAULT)) < 0) {
            printf("  FAIL  cycle %d: /step/0/v missing -- the step did not replay\n", n);
            H5Fclose(nfid);
            return 1;
        }
        H5Dread(nds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &got);
        H5Dclose(nds);
        H5Fclose(nfid);

        if (got != v) {
            printf("  FAIL  cycle %d: /step/0/v = %d, expected %d\n", n, got, v);
            return 1;
        }
    }

    unlink(fname);
    return 0;
}

int
main(void)
{
    int nerrors = 0;

    printf("vol-stream: the connector can be registered more than once per process\n");

    if (cycle(0) != 0)
        nerrors++;
    else
        printf("  ok    first register/step/close cycle\n");

    /* The one that used to fail. */
    if (cycle(1) != 0)
        nerrors++;
    else
        printf("  ok    second cycle after a full close -- init is idempotent\n");

    /* Two registrations alive at once: the concurrent case, which
     * unregistering the operations in term() would have broken instead. */
    {
        hid_t a = H5VL_stream_register();
        hid_t b = H5VL_stream_register();

        if (a < 0 || b < 0) {
            printf("  FAIL  two simultaneous registrations\n");
            nerrors++;
        }
        else
            printf("  ok    two registrations held at the same time\n");
        if (b >= 0)
            H5VLclose(b);
        if (a >= 0)
            H5VLclose(a);
    }

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
