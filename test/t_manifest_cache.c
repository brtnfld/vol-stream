/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * dev-plan.md's residual risks: a DsetWrite entry's type_enc used to be
 * unconditionally re-H5Tencode()'d and re-persisted to /step/<n>/.manifest
 * on every single step, even though a dataset's type never changes across
 * steps. H5VL__stream_type_cache_lookup()/_upsert() (src/H5VLstream.c) now
 * let the writer omit an unchanged type_enc and the replay side resolve
 * the omission from its own mirror of the same cache.
 *
 * This is a black-box proof, not a white-box one: the cache itself is
 * private to the connector, so this test never inspects it directly.
 * Instead it re-creates and rewrites the same-named dataset ("d") across
 * three steps -- t_reader.c's own restart-scenario idiom (write_step()
 * there) for "the same logical object across physical steps", since a
 * placeholder from an earlier, already-replayed step cannot be reopened
 * by bare name mid-step -- and compares the *persisted* /step/<n>/.manifest
 * dataset's on-disk size, reopened natively (no connector) once the file
 * is closed:
 *
 *   step 0: DsetCreate + DsetWrite for "d" -- both entries carry a real,
 *           full type_enc (nothing to omit yet for either).
 *   step 1: DsetCreate (always full, unavoidable -- create entries are
 *           never cached) + DsetWrite for "d" again, same type -- the
 *           *write* entry's type_enc must be omitted this time, so this
 *           step's .manifest must still be strictly smaller than step 0's.
 *   step 2: same as step 1 -- must land at the same (already-minimal) size,
 *           proving the omission is stable across repeated steps, not a
 *           one-off fluke.
 *
 * Correctness (the data itself, not just size) is checked too: caching the
 * *encoding* must never change what actually lands in the file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NELEM 4

static int nerrors = 0;

#define EXPECT(cond, what)                                                                                    \
    do {                                                                                                      \
        if (!(cond)) {                                                                                        \
            printf("  FAIL  %s (%s:%d)\n", (what), __FILE__, __LINE__);                                       \
            nerrors++;                                                                                        \
        }                                                                                                      \
        else                                                                                                  \
            printf("  ok    %s\n", (what));                                                                   \
    } while (0)

#define CHECK(expr, what)                                                                                      \
    do {                                                                                                       \
        if ((expr) < 0) {                                                                                      \
            printf("  FAIL  %s (%s:%d)\n", (what), __FILE__, __LINE__);                                        \
            nerrors++;                                                                                         \
            return -1;                                                                                         \
        }                                                                                                       \
    } while (0)

static int
run(void)
{
    hid_t   vol_id, fapl, fid, sp;
    hsize_t dims[1] = {NELEM};
    int     s;

    CHECK((vol_id = H5VL_stream_register()), "register vol-stream");
    CHECK((fapl = H5Pcreate(H5P_FILE_ACCESS)), "create fapl");
    CHECK(H5Pset_vol(fapl, vol_id, NULL), "set vol on fapl");
    CHECK((fid = H5Fcreate("t_manifest_cache.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl)), "create file");
    CHECK((sp = H5Screate_simple(1, dims, NULL)), "create dataspace");

    for (s = 0; s < 3; s++) {
        int    buf[NELEM];
        int    i;
        hid_t  ds;

        for (i = 0; i < NELEM; i++)
            buf[i] = s * 100 + i;

        CHECK(H5Fbegin_step(fid, 0, NULL, 0), "begin_step");

        /* Re-create every step, same name -- t_reader.c's write_step()
         * idiom for "the same logical object, a new physical step each
         * time" (dev-plan.md decision 1, restart-safety). Each step gets
         * its own DsetCreate entry (never cached) and DsetWrite entry
         * (cached from the second occurrence of this path onward). */
        CHECK((ds = H5Dcreate2(fid, "d", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)),
              "create dataset");
        CHECK(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf), "write");
        CHECK(H5Dclose(ds), "close dataset");
        CHECK(H5Fend_step(fid), "end_step");
    }

    CHECK(H5Sclose(sp), "close dataspace");
    CHECK(H5Fclose(fid), "close file");
    CHECK(H5Pclose(fapl), "close fapl");
    H5VLclose(vol_id);

    /* Reopen natively: once replayed, every /step/<n>/ is an ordinary HDF5
     * tree, same pattern as t_deferred.c/t_vl_reject.c's own post-step
     * verification. */
    {
        hid_t    rid;
        hsize_t  manifest_size[3];
        int      rbuf[NELEM];

        CHECK((rid = H5Fopen("t_manifest_cache.h5", H5F_ACC_RDONLY, H5P_DEFAULT)), "reopen natively");

        for (s = 0; s < 3; s++) {
            char  path[48];
            hid_t mds;

            snprintf(path, sizeof(path), "/step/%d/.manifest", s);
            CHECK((mds = H5Dopen2(rid, path, H5P_DEFAULT)), "open .manifest dataset");
            if ((manifest_size[s] = H5Dget_storage_size(mds)) == 0) {
                printf("  FAIL  .manifest storage size (%s:%d)\n", __FILE__, __LINE__);
                nerrors++;
            }
            CHECK(H5Dclose(mds), "close .manifest dataset");
        }

        printf("  info  .manifest sizes: step0=%llu step1=%llu step2=%llu\n",
               (unsigned long long)manifest_size[0], (unsigned long long)manifest_size[1],
               (unsigned long long)manifest_size[2]);

        EXPECT(manifest_size[1] < manifest_size[0],
               "step 1's manifest (omitted type_enc) is smaller than step 0's (create + first write)");
        EXPECT(manifest_size[2] == manifest_size[1],
               "step 2's manifest matches step 1's -- omission is stable, not a one-off");

        for (s = 0; s < 3; s++) {
            char  path[48];
            hid_t rds;
            int   i, ok = 1;

            snprintf(path, sizeof(path), "/step/%d/d", s);
            CHECK((rds = H5Dopen2(rid, path, H5P_DEFAULT)), "open replayed dataset");
            CHECK(H5Dread(rds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rbuf), "read replayed data");
            for (i = 0; i < NELEM; i++)
                if (rbuf[i] != s * 100 + i)
                    ok = 0;
            EXPECT(ok, "replayed data is correct despite the omitted/cached type_enc");
            CHECK(H5Dclose(rds), "close replayed dataset");
        }

        CHECK(H5Fclose(rid), "close native reopen");
    }

    return 0;
} /* end run() */

int
main(void)
{
    if (run() < 0)
        return 1;

    if (nerrors > 0) {
        printf("FAILED with %d error(s)\n", nerrors);
        return 1;
    }

    printf("PASSED\n");
    return 0;
} /* end main() */
