/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Reference support end to end (docs/dev-plan.md Decision #4): a dataset of
 * H5R references survives capture and replay, and the replayed references
 * still resolve to the right objects.
 *
 * Why an ordinary memcpy capture is safe here, when it looked like it
 * shouldn't be: an H5R_ref_t is opaque and can own heap memory in general
 * (a filename string, for a reference into another file), so the original
 * guard rejected every reference outright rather than risk a dangling
 * pointer on replay. Two separate attempts at a name-translation capture
 * path -- resolve the target's name via H5Rget_obj_name() at capture time,
 * or cache token -> logical path when H5Rcreate_object() first resolves it
 * and translate at capture without any HDF5 call -- both built cleanly and
 * both crashed identically deep in end_step()'s replay. What actually
 * unblocked this: for a *same-file* reference (H5R_OBJECT2/
 * H5R_DATASET_REGION2/H5R_ATTR, the only kind reachable through this
 * connector's own API -- H5Rcreate_object() constructs a reference relative
 * to its loc_id's own file, so there is no way to name a genuinely foreign
 * file without a loc_id in THAT file, which would not route through this
 * connector at all), the cached filename pointer is NULL -- confirmed by
 * reading the raw bytes against HDF5's own (private) H5R_ref_priv_obj_t
 * layout. Nothing to dangle, so plain memcpy is exactly as safe as for any
 * other fixed-size type, and simpler than either translation attempt: no
 * serialization, no replay-side reconstruction, nothing new in the wire
 * format. See dev-plan.md for the fuller account of both abandoned
 * attempts, kept there because the failure mode is not obvious.
 *
 * The checks:
 *   1. Writing a reference dataset succeeds -- it used to be rejected
 *      outright by H5VL__stream_type_unsafe_to_capture().
 *   2. After replay, opening the referenced object through the stored
 *      reference lands on the *correct target*, verified by reading its
 *      contents back and comparing values -- not merely by the open
 *      succeeding, which a reference to the wrong object would also do.
 *   3. Two references to two different targets stay distinct, so this
 *      cannot pass by accidentally pointing everything at one object.
 *   4. A reference NESTED inside a compound type is still rejected: the
 *      exemption above only applies to a reference at the top level of the
 *      write, not one at some byte offset inside a struct, which nothing
 *      here rewrites member by member.
 *
 * Targets come from an earlier, already-committed step. That restriction is
 * real and deliberate -- see test/t_ref_path.c, which asserts the same-step
 * case is refused, and dev-plan.md for why deferred writes make it so.
 *
 * Single process, no transport: this is about capture/replay fidelity.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_ref_roundtrip.h5"
#define NREFS 2

static int
make_target(hid_t fid, const char *name, int value)
{
    hid_t   space, ds;
    hsize_t one = 1;

    if ((space = H5Screate_simple(1, &one, NULL)) < 0)
        return -1;
    if ((ds = H5Dcreate2(fid, name, H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        H5Sclose(space);
        return -1;
    }
    if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value) < 0) {
        H5Dclose(ds);
        H5Sclose(space);
        return -1;
    }
    H5Dclose(ds);
    H5Sclose(space);
    return 0;
}

int
main(void)
{
    hid_t     vol_id, fapl, fid, refspace, refds;
    hsize_t   nrefs = NREFS;
    H5R_ref_t refs[NREFS];
    int       nerrors = 0;
    int       i;

    printf("vol-stream: H5R capture/replay round trip\n");

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

    /* --- Step 0: two distinct targets, committed. --- */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || make_target(fid, "/alpha", 111) < 0 ||
        make_target(fid, "/beta", 222) < 0 || H5Fend_step(fid) < 0) {
        printf("  FAIL  step 0 (targets)\n");
        return 1;
    }

    /* --- Step 1: a dataset of references to them. --- */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  begin_step 1\n");
        return 1;
    }

    memset(refs, 0, sizeof(refs));
    if (H5Rcreate_object(fid, "/alpha", H5P_DEFAULT, &refs[0]) < 0 ||
        H5Rcreate_object(fid, "/beta", H5P_DEFAULT, &refs[1]) < 0) {
        printf("  FAIL  H5Rcreate_object\n");
        return 1;
    }

    if ((refspace = H5Screate_simple(1, &nrefs, NULL)) < 0 ||
        (refds = H5Dcreate2(fid, "/refs", H5T_STD_REF, refspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) <
            0) {
        printf("  FAIL  create /refs\n");
        return 1;
    }
    if (H5Dwrite(refds, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, refs) < 0) {
        printf("  FAIL  writing a reference dataset was rejected\n");
        nerrors++;
    }
    else
        printf("  ok    reference dataset accepted for capture\n");

    H5Dclose(refds);
    H5Sclose(refspace);
    for (i = 0; i < NREFS; i++)
        H5Rdestroy(&refs[i]);

    /* end_step() is where capture-time names become replayed references. */
    if (H5Fend_step(fid) < 0) {
        printf("  FAIL  end_step 1 (reference replay)\n");
        nerrors++;
    }
    else
        printf("  ok    step committed, references replayed\n");

    /* --- A reference nested inside a compound must still be rejected: the
     * top-level exemption above does not extend to one buried at some byte
     * offset inside a struct. A fresh step, on the same still-open file, so
     * this cannot be mistaken for the same-step restriction t_ref_path.c
     * already covers. --- */
    {
        hid_t     cspace, cds, ctype;
        hsize_t   one = 1;
        H5R_ref_t cref;

        typedef struct {
            int       tag;
            H5R_ref_t ref;
        } compound_ref_t;
        compound_ref_t crec;

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || H5Rcreate_object(fid, "/alpha", H5P_DEFAULT, &cref) < 0) {
            printf("  FAIL  set up nested-reference check\n");
            nerrors++;
        }
        else {
            crec.tag = 1;
            crec.ref = cref;

            ctype = H5Tcreate(H5T_COMPOUND, sizeof(compound_ref_t));
            H5Tinsert(ctype, "tag", HOFFSET(compound_ref_t, tag), H5T_NATIVE_INT);
            H5Tinsert(ctype, "ref", HOFFSET(compound_ref_t, ref), H5T_STD_REF);

            cspace = H5Screate_simple(1, &one, NULL);
            cds    = H5Dcreate2(fid, "/compound_ref", ctype, cspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            if (cds < 0) {
                printf("  FAIL  create /compound_ref\n");
                nerrors++;
            }
            else {
                H5E_BEGIN_TRY
                {
                    if (H5Dwrite(cds, ctype, H5S_ALL, H5S_ALL, H5P_DEFAULT, &crec) >= 0) {
                        printf("  FAIL  a reference nested inside a compound was accepted; it is not "
                               "translated and must stay rejected\n");
                        nerrors++;
                    }
                    else
                        printf("  ok    a reference nested inside a compound is still rejected\n");
                }
                H5E_END_TRY
                H5Dclose(cds);
            }
            H5Sclose(cspace);
            H5Tclose(ctype);
            H5Rdestroy(&cref);
        }
        H5Fend_step(fid);
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    /* --- Verify against the underlying file directly, with the native
     * connector: the replayed references must resolve to the right objects.
     * Reading through the stream connector would prove less -- this checks
     * what actually landed on disk. --- */
    {
        hid_t     nfid, nrefds;
        H5R_ref_t back[NREFS];
        int       expected[NREFS] = {111, 222};

        if ((nfid = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
            printf("  FAIL  reopen with the native connector\n");
            return 1;
        }
        if ((nrefds = H5Dopen2(nfid, "/step/1/refs", H5P_DEFAULT)) < 0) {
            printf("  FAIL  /step/1/refs is not in the file\n");
            H5Fclose(nfid);
            return 1;
        }

        memset(back, 0, sizeof(back));
        if (H5Dread(nrefds, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, back) < 0) {
            printf("  FAIL  reading back the reference dataset\n");
            nerrors++;
        }
        else {
            for (i = 0; i < NREFS; i++) {
                hid_t tds;
                int   val = 0;

                if ((tds = H5Ropen_object(&back[i], H5P_DEFAULT, H5P_DEFAULT)) < 0) {
                    printf("  FAIL  reference %d does not resolve\n", i);
                    nerrors++;
                    continue;
                }
                if (H5Dread(tds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val) < 0) {
                    printf("  FAIL  reading through reference %d\n", i);
                    nerrors++;
                }
                else if (val != expected[i]) {
                    /* The decisive check: a reference pointing at the wrong
                     * object would still open fine, just yield the other
                     * target's value. */
                    printf("  FAIL  reference %d resolved to an object holding %d, expected %d\n", i, val,
                           expected[i]);
                    nerrors++;
                }
                else
                    printf("  ok    reference %d resolves to its own target (value %d)\n", i, val);

                H5Dclose(tds);
                H5Rdestroy(&back[i]);
            }
        }
        H5Dclose(nrefds);
        H5Fclose(nfid);
    }

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
