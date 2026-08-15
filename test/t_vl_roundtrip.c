/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Variable-length deep serialization (docs/dev-plan.md Decision #4's "v1
 * covers ... variable-length data via deep serialization") -- the last piece
 * of that decision to be built, and the counterpart to test/t_vl_reject.c,
 * which asserted the *interim* behavior of refusing VL writes outright.
 *
 * Why VL cannot be captured like ordinary data: a VL buffer is not
 * self-contained. An H5T_VLEN write hands over an array of hvl_t
 * {len, pointer}, and a VL-string write an array of char *, with the actual
 * bytes wherever the application put them. This connector defers the real
 * write to end_step(), by which time the application is entitled to have
 * reclaimed all of it -- so capture must follow those pointers immediately
 * and copy what they point at (H5VL__stream_vl_serialize()), and replay must
 * rebuild real pointers into fresh allocations before writing
 * (H5VL__stream_vl_deserialize()). Capturing the pointers verbatim is the
 * silent-corruption bug the reject guard was originally added to prevent.
 *
 * What is asserted:
 *   1. VL-sequence and VL-string writes are accepted, where they were
 *      previously rejected.
 *   2. Both survive the deferred round trip with exact contents -- checked
 *      against the underlying file through the *native* connector, so this
 *      tests what actually landed rather than trusting the stream path to
 *      report on itself.
 *   3. Ragged lengths are preserved per element (0, 1, and many), which is
 *      the whole point of variable-length storage and the thing a
 *      fixed-size-assumption bug would break.
 *   4. The application's buffers are freed *before* end_step() runs. This is
 *      the load-bearing case: if capture had stored pointers rather than
 *      bytes, replay would read freed memory, and this test would be the
 *      one to catch it.
 *   5. A VL nested inside a compound is still rejected -- deep-serializing
 *      one at a byte offset inside a struct would mean rewriting the buffer
 *      member by member, which this increment does not do.
 *
 * Single process, no transport: this is about capture/replay fidelity.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_vl_roundtrip.h5"
#define NSEQ  4

/* Deliberately ragged, including an empty sequence. */
static const int seq_lens[NSEQ] = {3, 0, 1, 5};

static const char *const strs[NSEQ] = {"alpha", "", "a-considerably-longer-string-value", "z"};

int
main(void)
{
    hid_t   vol_id, fapl, fid, space, vlen_type, str_type, ds_seq, ds_str;
    hsize_t dims = NSEQ;
    hvl_t   seq[NSEQ];
    char   *strbuf[NSEQ];
    int     nerrors = 0;
    int     i, j;

    printf("vol-stream: variable-length deep serialization round trip\n");

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

    if ((vlen_type = H5Tvlen_create(H5T_NATIVE_INT)) < 0 || (str_type = H5Tcopy(H5T_C_S1)) < 0 ||
        H5Tset_size(str_type, H5T_VARIABLE) < 0 || (space = H5Screate_simple(1, &dims, NULL)) < 0) {
        printf("  FAIL  build VL types\n");
        return 1;
    }

    /* Heap-allocated on purpose: everything here is freed before end_step()
     * so replay cannot be reading the application's memory. */
    for (i = 0; i < NSEQ; i++) {
        seq[i].len = (size_t)seq_lens[i];
        seq[i].p   = seq_lens[i] ? malloc((size_t)seq_lens[i] * sizeof(int)) : NULL;
        for (j = 0; j < seq_lens[i]; j++)
            ((int *)seq[i].p)[j] = i * 100 + j;

        strbuf[i] = strdup(strs[i]);
    }

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  begin_step\n");
        return 1;
    }

    if ((ds_seq = H5Dcreate2(fid, "/vlseq", vlen_type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        printf("  FAIL  create /vlseq\n");
        return 1;
    }
    if (H5Dwrite(ds_seq, vlen_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, seq) < 0) {
        printf("  FAIL  VL-sequence write was rejected\n");
        nerrors++;
    }
    else
        printf("  ok    VL-sequence write accepted for capture\n");
    H5Dclose(ds_seq);

    if ((ds_str = H5Dcreate2(fid, "/vlstr", str_type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        printf("  FAIL  create /vlstr\n");
        return 1;
    }
    if (H5Dwrite(ds_str, str_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, strbuf) < 0) {
        printf("  FAIL  VL-string write was rejected\n");
        nerrors++;
    }
    else
        printf("  ok    VL-string write accepted for capture\n");
    H5Dclose(ds_str);

    /* A VL buried inside a compound stays rejected. */
    {
        typedef struct {
            int   tag;
            hvl_t v;
        } compound_vl_t;
        compound_vl_t crec;
        hid_t         ctype, cspace, cds;
        hsize_t       one = 1;

        crec.tag   = 1;
        crec.v.len = 0;
        crec.v.p   = NULL;

        ctype = H5Tcreate(H5T_COMPOUND, sizeof(compound_vl_t));
        H5Tinsert(ctype, "tag", HOFFSET(compound_vl_t, tag), H5T_NATIVE_INT);
        H5Tinsert(ctype, "v", HOFFSET(compound_vl_t, v), vlen_type);

        cspace = H5Screate_simple(1, &one, NULL);
        cds    = H5Dcreate2(fid, "/compound_vl", ctype, cspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (cds < 0) {
            printf("  FAIL  create /compound_vl\n");
            nerrors++;
        }
        else {
            H5E_BEGIN_TRY
            {
                if (H5Dwrite(cds, ctype, H5S_ALL, H5S_ALL, H5P_DEFAULT, &crec) >= 0) {
                    printf("  FAIL  a VL nested inside a compound was accepted; it is not deep-serialized "
                           "and must stay rejected\n");
                    nerrors++;
                }
                else
                    printf("  ok    a VL nested inside a compound is still rejected\n");
            }
            H5E_END_TRY
            H5Dclose(cds);
        }
        H5Sclose(cspace);
        H5Tclose(ctype);
    }

    /* THE point of the test: drop every application buffer before the real
     * write happens. Anything that captured pointers instead of bytes is
     * now holding freed memory. */
    for (i = 0; i < NSEQ; i++) {
        free(seq[i].p);
        seq[i].p   = NULL;
        seq[i].len = 0;
        free(strbuf[i]);
        strbuf[i] = NULL;
    }

    if (H5Fend_step(fid) < 0) {
        printf("  FAIL  end_step (VL replay)\n");
        nerrors++;
    }
    else
        printf("  ok    step committed after the source buffers were freed\n");

    H5Sclose(space);
    H5Tclose(vlen_type);
    H5Tclose(str_type);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    /* --- Verify what actually landed, through the native connector. --- */
    {
        hid_t nfid, nds, ntype, nspace;
        hvl_t back_seq[NSEQ];
        char *back_str[NSEQ];

        if ((nfid = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
            printf("  FAIL  reopen with the native connector\n");
            return 1;
        }

        /* VL sequences. */
        if ((nds = H5Dopen2(nfid, "/step/0/vlseq", H5P_DEFAULT)) < 0) {
            printf("  FAIL  /step/0/vlseq is not in the file\n");
            nerrors++;
        }
        else {
            ntype  = H5Tvlen_create(H5T_NATIVE_INT);
            nspace = H5Dget_space(nds);
            memset(back_seq, 0, sizeof(back_seq));

            if (H5Dread(nds, ntype, H5S_ALL, H5S_ALL, H5P_DEFAULT, back_seq) < 0) {
                printf("  FAIL  reading back /vlseq\n");
                nerrors++;
            }
            else {
                int ok = 1;

                for (i = 0; i < NSEQ && ok; i++) {
                    if ((int)back_seq[i].len != seq_lens[i]) {
                        printf("  FAIL  /vlseq[%d] length is %d, expected %d (ragged lengths not "
                               "preserved)\n",
                               i, (int)back_seq[i].len, seq_lens[i]);
                        ok = 0;
                        break;
                    }
                    for (j = 0; j < seq_lens[i]; j++)
                        if (((int *)back_seq[i].p)[j] != i * 100 + j) {
                            printf("  FAIL  /vlseq[%d][%d] = %d, expected %d\n", i, j,
                                   ((int *)back_seq[i].p)[j], i * 100 + j);
                            ok = 0;
                            break;
                        }
                }
                if (ok)
                    printf("  ok    /vlseq round-tripped exactly, ragged lengths intact\n");
                else
                    nerrors++;
                H5Treclaim(ntype, nspace, H5P_DEFAULT, back_seq);
            }
            H5Sclose(nspace);
            H5Tclose(ntype);
            H5Dclose(nds);
        }

        /* VL strings. */
        if ((nds = H5Dopen2(nfid, "/step/0/vlstr", H5P_DEFAULT)) < 0) {
            printf("  FAIL  /step/0/vlstr is not in the file\n");
            nerrors++;
        }
        else {
            ntype = H5Tcopy(H5T_C_S1);
            H5Tset_size(ntype, H5T_VARIABLE);
            nspace = H5Dget_space(nds);
            memset(back_str, 0, sizeof(back_str));

            if (H5Dread(nds, ntype, H5S_ALL, H5S_ALL, H5P_DEFAULT, back_str) < 0) {
                printf("  FAIL  reading back /vlstr\n");
                nerrors++;
            }
            else {
                int ok = 1;

                for (i = 0; i < NSEQ; i++)
                    if (!back_str[i] || strcmp(back_str[i], strs[i]) != 0) {
                        printf("  FAIL  /vlstr[%d] = '%s', expected '%s'\n", i,
                               back_str[i] ? back_str[i] : "(null)", strs[i]);
                        ok = 0;
                        break;
                    }
                if (ok)
                    printf("  ok    /vlstr round-tripped exactly, including the empty string\n");
                else
                    nerrors++;
                H5Treclaim(ntype, nspace, H5P_DEFAULT, back_str);
            }
            H5Sclose(nspace);
            H5Tclose(ntype);
            H5Dclose(nds);
        }

        H5Fclose(nfid);
    }

    /* --- The wire form's length tag must be little-endian on every host,
     * not the host's own byte order. A file written here has to be readable
     * on a machine of the opposite endianness, and this is the one
     * hand-rolled binary field in the manifest (FlatBuffers is LE by spec;
     * the H5Tencode/H5Sencode2 blobs are HDF5's portable encodings). A raw
     * memcpy of a uint64_t passes every round-trip check above -- it only
     * fails across hosts -- so the byte order has to be asserted directly,
     * by reading the stored payload back and inspecting it. --- */
    {
        hid_t    nfid, pds;
        uint8_t *raw;
        hsize_t  plen;
        int      found_first_tag = 0;

        if ((nfid = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
            printf("  FAIL  reopen for payload byte-order check\n");
            return 1;
        }
        if ((pds = H5Dopen2(nfid, "/step/0/.payload", H5P_DEFAULT)) < 0) {
            printf("  FAIL  /step/0/.payload missing\n");
            H5Fclose(nfid);
            return 1;
        }
        plen = H5Dget_storage_size(pds);
        {
            hid_t sp = H5Dget_space(pds);
            hssize_t n = H5Sget_select_npoints(sp);

            H5Sclose(sp);
            plen = (hsize_t)n; /* logical length: the payload is opaque bytes */
        }
        if (NULL == (raw = (uint8_t *)malloc((size_t)plen))) {
            printf("  FAIL  malloc payload\n");
            return 1;
        }
        {
            hid_t otype = H5Dget_type(pds); /* opaque, and tagged -- use its own */

            if (H5Dread(pds, otype, H5S_ALL, H5S_ALL, H5P_DEFAULT, raw) < 0) {
                printf("  FAIL  reading .payload back\n");
                nerrors++;
            }
            else {
                /* The VL-sequence entry is first in the step, and its first
                 * element is seq_lens[0] == 3 ints == 12 bytes, so the tag is
                 * 13 (length + 1, the NULL-vs-empty bias). Little-endian that
                 * is 0d 00 00 00 00 00 00 00; byte-swapped it would be
                 * 00 ... 00 0d, which is what a native-endian memcpy would
                 * write on a big-endian host. */
                size_t i;

                for (i = 0; i + 8 <= (size_t)plen; i++) {
                    if (raw[i] == 0x0d && raw[i + 1] == 0 && raw[i + 2] == 0 && raw[i + 3] == 0 &&
                        raw[i + 4] == 0 && raw[i + 5] == 0 && raw[i + 6] == 0 && raw[i + 7] == 0) {
                        found_first_tag = 1;
                        break;
                    }
                }
                if (found_first_tag)
                    printf("  ok    VL length tags are stored little-endian, not host-endian\n");
                else {
                    printf("  FAIL  no little-endian length tag found in .payload -- the VL wire form is "
                           "host-endian and will not read on the opposite endianness\n");
                    nerrors++;
                }
            }
            H5Tclose(otype);
        }
        free(raw);
        H5Dclose(pds);
        H5Fclose(nfid);
    }

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
