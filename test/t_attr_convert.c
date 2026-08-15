/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Attribute writes whose memory type differs from the attribute's file type.
 *
 * An attribute's captured payload is stored in its *file* type, because that
 * is what replay writes back. When H5Awrite() is handed a different memory
 * type, the caller's bytes are not in that representation -- copying them
 * verbatim reinterprets one type as another. Writing native ints into a
 * native-double attribute produced 4.67e-313 rather than 11, accepted with
 * no error reported: silent corruption, not a rejection.
 *
 * It survived because every attribute scenario in the suite used matching
 * native types, so nothing exercised the conversion path. Datasets were
 * never affected -- a DsetWrite entry captures the caller's *memory* type
 * and replay converts on the way in, which is why byte-order coverage on
 * the dataset side never surfaced this.
 *
 * Cases here:
 *   1. Narrowing representation change (int written to a double attribute)
 *      -- the original failure.
 *   2. The reverse direction (double written to an int attribute), which
 *      also truncates numerically, so the expected values differ from the
 *      source and a copy-verbatim implementation cannot coincidentally pass.
 *   3. A byte-order change at the same width (native int into a big-endian
 *      int attribute), which a same-size memcpy would silently get wrong
 *      on a little-endian host while sizes still match -- so size equality
 *      alone is not a safe short-circuit.
 *   4. A genuinely matching type still round-trips, confirming the
 *      short-circuit path is intact.
 *
 * Values are verified through the *native* connector against what actually
 * landed in the file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_attr_convert.h5"
#define N     4

static int nerrors = 0;

/* Creates an attribute of file type ftype, writes buf using mem type mtype. */
static int
write_attr(hid_t fid, const char *name, hid_t ftype, hid_t mtype, const void *buf)
{
    hid_t   sp, attr;
    hsize_t dim = N;
    herr_t  st;

    if ((sp = H5Screate_simple(1, &dim, NULL)) < 0)
        return -1;
    if ((attr = H5Acreate2(fid, name, ftype, sp, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        H5Sclose(sp);
        return -1;
    }
    st = H5Awrite(attr, mtype, buf);
    H5Aclose(attr);
    H5Sclose(sp);
    return (st < 0) ? -1 : 0;
}

int
main(void)
{
    hid_t  vol_id, fapl, fid, be_int;
    int    int_src[N]    = {11, 22, 33, 44};
    double dbl_src[N]    = {1.5, 2.5, -3.5, 4.5};
    int    match_src[N]  = {7, 8, 9, 10};
    int    i;

    printf("vol-stream: attribute memory/file datatype conversion\n");

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

    if ((be_int = H5Tcopy(H5T_NATIVE_INT)) < 0 || H5Tset_order(be_int, H5T_ORDER_BE) < 0) {
        printf("  FAIL  build big-endian int type\n");
        return 1;
    }

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  begin_step\n");
        return 1;
    }

    if (write_attr(fid, "int_to_double", H5T_NATIVE_DOUBLE, H5T_NATIVE_INT, int_src) < 0) {
        printf("  FAIL  writing int into a double attribute\n");
        nerrors++;
    }
    if (write_attr(fid, "double_to_int", H5T_NATIVE_INT, H5T_NATIVE_DOUBLE, dbl_src) < 0) {
        printf("  FAIL  writing double into an int attribute\n");
        nerrors++;
    }
    if (write_attr(fid, "byte_order", be_int, H5T_NATIVE_INT, int_src) < 0) {
        printf("  FAIL  writing native int into a big-endian int attribute\n");
        nerrors++;
    }
    if (write_attr(fid, "matching", H5T_NATIVE_INT, H5T_NATIVE_INT, match_src) < 0) {
        printf("  FAIL  writing a matching-type attribute\n");
        nerrors++;
    }

    if (H5Fend_step(fid) < 0) {
        printf("  FAIL  end_step\n");
        nerrors++;
    }

    H5Tclose(be_int);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    /* --- What actually landed, read natively. --- */
    {
        hid_t  nfid, na;
        double back_d[N];
        int    back_i[N];

        if ((nfid = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
            printf("  FAIL  reopen with the native connector\n");
            return 1;
        }

        /* 1. int -> double: values preserved, representation converted. */
        if ((na = H5Aopen_by_name(nfid, "/step/0", "int_to_double", H5P_DEFAULT, H5P_DEFAULT)) < 0) {
            printf("  FAIL  int_to_double attribute missing\n");
            nerrors++;
        }
        else {
            memset(back_d, 0, sizeof(back_d));
            H5Aread(na, H5T_NATIVE_DOUBLE, back_d);
            for (i = 0; i < N; i++)
                if (back_d[i] != (double)int_src[i]) {
                    printf("  FAIL  int_to_double[%d] = %g, expected %g\n", i, back_d[i],
                           (double)int_src[i]);
                    nerrors++;
                    break;
                }
            if (i == N)
                printf("  ok    int written to a double attribute converts correctly\n");
            H5Aclose(na);
        }

        /* 2. double -> int: HDF5 truncates toward zero. */
        if ((na = H5Aopen_by_name(nfid, "/step/0", "double_to_int", H5P_DEFAULT, H5P_DEFAULT)) < 0) {
            printf("  FAIL  double_to_int attribute missing\n");
            nerrors++;
        }
        else {
            memset(back_i, 0, sizeof(back_i));
            H5Aread(na, H5T_NATIVE_INT, back_i);
            for (i = 0; i < N; i++)
                if (back_i[i] != (int)dbl_src[i]) {
                    printf("  FAIL  double_to_int[%d] = %d, expected %d\n", i, back_i[i],
                           (int)dbl_src[i]);
                    nerrors++;
                    break;
                }
            if (i == N)
                printf("  ok    double written to an int attribute converts correctly\n");
            H5Aclose(na);
        }

        /* 3. Byte-order change at equal width. Reading back as native int
         * undoes the file's ordering, so correct storage means the original
         * values reappear -- a same-size memcpy would yield byte-swapped
         * garbage here. */
        if ((na = H5Aopen_by_name(nfid, "/step/0", "byte_order", H5P_DEFAULT, H5P_DEFAULT)) < 0) {
            printf("  FAIL  byte_order attribute missing\n");
            nerrors++;
        }
        else {
            memset(back_i, 0, sizeof(back_i));
            H5Aread(na, H5T_NATIVE_INT, back_i);
            for (i = 0; i < N; i++)
                if (back_i[i] != int_src[i]) {
                    printf("  FAIL  byte_order[%d] = %d, expected %d (same-width conversion still needs "
                           "a real convert, not a memcpy)\n",
                           i, back_i[i], int_src[i]);
                    nerrors++;
                    break;
                }
            if (i == N)
                printf("  ok    byte-order-only conversion is handled at equal width\n");
            H5Aclose(na);
        }

        /* 4. Matching types keep working. */
        if ((na = H5Aopen_by_name(nfid, "/step/0", "matching", H5P_DEFAULT, H5P_DEFAULT)) < 0) {
            printf("  FAIL  matching attribute missing\n");
            nerrors++;
        }
        else {
            memset(back_i, 0, sizeof(back_i));
            H5Aread(na, H5T_NATIVE_INT, back_i);
            for (i = 0; i < N; i++)
                if (back_i[i] != match_src[i]) {
                    printf("  FAIL  matching[%d] = %d, expected %d\n", i, back_i[i], match_src[i]);
                    nerrors++;
                    break;
                }
            if (i == N)
                printf("  ok    a matching-type attribute still round-trips\n");
            H5Aclose(na);
        }

        H5Fclose(nfid);
    }

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
