/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Group attributes written outside a step reach the stream.
 *
 * A NeXus writer sets up its skeleton before acquiring: /@default,
 * /entry@NX_class, /entry/data@NX_class and @signal, as VL UTF-8 strings the
 * way h5py writes them. Those writes pass through to the live groups, and
 * until now nothing carried them into a step: /step/<n>/entry had no
 * NX_class, and no subscriber or connector reader saw them. They are now
 * folded into the next step that commits.
 *
 *   before step 0   /@default, /entry@NX_class, /entry/data@NX_class,
 *                   /entry/data@signal, /entry@count (an int)
 *   step 0          /entry/data/data
 *   between         /entry@later
 *   step 1          /entry/data/data again
 *
 * Checked natively in /step/0 and /step/1, and through a connector reader at
 * each step. No transport needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_group_attrs.h5"

static int nerrors = 0;

#define CHECK(cond, ...)                                                                                      \
    do {                                                                                                      \
        if (cond)                                                                                             \
            printf("  ok    " __VA_ARGS__);                                                                   \
        else {                                                                                                \
            printf("  FAIL  " __VA_ARGS__);                                                                   \
            nerrors++;                                                                                        \
        }                                                                                                     \
        printf("\n");                                                                                         \
    } while (0)

static hid_t
utf8(void)
{
    hid_t t = H5Tcopy(H5T_C_S1);

    H5Tset_size(t, H5T_VARIABLE);
    H5Tset_cset(t, H5T_CSET_UTF8);
    return t;
}

static int
put_str(hid_t obj, const char *name, const char *val)
{
    hid_t t = utf8(), s = H5Screate(H5S_SCALAR), a;
    int   rc = -1;

    if ((a = H5Acreate2(obj, name, t, s, H5P_DEFAULT, H5P_DEFAULT)) >= 0) {
        rc = H5Awrite(a, t, &val) < 0 ? -1 : 0;
        H5Aclose(a);
    }
    H5Sclose(s);
    H5Tclose(t);
    return rc;
}

static int
put_int(hid_t obj, const char *name, int val)
{
    hid_t s = H5Screate(H5S_SCALAR), a;
    int   rc = -1;

    if ((a = H5Acreate2(obj, name, H5T_NATIVE_INT, s, H5P_DEFAULT, H5P_DEFAULT)) >= 0) {
        rc = H5Awrite(a, H5T_NATIVE_INT, &val) < 0 ? -1 : 0;
        H5Aclose(a);
    }
    H5Sclose(s);
    return rc;
}

/* The string attribute obj@name through file f, or "" (the error stack quiet). */
static const char *
get_str(hid_t f, const char *obj, const char *name)
{
    static char out[64];
    hid_t       a, t;
    char       *v = NULL;

    out[0] = '\0';
    H5E_BEGIN_TRY
    {
        a = H5Aopen_by_name(f, obj, name, H5P_DEFAULT, H5P_DEFAULT);
    }
    H5E_END_TRY
    if (a < 0)
        return out;
    t = utf8();
    if (H5Aread(a, t, &v) >= 0 && v) {
        snprintf(out, sizeof(out), "%s", v);
        H5free_memory(v);
    }
    H5Tclose(t);
    H5Aclose(a);
    return out;
}

static int
get_int(hid_t f, const char *obj, const char *name)
{
    hid_t a;
    int   v = -1;

    H5E_BEGIN_TRY
    {
        a = H5Aopen_by_name(f, obj, name, H5P_DEFAULT, H5P_DEFAULT);
    }
    H5E_END_TRY
    if (a < 0)
        return -1;
    if (H5Aread(a, H5T_NATIVE_INT, &v) < 0)
        v = -1;
    H5Aclose(a);
    return v;
}

static int
write_file(hid_t fapl)
{
    hsize_t n = 4;
    int     vals[4] = {1, 2, 3, 4}, s;
    hid_t   fid, entry, data, sp, ds = H5I_INVALID_HID;

    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (entry = H5Gcreate2(fid, "/entry", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        (data = H5Gcreate2(entry, "data", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        put_str(fid, "default", "entry") < 0 || put_str(entry, "NX_class", "NXentry") < 0 ||
        put_str(data, "NX_class", "NXdata") < 0 || put_str(data, "signal", "data") < 0 ||
        put_int(entry, "count", 7) < 0 || (sp = H5Screate_simple(1, &n, NULL)) < 0)
        return -1;

    for (s = 0; s < 2; s++) {
        if (s == 1 && put_int(entry, "later", 5) < 0)
            return -1;
        if (H5Fbegin_step(fid, 0, NULL, 0) < 0)
            return -1;
        if (s == 0 && (ds = H5Dcreate2(data, "data", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT)) < 0)
            return -1;
        vals[0] = s;
        if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0 || H5Fend_step(fid) < 0)
            return -1;
    }
    H5Dclose(ds);
    H5Sclose(sp);
    H5Gclose(data);
    H5Gclose(entry);
    return H5Fclose(fid);
}

int
main(void)
{
    hid_t fapl, nf, rf;

    printf("vol-stream: group attributes written outside a step reach the stream\n");
    unlink(FNAME);
    fapl = H5Pcreate(H5P_FILE_ACCESS);
    if (H5Pset_fapl_stream(fapl, NULL) < 0 || write_file(fapl) < 0) {
        printf("  FAIL  write\n");
        return 1;
    }

    if ((nf = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  native open\n");
        return 1;
    }
    CHECK(!strcmp(get_str(nf, "/entry", "NX_class"), "NXentry") && !strcmp(get_str(nf, "/", "default"), "entry"),
          "the live groups keep what was written (a plain HDF5 write)");
    CHECK(!strcmp(get_str(nf, "/step/0/entry", "NX_class"), "NXentry") &&
              !strcmp(get_str(nf, "/step/0/entry/data", "NX_class"), "NXdata") &&
              !strcmp(get_str(nf, "/step/0/entry/data", "signal"), "data") &&
              get_int(nf, "/step/0/entry", "count") == 7,
          "/step/0 carries the skeleton's NX_class, @signal and an int attribute");
    CHECK(!strcmp(get_str(nf, "/step/0", "default"), "entry"), "and the root's @default, on /step/0");
    CHECK(get_int(nf, "/step/0/entry", "later") == -1 && get_int(nf, "/step/1/entry", "later") == 5,
          "an attribute written between steps 0 and 1 goes into step 1, not 0");
    H5Fclose(nf);

    /* A connector reader resolves them at the step. */
    if ((rf = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0 || H5Fbegin_step(rf, 0, NULL, 0) < 0) {
        printf("  FAIL  reader open\n");
        return 1;
    }
    {
        hid_t g = H5Gopen2(rf, "/entry", H5P_DEFAULT), a;
        hid_t t = utf8();
        char *v = NULL;

        H5E_BEGIN_TRY
        {
            a = g >= 0 ? H5Aopen(g, "NX_class", H5P_DEFAULT) : -1;
        }
        H5E_END_TRY
        CHECK(a >= 0 && H5Aread(a, t, &v) >= 0 && v && !strcmp(v, "NXentry"),
              "a connector reader at step 0 reads /entry@NX_class");
        if (v)
            H5free_memory(v);
        if (a >= 0)
            H5Aclose(a);
        H5Tclose(t);
        if (g >= 0)
            H5Gclose(g);
    }
    H5Fclose(rf);
    H5Pclose(fapl);
    if (!getenv("T_GROUP_ATTRS_KEEP"))
        unlink(FNAME);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
