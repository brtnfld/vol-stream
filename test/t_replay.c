/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M2 exit gate: the replay invariant.
 *
 * The same content is written twice: once natively at a bare path (e.g.
 * "/ints"), and once through vol-stream with every write bracketed in a
 * single step (landing, per decision #2 in dev-plan.md, group-based under
 * "/step/0/ints"). Because the connector never touches the real file until
 * end_step() decodes its own just-captured H5Tencode/H5Sencode2/H5Pencode2
 * blobs and replays from *those*, an h5diff match here is a value-level
 * proof of the manifest's fidelity across HDF5's data model -- not merely
 * that the connector's own bookkeeping is self-consistent.
 *
 * Covers the M2 exit-gate matrix: compound and enum types, byte-order
 * mismatch, a dataset using a committed datatype, attributes, a nested
 * group, chunked and contiguous layouts, and a dataset written via two
 * separate partial hyperslabs in the same step. VL data, references and
 * filtered-chunk passthrough are explicitly out of scope for M2 -- see
 * docs/dev-plan.md.
 *
 * HDF5 has no in-process diff API, so h5diff is invoked as a subprocess --
 * same idiom as test/run_api_suite.sh. Point VOL_STREAM_H5DIFF at the
 * binary if it is not on PATH.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NATIVE_FILE  "t_replay_native.h5"
#define STEPPED_FILE "t_replay_stepped.h5"
#define NELEM        64

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
            printf("  FAIL  %s (%s:%d)\n", (what), __FILE__, __LINE__);                                     \
            nerrors++;                                                                                       \
            return -1;                                                                                       \
        }                                                                                                     \
    } while (0)

typedef enum { RED = 0, GREEN = 1, BLUE = 2 } color_t;

typedef struct {
    int    a;
    double b;
} compound_t;

/* ------------------------------------------------------------------------
 * Content shared between the native and stepped writers, so a mismatch can
 * only come from the connector's capture/replay path, not from the two
 * writers disagreeing about what to write.
 * ------------------------------------------------------------------------ */
static void
fill_ints(int *buf)
{
    for (int i = 0; i < NELEM; i++)
        buf[i] = i * 3 - 7;
}

static void
fill_compound(compound_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        buf[i].a = (int)i;
        buf[i].b = (double)i * 1.5;
    }
}

static void
fill_enum(color_t *buf, size_t n)
{
    static const color_t cycle[3] = {RED, GREEN, BLUE};
    for (size_t i = 0; i < n; i++)
        buf[i] = cycle[i % 3];
}

static hid_t
make_compound_type(void)
{
    hid_t t = H5Tcreate(H5T_COMPOUND, sizeof(compound_t));
    H5Tinsert(t, "a", offsetof(compound_t, a), H5T_NATIVE_INT);
    H5Tinsert(t, "b", offsetof(compound_t, b), H5T_NATIVE_DOUBLE);
    return t;
}

static hid_t
make_enum_type(void)
{
    hid_t   t = H5Tcreate(H5T_ENUM, sizeof(color_t));
    color_t v;

    v = RED;
    H5Tenum_insert(t, "RED", &v);
    v = GREEN;
    H5Tenum_insert(t, "GREEN", &v);
    v = BLUE;
    H5Tenum_insert(t, "BLUE", &v);
    return t;
}

/* ------------------------------------------------------------------------
 * Writer, parameterized by loc_id: pass a plain file id for the native
 * reference, or a connector-backed one (with a step already open) for the
 * stepped copy. Every object lands at the same relative path either way --
 * the connector is what moves it under /step/0/ on the far side.
 * ------------------------------------------------------------------------ */
static int
write_scenarios(hid_t loc_id)
{
    hid_t   sp1[1], dcpl;
    hsize_t dims[1] = {NELEM};
    int     ibuf[NELEM];

    fill_ints(ibuf);

    /* 1: plain contiguous int dataset */
    {
        hid_t ds, space;
        CHECK(space = H5Screate_simple(1, dims, NULL), "H5Screate_simple(ints)");
        CHECK(ds = H5Dcreate2(loc_id, "ints", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              "H5Dcreate2(ints)");
        CHECK(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, ibuf), "H5Dwrite(ints)");

        /* Attributes on a dataset created in the same step. */
        {
            hid_t   ascalar, aarray, sspace, aspace;
            int     scalar_val = 42;
            int     array_val[4] = {1, 2, 3, 4};
            hsize_t adims[1]     = {4};

            CHECK(sspace = H5Screate(H5S_SCALAR), "H5Screate(scalar attr space)");
            CHECK(ascalar = H5Acreate2(ds, "scalar_attr", H5T_NATIVE_INT, sspace, H5P_DEFAULT, H5P_DEFAULT),
                  "H5Acreate2(scalar_attr)");
            CHECK(H5Awrite(ascalar, H5T_NATIVE_INT, &scalar_val), "H5Awrite(scalar_attr)");
            H5Aclose(ascalar);
            H5Sclose(sspace);

            CHECK(aspace = H5Screate_simple(1, adims, NULL), "H5Screate_simple(array attr space)");
            CHECK(aarray = H5Acreate2(ds, "array_attr", H5T_NATIVE_INT, aspace, H5P_DEFAULT, H5P_DEFAULT),
                  "H5Acreate2(array_attr)");
            CHECK(H5Awrite(aarray, H5T_NATIVE_INT, array_val), "H5Awrite(array_attr)");
            H5Aclose(aarray);
            H5Sclose(aspace);
        }

        H5Dclose(ds);
        H5Sclose(space);
    }

    /* 2: chunked dataset, same content */
    {
        hid_t ds, space;
        CHECK(space = H5Screate_simple(1, dims, NULL), "H5Screate_simple(chunked)");
        CHECK(dcpl = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate(dcpl)");
        {
            hsize_t chunk[1] = {16};
            CHECK(H5Pset_chunk(dcpl, 1, chunk), "H5Pset_chunk");
        }
        CHECK(ds = H5Dcreate2(loc_id, "chunked", H5T_NATIVE_INT, space, H5P_DEFAULT, dcpl, H5P_DEFAULT),
              "H5Dcreate2(chunked)");
        CHECK(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, ibuf), "H5Dwrite(chunked)");
        H5Dclose(ds);
        H5Sclose(space);
        H5Pclose(dcpl);
    }

    /* 3: compound type */
    {
        hid_t      ds, space, ctype;
        compound_t cbuf[8];
        hsize_t    cdims[1] = {8};

        fill_compound(cbuf, 8);
        CHECK(space = H5Screate_simple(1, cdims, NULL), "H5Screate_simple(compound)");
        CHECK(ctype = make_compound_type(), "make_compound_type");
        CHECK(ds = H5Dcreate2(loc_id, "compound", ctype, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              "H5Dcreate2(compound)");
        CHECK(H5Dwrite(ds, ctype, H5S_ALL, H5S_ALL, H5P_DEFAULT, cbuf), "H5Dwrite(compound)");
        H5Dclose(ds);
        H5Sclose(space);
        H5Tclose(ctype);
    }

    /* 4: enum type */
    {
        hid_t   ds, space, etype;
        color_t ebuf[8];
        hsize_t edims[1] = {8};

        fill_enum(ebuf, 8);
        CHECK(space = H5Screate_simple(1, edims, NULL), "H5Screate_simple(enum)");
        CHECK(etype = make_enum_type(), "make_enum_type");
        CHECK(ds = H5Dcreate2(loc_id, "enumd", etype, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              "H5Dcreate2(enumd)");
        CHECK(H5Dwrite(ds, etype, H5S_ALL, H5S_ALL, H5P_DEFAULT, ebuf), "H5Dwrite(enumd)");
        H5Dclose(ds);
        H5Sclose(space);
        H5Tclose(etype);
    }

    /* 5: byte-order mismatch -- file type is explicitly big-endian, written
     * from a native-order buffer, forcing a real type-conversion write (and,
     * on replay, a real type-conversion write again from the decoded type). */
    {
        hid_t ds, space, betype;
        CHECK(space = H5Screate_simple(1, dims, NULL), "H5Screate_simple(byteswap)");
        CHECK(betype = H5Tcopy(H5T_STD_I32BE), "H5Tcopy(H5T_STD_I32BE)");
        CHECK(ds = H5Dcreate2(loc_id, "byteswap", betype, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              "H5Dcreate2(byteswap)");
        CHECK(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, ibuf), "H5Dwrite(byteswap)");
        H5Dclose(ds);
        H5Sclose(space);
        H5Tclose(betype);
    }

    /* 6: dataset using a committed datatype. Committed types stay live even
     * inside a step (dev-plan.md decision, see H5VLstream.c) -- only the
     * dataset that uses one is captured, via its own type_enc blob. */
    {
        hid_t ds, space, ctype;
        CHECK(ctype = H5Tcopy(H5T_NATIVE_INT), "H5Tcopy(committed base)");
        CHECK(H5Tcommit2(loc_id, "committed_type", ctype, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              "H5Tcommit2");
        CHECK(space = H5Screate_simple(1, dims, NULL), "H5Screate_simple(committed_type_ds)");
        CHECK(ds = H5Dcreate2(loc_id, "committed_type_ds", ctype, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              "H5Dcreate2(committed_type_ds)");
        CHECK(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, ibuf), "H5Dwrite(committed_type_ds)");
        H5Dclose(ds);
        H5Sclose(space);
        H5Tclose(ctype);
    }

    /* 7: nested group */
    {
        hid_t grp, ds, space;
        CHECK(grp = H5Gcreate2(loc_id, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), "H5Gcreate2(grp)");
        CHECK(space = H5Screate_simple(1, dims, NULL), "H5Screate_simple(grp/sub)");
        CHECK(ds = H5Dcreate2(grp, "sub", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              "H5Dcreate2(grp/sub)");
        CHECK(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, ibuf), "H5Dwrite(grp/sub)");
        H5Dclose(ds);
        H5Sclose(space);
        H5Gclose(grp);
    }

    /* 8: two disjoint partial hyperslab writes to the same dataset --
     * multiple DsetWrite entries against one DsetCreate entry. */
    {
        hid_t   ds, space, fspace1, fspace2, mspace;
        hsize_t half = NELEM / 2;
        hsize_t start1[1] = {0}, count1[1] = {half};
        hsize_t start2[1] = {half}, count2[1] = {half};

        CHECK(space = H5Screate_simple(1, dims, NULL), "H5Screate_simple(partial)");
        CHECK(ds = H5Dcreate2(loc_id, "partial", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              "H5Dcreate2(partial)");

        CHECK(mspace = H5Screate_simple(1, &half, NULL), "H5Screate_simple(partial mem)");

        CHECK(fspace1 = H5Scopy(space), "H5Scopy(fspace1)");
        CHECK(H5Sselect_hyperslab(fspace1, H5S_SELECT_SET, start1, NULL, count1, NULL), "hyperslab 1");
        CHECK(H5Dwrite(ds, H5T_NATIVE_INT, mspace, fspace1, H5P_DEFAULT, ibuf), "H5Dwrite(partial 1)");
        H5Sclose(fspace1);

        CHECK(fspace2 = H5Scopy(space), "H5Scopy(fspace2)");
        CHECK(H5Sselect_hyperslab(fspace2, H5S_SELECT_SET, start2, NULL, count2, NULL), "hyperslab 2");
        CHECK(H5Dwrite(ds, H5T_NATIVE_INT, mspace, fspace2, H5P_DEFAULT, ibuf + half), "H5Dwrite(partial 2)");
        H5Sclose(fspace2);

        H5Sclose(mspace);
        H5Dclose(ds);
        H5Sclose(space);
    }

    (void)sp1;
    return 0;
} /* end write_scenarios() */

static int
write_native(void)
{
    hid_t fid;
    int   ret = -1;

    CHECK(fid = H5Fcreate(NATIVE_FILE, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT), "H5Fcreate(native)");
    if (write_scenarios(fid) < 0)
        goto done;
    ret = 0;
done:
    H5Fclose(fid);
    return ret;
} /* end write_native() */

static int
write_stepped(hid_t vol_id)
{
    hid_t fapl, fid;
    int   ret = -1;

    CHECK(fapl = H5Pcreate(H5P_FILE_ACCESS), "H5Pcreate(fapl)");
    CHECK(H5Pset_vol(fapl, vol_id, NULL), "H5Pset_vol");
    CHECK(fid = H5Fcreate(STEPPED_FILE, H5F_ACC_TRUNC, H5P_DEFAULT, fapl), "H5Fcreate(stepped)");
    H5Pclose(fapl);

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  H5Fbegin_step (%s:%d)\n", __FILE__, __LINE__);
        nerrors++;
        goto done;
    }
    if (write_scenarios(fid) < 0)
        goto done;
    if (H5Fend_step(fid) < 0) {
        printf("  FAIL  H5Fend_step (%s:%d)\n", __FILE__, __LINE__);
        nerrors++;
        goto done;
    }
    ret = 0;
done:
    H5Fclose(fid);
    return ret;
} /* end write_stepped() */

/* h5diff <stepped> <native> /step/0/<rel_path> /<rel_path> -- HDF5 has no
 * in-process diff API, so shell out (same idiom as run_api_suite.sh). */
static int
diff_object(const char *rel_path)
{
    const char *h5diff = getenv("VOL_STREAM_H5DIFF");
    char        cmd[1024];
    char        what[256];
    int         rc;

    if (!h5diff || h5diff[0] == '\0')
        h5diff = "h5diff";

    snprintf(cmd, sizeof(cmd), "%s %s %s /step/0/%s /%s", h5diff, STEPPED_FILE, NATIVE_FILE, rel_path,
             rel_path);
    rc = system(cmd);

    snprintf(what, sizeof(what), "h5diff clean: %s", rel_path);
    EXPECT(rc == 0, what);

    return rc;
} /* end diff_object() */

int
main(void)
{
    hid_t vol_id;

    printf("vol-stream M2 exit gate: the replay invariant\n");

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("  FAIL  H5VL_stream_register\n");
        return 1;
    }

    if (write_native() < 0) {
        printf("  FAIL  writing native reference file\n");
        return 1;
    }
    if (write_stepped(vol_id) < 0) {
        printf("  FAIL  writing connector file with a step\n");
        return 1;
    }

    diff_object("ints");
    diff_object("chunked");
    diff_object("compound");
    diff_object("enumd");
    diff_object("byteswap");
    diff_object("committed_type_ds");
    diff_object("grp/sub");
    diff_object("partial");

    H5VLclose(vol_id);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
} /* end main() */
