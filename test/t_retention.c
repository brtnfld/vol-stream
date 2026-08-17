/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Step retention: H5Fset_stream_retention_policy().
 *
 * vol-stream used as a live transit pipe rather than an archive. Every
 * committed step creates a real /step/<n> group, so an unbounded run grows
 * the file's metadata without limit. Retention bounds that by unlinking the
 * oldest steps after each commit.
 *
 * Four assertions, in increasing order of how easy they would be to get
 * wrong:
 *
 *   1. With no policy set, nothing is pruned -- the default must remain the
 *      unbounded history the rest of the connector assumes.
 *   2. With max_steps = K, exactly the K most recent steps survive, and they
 *      are the *newest* K, not an arbitrary K.
 *   3. The surviving steps still hold correct data. Pruning must not disturb
 *      what it keeps, which a count-only check would not catch.
 *   4. A policy set mid-stream bounds the whole history, including steps
 *      committed before the call -- the seeding path in the set_retention
 *      handler. Set it after 6 steps with max_steps = 2 and only 2 may
 *      remain, not 2-plus-the-6-already-there.
 *
 * Needs no transport: retention is a pure file-side operation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NELEM 16

static int nerrors = 0;

#define EXPECT(cond, what)                                                                                   \
    do {                                                                                                     \
        if (!(cond)) {                                                                                        \
            printf("  FAIL  %s (%s:%d)\n", (what), __FILE__, __LINE__);                                      \
            nerrors++;                                                                                       \
        }                                                                                                    \
        else                                                                                                 \
            printf("  ok    %s\n", (what));                                                                  \
    } while (0)

/* Does /step/<n> exist in a file opened natively (no connector)? */
static int
step_exists(const char *fn, unsigned n)
{
    hid_t fid;
    char  path[64];
    int   found;

    if ((fid = H5Fopen(fn, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0)
        return -1;
    snprintf(path, sizeof(path), "/step/%u", n);
    found = (H5Lexists(fid, path, H5P_DEFAULT) > 0) ? 1 : 0;
    H5Fclose(fid);
    return found;
}

/* Read /step/<n>/data[0] from a file opened natively. -1 if unreadable. */
static int
step_first_value(const char *fn, unsigned n)
{
    hid_t fid, ds;
    char  path[64];
    int   buf[NELEM];

    if ((fid = H5Fopen(fn, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0)
        return -1;
    snprintf(path, sizeof(path), "/step/%u/data", n);
    if ((ds = H5Dopen2(fid, path, H5P_DEFAULT)) < 0) {
        H5Fclose(fid);
        return -1;
    }
    if (H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) < 0) {
        H5Dclose(ds);
        H5Fclose(fid);
        return -1;
    }
    H5Dclose(ds);
    H5Fclose(fid);
    return buf[0];
}

/*
 * Write n_steps steps of /data, each filled with (step * 1000).
 *
 * set_after < 0 sets no policy at all; otherwise the policy is applied after
 * set_after committed steps, which is how assertion 4 exercises seeding.
 */
static int
write_steps(const char *fn, hid_t vol_id, int n_steps, int set_after, size_t max_steps)
{
    hid_t   fapl, fid, sp;
    hsize_t dims[1] = {NELEM};
    int     buf[NELEM];
    int     ret = -1;

    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0)
        return -1;
    if (H5Pset_vol(fapl, vol_id, NULL) < 0) {
        H5Pclose(fapl);
        return -1;
    }
    if ((fid = H5Fcreate(fn, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        H5Pclose(fapl);
        return -1;
    }

    if (set_after == 0 && H5Fset_stream_retention_policy(fid, max_steps, 0) < 0)
        goto done;

    sp = H5Screate_simple(1, dims, NULL);

    for (int s = 0; s < n_steps; s++) {
        hid_t ds;

        for (int i = 0; i < NELEM; i++)
            buf[i] = s * 1000 + i;

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0)
            goto done_sp;
        if ((ds = H5Dcreate2(fid, "data", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0)
            goto done_sp;
        if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) < 0) {
            H5Dclose(ds);
            goto done_sp;
        }
        H5Dclose(ds);
        if (H5Fend_step(fid) < 0)
            goto done_sp;

        if (set_after > 0 && s + 1 == set_after &&
            H5Fset_stream_retention_policy(fid, max_steps, 0) < 0)
            goto done_sp;
    }
    ret = 0;

done_sp:
    H5Sclose(sp);
done:
    H5Fclose(fid);
    H5Pclose(fapl);
    return ret;
}

int
main(void)
{
    hid_t vol_id;

    printf("t_retention: step retention policy\n");

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("  FAIL  could not register vol-stream\n");
        return 1;
    }

    /* 1. No policy: every step survives. */
    {
        const char *fn = "t_retention_none.h5";

        EXPECT(write_steps(fn, vol_id, 8, -1, 0) == 0, "wrote 8 steps, no retention policy");
        EXPECT(step_exists(fn, 0) == 1, "no policy: oldest step 0 retained");
        EXPECT(step_exists(fn, 7) == 1, "no policy: newest step 7 retained");
        remove(fn);
    }

    /* 2 + 3. max_steps = 3 over 10 steps: only 7, 8, 9 survive, intact. */
    {
        const char *fn = "t_retention_bound.h5";

        EXPECT(write_steps(fn, vol_id, 10, 0, 3) == 0, "wrote 10 steps, max_steps = 3");

        EXPECT(step_exists(fn, 0) == 0, "step 0 pruned");
        EXPECT(step_exists(fn, 5) == 0, "step 5 pruned");
        EXPECT(step_exists(fn, 6) == 0, "step 6 pruned");
        EXPECT(step_exists(fn, 7) == 1, "step 7 retained");
        EXPECT(step_exists(fn, 8) == 1, "step 8 retained");
        EXPECT(step_exists(fn, 9) == 1, "step 9 retained (newest)");

        /* The survivors are the newest, and pruning left their data alone. */
        EXPECT(step_first_value(fn, 7) == 7000, "retained step 7 holds its own data");
        EXPECT(step_first_value(fn, 9) == 9000, "retained step 9 holds its own data");
        remove(fn);
    }

    /* 4. Policy set mid-stream bounds the whole history, not just what
     *    follows it -- the seeding path. 10 steps, policy set after 6 with
     *    max_steps = 2, so only 8 and 9 may survive. */
    {
        const char *fn = "t_retention_mid.h5";

        EXPECT(write_steps(fn, vol_id, 10, 6, 2) == 0, "wrote 10 steps, policy set after 6, max_steps = 2");
        EXPECT(step_exists(fn, 0) == 0, "mid-stream policy pruned pre-policy step 0");
        EXPECT(step_exists(fn, 5) == 0, "mid-stream policy pruned pre-policy step 5");
        EXPECT(step_exists(fn, 7) == 0, "mid-stream policy pruned step 7");
        EXPECT(step_exists(fn, 8) == 1, "step 8 retained");
        EXPECT(step_exists(fn, 9) == 1, "step 9 retained (newest)");
        EXPECT(step_first_value(fn, 9) == 9000, "retained step 9 holds its own data");
        remove(fn);
    }

    if (nerrors) {
        printf("t_retention: %d FAILURE(S)\n", nerrors);
        return 1;
    }
    printf("t_retention: all tests passed\n");
    return 0;
}
