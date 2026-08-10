/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M3 exit gate: the decoupled reader.
 *
 * A file opened H5F_ACC_RDONLY is a reader: H5Fbegin_step() advances a read
 * cursor instead of starting write capture, and object opens made afterward
 * transparently resolve a bare app-given path to the /step/<k>/<path>
 * replica that is authoritative as of the reader's current step. Covers the
 * three dev-plan.md exit-gate scenarios:
 *
 *   1. A selection straddling multiple writer calls (two partial hyperslab
 *      writes into the same dataset within one step) reads back correctly.
 *   2. A dataset written every 25th step resolves correctly at every
 *      intermediate step, not just the steps that wrote it -- exercised via
 *      both a nested H5Gopen2()/H5Dopen2() (the READER_VIRTUAL group path)
 *      and a one-shot multi-component H5Dopen2().
 *   3. A restart-overlap sequence -- dev-plan.md's own openPMD example
 *      (logical ids 0, 50, ..., 750, then a restart rewriting 500, 550, ...,
 *      750) -- is read back in correct logical order: H5Fget_logical_steps()
 *      dedupes to each id's authoritative (latest) occurrence, and
 *      H5Fbegin_logical_step() jumps straight to it.
 *
 * Plus the finding-#2 regression: an object built incrementally
 * (H5Gcreate2() then H5Dcreate2(), captured as "/g/sub") must still resolve
 * when read back in one shot via a leading-slash absolute path
 * ("/g/sub") -- the exact mismatch H5VL__stream_child_path()'s leading-slash
 * strip fixed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

static int nerrors = 0;

#define EXPECT(cond, what)                                                                                  \
    do {                                                                                                    \
        if (!(cond)) {                                                                                       \
            printf("  FAIL  %s (%s:%d)\n", (what), __FILE__, __LINE__);                                     \
            nerrors++;                                                                                      \
        }                                                                                                    \
        else                                                                                                 \
            printf("  ok    %s\n", (what));                                                                 \
    } while (0)

#define CHECK(expr, what)                                                                                    \
    do {                                                                                                     \
        if ((expr) < 0) {                                                                                    \
            printf("  FAIL  %s (%s:%d)\n", (what), __FILE__, __LINE__);                                      \
            nerrors++;                                                                                       \
            return -1;                                                                                       \
        }                                                                                                     \
    } while (0)

/* ------------------------------------------------------------------------
 * Scenario 1: a selection straddling two writer calls in one step.
 * ------------------------------------------------------------------------ */
#define WIDE_FILE  "t_reader_wide.h5"
#define WIDE_NELEM 64

static void
fill_wide(int *buf)
{
    for (int i = 0; i < WIDE_NELEM; i++)
        buf[i] = i * 3 - 7;
} /* end fill_wide() */

static int
write_wide(hid_t vol_id)
{
    hid_t   fapl, fid, space, ds, mspace, fspace1, fspace2;
    hsize_t dims[1]      = {WIDE_NELEM};
    hsize_t half         = WIDE_NELEM / 2;
    hsize_t start1[1]    = {0}, count1[1] = {half};
    hsize_t start2[1]    = {half}, count2[1] = {half};
    int     buf[WIDE_NELEM];
    int     ret = -1;

    fill_wide(buf);

    CHECK(fapl = H5Pcreate(H5P_FILE_ACCESS), "H5Pcreate(fapl)");
    CHECK(H5Pset_vol(fapl, vol_id, NULL), "H5Pset_vol");
    CHECK(fid = H5Fcreate(WIDE_FILE, H5F_ACC_TRUNC, H5P_DEFAULT, fapl), "H5Fcreate(wide)");
    H5Pclose(fapl);

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  H5Fbegin_step (%s:%d)\n", __FILE__, __LINE__);
        nerrors++;
        goto done;
    }

    CHECK(space = H5Screate_simple(1, dims, NULL), "H5Screate_simple(wide)");
    CHECK(ds = H5Dcreate2(fid, "wide", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
          "H5Dcreate2(wide)");
    CHECK(mspace = H5Screate_simple(1, &half, NULL), "H5Screate_simple(wide mem)");

    CHECK(fspace1 = H5Scopy(space), "H5Scopy(fspace1)");
    CHECK(H5Sselect_hyperslab(fspace1, H5S_SELECT_SET, start1, NULL, count1, NULL), "hyperslab 1");
    CHECK(H5Dwrite(ds, H5T_NATIVE_INT, mspace, fspace1, H5P_DEFAULT, buf), "H5Dwrite(wide 1)");
    H5Sclose(fspace1);

    CHECK(fspace2 = H5Scopy(space), "H5Scopy(fspace2)");
    CHECK(H5Sselect_hyperslab(fspace2, H5S_SELECT_SET, start2, NULL, count2, NULL), "hyperslab 2");
    CHECK(H5Dwrite(ds, H5T_NATIVE_INT, mspace, fspace2, H5P_DEFAULT, buf + half), "H5Dwrite(wide 2)");
    H5Sclose(fspace2);

    H5Sclose(mspace);
    H5Dclose(ds);
    H5Sclose(space);

    if (H5Fend_step(fid) < 0) {
        printf("  FAIL  H5Fend_step (%s:%d)\n", __FILE__, __LINE__);
        nerrors++;
        goto done;
    }
    ret = 0;
done:
    H5Fclose(fid);
    return ret;
} /* end write_wide() */

static void
check_wide(hid_t vol_id)
{
    hid_t   fapl, fid, ds, fspace, mspace;
    hsize_t start[1] = {24}, count[1] = {16};
    int     got[16];

    fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_vol(fapl, vol_id, NULL);
    fid = H5Fopen(WIDE_FILE, H5F_ACC_RDONLY, fapl);
    H5Pclose(fapl);
    if (fid < 0) {
        printf("  FAIL  H5Fopen(wide)\n");
        nerrors++;
        return;
    }

    EXPECT(H5Fbegin_step(fid, 0, NULL, 0) >= 0, "reader begin_step lands on the wide-dataset step");

    ds = H5Dopen2(fid, "/wide", H5P_DEFAULT);
    EXPECT(ds >= 0, "H5Dopen2(/wide) resolves through the reader index");
    if (ds < 0) {
        H5Fclose(fid);
        return;
    }

    fspace = H5Dget_space(ds);
    H5Sselect_hyperslab(fspace, H5S_SELECT_SET, start, NULL, count, NULL);
    mspace = H5Screate_simple(1, count, NULL);

    EXPECT(H5Dread(ds, H5T_NATIVE_INT, mspace, fspace, H5P_DEFAULT, got) >= 0,
           "H5Dread across the straddling selection succeeds");

    {
        int ok = 1;
        for (int i = 0; i < 16; i++)
            if (got[i] != (24 + i) * 3 - 7)
                ok = 0;
        EXPECT(ok, "straddling read returns correct data from both hyperslab writes");
    }

    H5Sclose(mspace);
    H5Sclose(fspace);
    H5Dclose(ds);
    H5Fclose(fid);
} /* end check_wide() */

/* ------------------------------------------------------------------------
 * Scenario 2: a dataset written every 25th step, resolved correctly at
 * every step -- on the occurrence and in between.
 * ------------------------------------------------------------------------ */
#define GROUP_FILE  "t_reader_group.h5"
#define N_STEPS     100
#define STEP_EVERY  25

static int
write_group_steps(hid_t vol_id)
{
    hid_t fapl, fid, mesh;
    int   ret = -1;

    CHECK(fapl = H5Pcreate(H5P_FILE_ACCESS), "H5Pcreate(fapl)");
    CHECK(H5Pset_vol(fapl, vol_id, NULL), "H5Pset_vol");
    CHECK(fid = H5Fcreate(GROUP_FILE, H5F_ACC_TRUNC, H5P_DEFAULT, fapl), "H5Fcreate(group)");
    H5Pclose(fapl);

    CHECK(mesh = H5Gcreate2(fid, "mesh", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), "H5Gcreate2(mesh)");

    for (int s = 0; s < N_STEPS; s++) {
        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            printf("  FAIL  H5Fbegin_step at s=%d (%s:%d)\n", s, __FILE__, __LINE__);
            nerrors++;
            goto done;
        }

        if (s % STEP_EVERY == 0) {
            hid_t space, ds;
            int   val = s;

            CHECK(space = H5Screate(H5S_SCALAR), "H5Screate(field scalar)");
            CHECK(ds = H5Dcreate2(mesh, "field", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                  "H5Dcreate2(mesh/field)");
            CHECK(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val), "H5Dwrite(mesh/field)");
            H5Dclose(ds);
            H5Sclose(space);
        }

        if (H5Fend_step(fid) < 0) {
            printf("  FAIL  H5Fend_step at s=%d (%s:%d)\n", s, __FILE__, __LINE__);
            nerrors++;
            goto done;
        }
    }
    ret = 0;
done:
    H5Gclose(mesh);
    H5Fclose(fid);
    return ret;
} /* end write_group_steps() */

static void
check_group_steps(hid_t vol_id)
{
    hid_t fapl, fid;
    int   all_ok = 1;

    fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_vol(fapl, vol_id, NULL);
    fid = H5Fopen(GROUP_FILE, H5F_ACC_RDONLY, fapl);
    H5Pclose(fapl);
    if (fid < 0) {
        printf("  FAIL  H5Fopen(group)\n");
        nerrors++;
        return;
    }

    for (int k = 0; k < N_STEPS; k++) {
        hid_t grp = -1, ds;
        int   expected = STEP_EVERY * (k / STEP_EVERY);
        int   val      = -1;

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            printf("  FAIL  reader begin_step at k=%d (%s:%d)\n", k, __FILE__, __LINE__);
            nerrors++;
            all_ok = 0;
            break;
        }

        if (k % 2 == 0) {
            /* Nested open: exercises the READER_VIRTUAL group wrapper. */
            grp = H5Gopen2(fid, "/mesh", H5P_DEFAULT);
            ds  = (grp >= 0) ? H5Dopen2(grp, "field", H5P_DEFAULT) : -1;
        }
        else {
            /* One-shot multi-component open straight from the file. */
            ds = H5Dopen2(fid, "/mesh/field", H5P_DEFAULT);
        }

        if (ds < 0 || H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val) < 0 ||
            val != expected)
            all_ok = 0;

        if (ds >= 0)
            H5Dclose(ds);
        if (grp >= 0)
            H5Gclose(grp);
    }
    EXPECT(all_ok, "mesh/field resolves correctly at every step, on and between occurrences");

    H5Fclose(fid);
} /* end check_group_steps() */

/* ------------------------------------------------------------------------
 * Scenario 3: restart-overlap logical order, dev-plan.md's openPMD example.
 * ------------------------------------------------------------------------ */
#define RESTART_FILE  "t_reader_restart.h5"
#define ORIG_LO       0
#define ORIG_HI       750
#define ORIG_STEP     50
#define RESTART_LO    500
#define RESTART_MARK  100000

static int
write_one_logical_step(hid_t fid, uint64_t logical_id, int val)
{
    hid_t space, ds;

    CHECK(H5Fbegin_step(fid, 1, &logical_id, 0), "H5Fbegin_step(logical)");
    CHECK(space = H5Screate(H5S_SCALAR), "H5Screate(val scalar)");
    CHECK(ds = H5Dcreate2(fid, "val", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
          "H5Dcreate2(val)");
    CHECK(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val), "H5Dwrite(val)");
    H5Dclose(ds);
    H5Sclose(space);
    CHECK(H5Fend_step(fid), "H5Fend_step(logical)");

    return 0;
} /* end write_one_logical_step() */

static int
write_restart(hid_t vol_id)
{
    hid_t fapl, fid;
    int   ret = -1;

    CHECK(fapl = H5Pcreate(H5P_FILE_ACCESS), "H5Pcreate(fapl)");
    CHECK(H5Pset_vol(fapl, vol_id, NULL), "H5Pset_vol");
    CHECK(fid = H5Fcreate(RESTART_FILE, H5F_ACC_TRUNC, H5P_DEFAULT, fapl), "H5Fcreate(restart)");
    H5Pclose(fapl);

    /* Original run: logical ids 0, 50, ..., 750. */
    for (uint64_t id = ORIG_LO; id <= ORIG_HI; id += ORIG_STEP)
        if (write_one_logical_step(fid, id, (int)id) < 0)
            goto done;

    /* Restart from 500: logical ids 500, 550, ..., 750 again, at new
     * physical steps -- these must supersede the originals. */
    for (uint64_t id = RESTART_LO; id <= ORIG_HI; id += ORIG_STEP)
        if (write_one_logical_step(fid, id, (int)id + RESTART_MARK) < 0)
            goto done;

    ret = 0;
done:
    H5Fclose(fid);
    return ret;
} /* end write_restart() */

static void
check_restart(hid_t vol_id)
{
    hid_t     fapl, fid;
    size_t    n = 0;
    uint64_t *ids;

    fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_vol(fapl, vol_id, NULL);
    fid = H5Fopen(RESTART_FILE, H5F_ACC_RDONLY, fapl);
    H5Pclose(fapl);
    if (fid < 0) {
        printf("  FAIL  H5Fopen(restart)\n");
        nerrors++;
        return;
    }

    EXPECT(H5Fget_logical_steps(fid, &n, NULL) >= 0, "H5Fget_logical_steps size query");
    EXPECT(n == (ORIG_HI - ORIG_LO) / ORIG_STEP + 1, "16 deduped logical ids (restart ids not double-counted)");

    ids = (uint64_t *)malloc(n * sizeof(uint64_t));
    EXPECT(H5Fget_logical_steps(fid, &n, ids) >= 0, "H5Fget_logical_steps fill query");

    {
        int ascending = 1;
        for (size_t i = 1; i < n; i++)
            if (ids[i] <= ids[i - 1])
                ascending = 0;
        EXPECT(ascending, "logical ids returned ascending");
        EXPECT(n > 0 && ids[0] == ORIG_LO && ids[n - 1] == ORIG_HI, "range matches the openPMD example");
    }

    {
        int all_ok = 1;
        for (size_t i = 0; i < n; i++) {
            uint64_t id = ids[i];
            hid_t    ds;
            int      val      = -1;
            int      expected = (id >= RESTART_LO) ? (int)id + RESTART_MARK : (int)id;

            if (H5Fbegin_logical_step(fid, id) < 0) {
                all_ok = 0;
                continue;
            }
            ds = H5Dopen2(fid, "/val", H5P_DEFAULT);
            if (ds < 0 || H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val) < 0 ||
                val != expected)
                all_ok = 0;
            if (ds >= 0)
                H5Dclose(ds);
        }
        EXPECT(all_ok, "every logical id resolves to its authoritative (latest) occurrence");
    }
    free(ids);

    /* An id that never appears must be rejected, not silently land somewhere. */
    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
    EXPECT(H5Fbegin_logical_step(fid, 999999) < 0, "unknown logical id is rejected");
    H5Eset_auto2(H5E_DEFAULT, (H5E_auto2_t)H5Eprint2, stderr);

    H5Fclose(fid);
} /* end check_restart() */

/* ------------------------------------------------------------------------
 * Finding-#2 regression: an object built incrementally must still resolve
 * when read back in one shot via a leading-slash absolute path.
 * ------------------------------------------------------------------------ */
#define SLASH_FILE "t_reader_slash.h5"
#define SLASH_VAL  4242

static int
write_slash(hid_t vol_id)
{
    hid_t fapl, fid, grp, ds, space;
    int   val = SLASH_VAL;
    int   ret = -1;

    CHECK(fapl = H5Pcreate(H5P_FILE_ACCESS), "H5Pcreate(fapl)");
    CHECK(H5Pset_vol(fapl, vol_id, NULL), "H5Pset_vol");
    CHECK(fid = H5Fcreate(SLASH_FILE, H5F_ACC_TRUNC, H5P_DEFAULT, fapl), "H5Fcreate(slash)");
    H5Pclose(fapl);

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  H5Fbegin_step (%s:%d)\n", __FILE__, __LINE__);
        nerrors++;
        goto done;
    }

    /* Built incrementally, exactly like the natural H5Gcreate2()-then-
     * H5Dcreate2() idiom -- captured as "/g/sub". */
    CHECK(grp = H5Gcreate2(fid, "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), "H5Gcreate2(g)");
    CHECK(space = H5Screate(H5S_SCALAR), "H5Screate(slash scalar)");
    CHECK(ds = H5Dcreate2(grp, "sub", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
          "H5Dcreate2(g/sub)");
    CHECK(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val), "H5Dwrite(g/sub)");
    H5Dclose(ds);
    H5Sclose(space);
    H5Gclose(grp);

    if (H5Fend_step(fid) < 0) {
        printf("  FAIL  H5Fend_step (%s:%d)\n", __FILE__, __LINE__);
        nerrors++;
        goto done;
    }
    ret = 0;
done:
    H5Fclose(fid);
    return ret;
} /* end write_slash() */

static void
check_slash(hid_t vol_id)
{
    hid_t fapl, fid, ds;
    int   got = -1;

    fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_vol(fapl, vol_id, NULL);
    fid = H5Fopen(SLASH_FILE, H5F_ACC_RDONLY, fapl);
    H5Pclose(fapl);
    if (fid < 0) {
        printf("  FAIL  H5Fopen(slash)\n");
        nerrors++;
        return;
    }

    EXPECT(H5Fbegin_step(fid, 0, NULL, 0) >= 0, "reader begin_step lands on the slash-regression step");

    /* Read back in one shot with a leading-slash absolute path -- the exact
     * mismatch H5VL__stream_child_path()'s leading-slash strip fixed. */
    ds = H5Dopen2(fid, "/g/sub", H5P_DEFAULT);
    EXPECT(ds >= 0, "H5Dopen2(\"/g/sub\") resolves a leading-slash one-shot path");
    if (ds >= 0) {
        EXPECT(H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &got) >= 0 && got == SLASH_VAL,
               "leading-slash open returns the incrementally-written value");
        H5Dclose(ds);
    }

    H5Fclose(fid);
} /* end check_slash() */

int
main(void)
{
    hid_t vol_id;

    printf("vol-stream M3 exit gate: the decoupled reader\n");

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("  FAIL  H5VL_stream_register\n");
        return 1;
    }

    if (write_wide(vol_id) < 0) {
        printf("  FAIL  writing straddling-hyperslab scenario\n");
        return 1;
    }
    check_wide(vol_id);

    if (write_group_steps(vol_id) < 0) {
        printf("  FAIL  writing every-25th-step group scenario\n");
        return 1;
    }
    check_group_steps(vol_id);

    if (write_restart(vol_id) < 0) {
        printf("  FAIL  writing restart-overlap scenario\n");
        return 1;
    }
    check_restart(vol_id);

    if (write_slash(vol_id) < 0) {
        printf("  FAIL  writing leading-slash regression scenario\n");
        return 1;
    }
    check_slash(vol_id);

    H5VLclose(vol_id);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
} /* end main() */
