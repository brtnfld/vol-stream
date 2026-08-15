/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Capture-time reject guard for buffers that are not self-contained.
 *
 * A DsetWrite/Attr capture inside an open step memcpy()s whatever buffer
 * H5Dwrite()/H5Awrite() was given straight into the pending entry's
 * payload, replayed later via a *different* call once end_step() runs.
 * That is only correct for buffers whose bytes stand alone. Where they do
 * not -- a variable-length datatype is an array of {len, pointer} structs,
 * pointing at memory the caller may reclaim the moment the write returns --
 * capturing verbatim would read through stale pointers at replay time.
 *
 * **Scope note:** top-level VL types are no longer rejected. They are now
 * deep-serialized at capture instead (H5VL__stream_vl_serialize(), covered
 * end to end by test/t_vl_roundtrip.c), which is what Decision #4 always
 * intended; this test asserted the interim reject and has been narrowed
 * accordingly. What it still guards is the part that genuinely remains
 * unhandled: a variable-length type *nested* inside a compound, where
 * deep-serializing would mean rewriting the buffer member by member. That
 * case must keep failing loudly rather than silently corrupting, and a
 * plain (non-VL) write in the same step must be unaffected either way.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

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
    hid_t   vol_id, fapl, fid, sp, scalar, ds, plain_ds, vlstr_type, vlseq_type;
    hid_t   attr, plain_attr;
    hsize_t dims[1] = {4};
    int     plain_buf[4] = {1, 2, 3, 4};
    const char *vlstr_buf[4];
    hvl_t       vlseq_buf[4];
    int         i;

    for (i = 0; i < 4; i++) {
        vlstr_buf[i]     = "unsafe";
        vlseq_buf[i].len = 0;
        vlseq_buf[i].p   = NULL;
    }

    CHECK((vol_id = H5VL_stream_register()), "register vol-stream");
    CHECK((fapl = H5Pcreate(H5P_FILE_ACCESS)), "create fapl");
    CHECK(H5Pset_vol(fapl, vol_id, NULL), "set vol on fapl");
    CHECK((fid = H5Fcreate("t_vl_reject.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl)), "create file");

    CHECK((sp = H5Screate_simple(1, dims, NULL)), "create dataspace");
    CHECK((scalar = H5Screate(H5S_SCALAR)), "create scalar dataspace");
    CHECK((vlstr_type = H5Tcopy(H5T_C_S1)), "copy string type");
    CHECK(H5Tset_size(vlstr_type, H5T_VARIABLE), "make variable-length string");
    CHECK((vlseq_type = H5Tvlen_create(H5T_NATIVE_INT)), "create VL sequence type");

    CHECK(H5Fbegin_step(fid, 0, NULL, 0), "begin_step");

    /* Top-level VL is deep-serialized now, not rejected -- asserted here so
     * a regression back to rejecting would be caught by this test too, not
     * only by t_vl_roundtrip.c. */
    CHECK((ds = H5Dcreate2(fid, "vlstr", vlstr_type, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)),
          "create VL-string dataset (deferred placeholder)");
    EXPECT(H5Dwrite(ds, vlstr_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, vlstr_buf) >= 0,
           "top-level VL-string dataset write is accepted (deep-serialized)");
    H5Dclose(ds);

    CHECK((ds = H5Dcreate2(fid, "vlseq", vlseq_type, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)),
          "create VL-sequence dataset (deferred placeholder)");
    EXPECT(H5Dwrite(ds, vlseq_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, vlseq_buf) >= 0,
           "top-level VL-sequence dataset write is accepted (deep-serialized)");
    H5Dclose(ds);

    CHECK((attr = H5Acreate2(fid, "vlattr", vlstr_type, scalar, H5P_DEFAULT, H5P_DEFAULT)),
          "create VL-string attribute (deferred placeholder)");
    EXPECT(H5Awrite(attr, vlstr_type, vlstr_buf) >= 0,
           "top-level VL-string attribute write is accepted (deep-serialized)");
    H5Aclose(attr);

    /* The case that genuinely remains unhandled, and so must still fail
     * loudly: a VL buried at a byte offset inside a compound. */
    {
        typedef struct {
            int   tag;
            hvl_t v;
        } compound_vl_t;
        compound_vl_t crec;
        hid_t         ctype, cds;

        crec.tag   = 1;
        crec.v.len = 0;
        crec.v.p   = NULL;

        CHECK((ctype = H5Tcreate(H5T_COMPOUND, sizeof(compound_vl_t))), "create compound type");
        CHECK(H5Tinsert(ctype, "tag", HOFFSET(compound_vl_t, tag), H5T_NATIVE_INT), "insert tag member");
        CHECK(H5Tinsert(ctype, "v", HOFFSET(compound_vl_t, v), vlseq_type), "insert nested VL member");

        CHECK((cds = H5Dcreate2(fid, "compound_vl", ctype, scalar, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)),
              "create compound-with-VL dataset (deferred placeholder)");
        H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
        EXPECT(H5Dwrite(cds, ctype, H5S_ALL, H5S_ALL, H5P_DEFAULT, &crec) < 0,
               "a VL nested inside a compound is still rejected, not silently captured");
        H5Eset_auto2(H5E_DEFAULT, (H5E_auto2_t)H5Eprint2, stderr);
        H5Dclose(cds);
        H5Tclose(ctype);
    }

    /* A plain (non-VL) dataset and attribute write in the same step must be
     * completely unaffected by the rejections above. */
    CHECK((plain_ds = H5Dcreate2(fid, "plain", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)),
          "create plain dataset (deferred placeholder)");
    EXPECT(H5Dwrite(plain_ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, plain_buf) >= 0,
           "plain dataset write in the same step still succeeds");
    H5Dclose(plain_ds);

    CHECK((plain_attr = H5Acreate2(fid, "plainattr", H5T_NATIVE_INT, scalar, H5P_DEFAULT, H5P_DEFAULT)),
          "create plain attribute (deferred placeholder)");
    EXPECT(H5Awrite(plain_attr, H5T_NATIVE_INT, plain_buf) >= 0,
           "plain attribute write in the same step still succeeds");
    H5Aclose(plain_attr);

    CHECK(H5Fend_step(fid), "end_step");

    H5Tclose(vlstr_type);
    H5Tclose(vlseq_type);
    H5Sclose(sp);
    H5Sclose(scalar);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    /* The plain dataset must have replayed correctly under /step/0/ -- the
     * rejections above must not have poisoned the rest of the step. Reopen
     * natively (no connector): once replayed, this is an ordinary HDF5
     * file, same pattern as t_deferred.c's own post-step verification. */
    {
        hid_t rid, rds;
        int   rbuf[4] = {0};

        CHECK((rid = H5Fopen("t_vl_reject.h5", H5F_ACC_RDONLY, H5P_DEFAULT)), "reopen natively");
        CHECK((rds = H5Dopen2(rid, "/step/0/plain", H5P_DEFAULT)), "open replayed plain dataset");
        CHECK(H5Dread(rds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, rbuf), "read plain dataset back");
        EXPECT(memcmp(rbuf, plain_buf, sizeof(plain_buf)) == 0,
               "plain dataset's data is correct after the step commits");
        H5Dclose(rds);
        H5Fclose(rid);
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
