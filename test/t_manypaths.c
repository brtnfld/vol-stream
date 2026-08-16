/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Enough distinct object paths to exercise the path -> index hash map.
 *
 * fs->path_index and the two type caches are keyed by object path. They used
 * to be strcmp() scans of a flat array; they are now backed by
 * H5VL_stream_pathmap_t. Nothing else in the suite comes close to the number
 * of distinct paths needed to make that map do anything interesting -- most
 * tests use a handful, so the map never grows past its initial capacity and
 * never sees a collision. This does: NPATHS entries force several
 * doublings/rehashes, and the paths deliberately share a long common prefix
 * ("/mesh/blockNNNN/field/rho"), which is the input shape a weak hash
 * degenerates on.
 *
 * Two details are what make this able to fail, and both were missing from a
 * first version that passed happily against a deliberately broken map:
 *
 *   1. It reads back through the CONNECTOR'S READER MODE (H5F_ACC_RDONLY +
 *      H5Fbegin_step + a bare logical path), not natively at the literal
 *      "/step/<n>/..." location. fs->path_index is consulted only by reader
 *      resolution, so a native read-back exercises the map's insert side and
 *      nothing else -- a lookup returning the wrong index sailed straight
 *      through.
 *
 *   2. The datatype VARIES per path. With one datatype everywhere, a
 *      type-cache lookup landing on another path's entry returns
 *      byte-identical H5Tencode() output, so the omit-if-unchanged
 *      optimization behaves the same whether the lookup was right or wrong.
 *      Alternating int/double/short makes a wrong hit produce a wrong type on
 *      replay.
 *
 *   3. Each path is written in only SOME steps, not all of them. This is the
 *      one that actually bites. With every path written in every step, every
 *      path's step list is identical ({0,1,2,3}), so a lookup returning some
 *      OTHER path's entry still yields the right answer -- the lists are
 *      interchangeable. A second broken-map run passed for exactly that
 *      reason. Writing path p only in steps where (s + p) is even makes the
 *      lists differ per path, so resolution has to land on the right entry:
 *      "the largest step <= current that actually has this path" is only
 *      correct if that path's own list was consulted.
 *
 * A lookup that wrongly misses creates a *second* path_index entry for a path
 * that already has one, splitting its step list so a reader sees only some of
 * its steps. A lookup that wrongly hits returns another path's steps, and with
 * per-path step sets that resolves to the wrong version of the object -- a
 * stale value, or an open that fails outright.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME  "t_manypaths.h5"
#define NPATHS 200 /* several map growths past the initial capacity of 16 */
#define NSTEPS 4
#define NELEM  4

/* Distinct per (step, path, element) and small enough to be exact in every
 * type below -- H5T_NATIVE_SHORT is the binding one, so values must stay well
 * inside 16 bits or they saturate and the comparison fails for reasons that
 * have nothing to do with what is being tested. s*1000 dominates, p*4+i is
 * unique for p < 250. */
static int
expect(int s, int p, int i)
{
    return s * 1000 + p * 4 + i;
}

/* Three different file datatypes, cycled by path. A type-cache lookup that
 * lands on the wrong entry then reproduces the wrong type on replay, which a
 * single-datatype workload cannot reveal (identical H5Tencode() bytes). */
static hid_t
ftype_for(int p)
{
    switch (p % 3) {
        case 0:
            return H5T_NATIVE_INT;
        case 1:
            return H5T_NATIVE_DOUBLE;
        default:
            return H5T_NATIVE_SHORT;
    }
}

int
main(void)
{
    hid_t   vol, fapl, fid, sp;
    hsize_t dims[1] = {NELEM};
    int     buf[NELEM];
    int     s, p, i;
    int     nerrors = 0;
    char    name[64];

    if ((vol = H5VL_stream_register()) < 0) {
        printf("FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol, NULL) < 0) {
        printf("FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("FAIL create\n");
        return 1;
    }
    if ((sp = H5Screate_simple(1, dims, NULL)) < 0) {
        printf("FAIL dataspace\n");
        return 1;
    }

    for (s = 0; s < NSTEPS; s++) {
        const uint64_t lid = (uint64_t)s;

        if (H5Fbegin_step(fid, 1, &lid, 0) < 0) {
            printf("FAIL begin_step %d\n", s);
            return 1;
        }
        for (p = 0; p < NPATHS; p++) {
            hid_t ds;

            /* Per-path step sets -- see point 3 in this file's comment. */
            if (((s + p) % 2) != 0)
                continue;

            snprintf(name, sizeof(name), "/mesh/block%04d/field/rho", p);
            for (i = 0; i < NELEM; i++)
                buf[i] = expect(s, p, i);

            if ((ds = H5Dcreate2(fid, name, ftype_for(p), sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) <
                0) {
                printf("FAIL create %s in step %d\n", name, s);
                return 1;
            }
            if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) < 0) {
                printf("FAIL write %s in step %d\n", name, s);
                return 1;
            }
            H5Dclose(ds);
        }
        if (H5Fend_step(fid) < 0) {
            printf("FAIL end_step %d\n", s);
            return 1;
        }
    }

    H5Sclose(sp);
    H5Fclose(fid);
    H5Pclose(fapl);

    /* Read back through the connector's READER mode, so path resolution --
     * the map's actual consumer -- is what is being tested. Each step is
     * visited with H5Fbegin_step() and every path opened by its BARE logical
     * name, which only resolves correctly if that path's step list is intact
     * and belongs to it. */
    {
        hid_t rfapl, rfid;

        if ((rfapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(rfapl, vol, NULL) < 0) {
            printf("FAIL reader fapl\n");
            return 1;
        }
        if ((rfid = H5Fopen(FNAME, H5F_ACC_RDONLY, rfapl)) < 0) {
            printf("FAIL reopen through the connector\n");
            return 1;
        }

        for (s = 0; s < NSTEPS && nerrors < 5; s++) {
            if (H5Fbegin_step(rfid, 0, NULL, 0) < 0) {
                printf("FAIL reader begin_step %d\n", s);
                return 1;
            }

            for (p = 0; p < NPATHS && nerrors < 5; p++) {
                hid_t ds;
                int   got[NELEM];
                int   src = -1, k;

                /* The step the reader must resolve to: the most recent one at
                 * or before s that actually wrote this path. */
                for (k = s; k >= 0; k--)
                    if (((k + p) % 2) == 0) {
                        src = k;
                        break;
                    }

                snprintf(name, sizeof(name), "/mesh/block%04d/field/rho", p);

                if (src < 0) {
                    /* Never written at or before this step -- must NOT resolve. */
                    hid_t bad;

                    H5E_BEGIN_TRY
                    {
                        bad = H5Dopen2(rfid, name, H5P_DEFAULT);
                    }
                    H5E_END_TRY
                    if (bad >= 0) {
                        printf("FAIL %s resolved at step %d but was never written by then\n", name, s);
                        nerrors++;
                        H5Dclose(bad);
                    }
                    continue;
                }

                if ((ds = H5Dopen2(rfid, name, H5P_DEFAULT)) < 0) {
                    printf("FAIL %s unresolvable at step %d -- a path-index lookup lost an entry\n", name,
                           s);
                    nerrors++;
                    continue;
                }
                /* Read as int regardless of the file type: HDF5 converts, so a
                 * wrong type on replay shows up as a wrong value here. */
                if (H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) < 0) {
                    printf("FAIL read %s at step %d\n", name, s);
                    nerrors++;
                    H5Dclose(ds);
                    continue;
                }
                H5Dclose(ds);

                for (i = 0; i < NELEM; i++)
                    if (got[i] != expect(src, p, i)) {
                        printf("FAIL %s[%d] at step %d: expected %d (from step %d), got %d -- resolution "
                               "landed on another path's step list\n",
                               name, i, s, expect(src, p, i), src, got[i]);
                        nerrors++;
                        break;
                    }
            }
        }

        H5Fclose(rfid);
        H5Pclose(rfapl);
    }

    if (nerrors) {
        printf("t_manypaths: FAILED\n");
        return 1;
    }
    printf("t_manypaths: %d paths x %d steps round-tripped\n", NPATHS, NSTEPS);
    return 0;
}
