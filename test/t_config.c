/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Per-file configuration through the FAPL: H5Pset_fapl_stream().
 *
 *   1. Two files in one process, configured differently, behave differently:
 *      one with payload staging off leaves no /step/0/.payload, the other
 *      (defaults) does. Environment-variable settings cannot express this.
 *   2. max_pending_bytes applies to its own file only: a write over the
 *      limit fails there and succeeds in a file without one.
 *   3. An environment variable that is set overrides the FAPL value.
 *   4. The same settings round-trip through the connector-info string form
 *      that HDF5_VOL_CONNECTOR uses, and an unknown key is refused.
 *
 * No transport needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define N 256 /* ints per write: 1 KiB */

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

/* Write one step of N ints to /x in a new file under fapl. Returns the
 * H5Dwrite() result (the call a pending-bytes limit refuses), or -2 if the
 * setup around it failed. */
static int
write_one(const char *name, hid_t fapl)
{
    hid_t   fid, sp, ds;
    hsize_t n = N;
    int     vals[N], i, w;

    for (i = 0; i < N; i++)
        vals[i] = i;
    H5E_BEGIN_TRY
    {
        unlink(name);
    }
    H5E_END_TRY
    if ((fid = H5Fcreate(name, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 || (sp = H5Screate_simple(1, &n, NULL)) < 0 ||
        H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (ds = H5Dcreate2(fid, "/x", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0)
        return -2;
    H5E_BEGIN_TRY
    {
        w = H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals);
    }
    H5E_END_TRY
    H5Dclose(ds);
    H5Sclose(sp);
    if (H5Fend_step(fid) < 0)
        w = w < 0 ? w : -2;
    H5Fclose(fid);
    return w < 0 ? -1 : 0;
}

/* Whether the step's .payload staging dataset exists, read natively. */
static int
staged(const char *name)
{
    hid_t fid = H5Fopen(name, H5F_ACC_RDONLY, H5P_DEFAULT);
    int   yes;

    if (fid < 0)
        return -1;
    yes = H5Lexists(fid, "/step/0/.payload", H5P_DEFAULT) > 0;
    H5Fclose(fid);
    return yes;
}

int
main(void)
{
    H5VL_stream_config_t cfg;
    hid_t                fapl_off, fapl_def, fapl_lim, vol_id;

    printf("vol-stream: per-file configuration through the FAPL\n");
    unsetenv("VOL_STREAM_STAGE_PAYLOAD");
    unsetenv("VOL_STREAM_MAX_PENDING_BYTES");
    unsetenv("VOL_STREAM_NA");

    /* 1. Two differently configured files in one process. */
    H5VL_stream_config_init(&cfg);
    cfg.stage_payload = 0;
    fapl_off          = H5Pcreate(H5P_FILE_ACCESS);
    fapl_def          = H5Pcreate(H5P_FILE_ACCESS);
    if (H5Pset_fapl_stream(fapl_off, &cfg) < 0 || H5Pset_fapl_stream(fapl_def, NULL) < 0) {
        printf("  FAIL  H5Pset_fapl_stream\n");
        return 1;
    }
    CHECK(write_one("t_config_off.h5", fapl_off) == 0 && write_one("t_config_def.h5", fapl_def) == 0,
          "both files written");
    CHECK(staged("t_config_off.h5") == 0, "stage_payload=0 on its FAPL: no .payload in that file");
    CHECK(staged("t_config_def.h5") == 1, "default FAPL in the same process: .payload staged");

    /* 2. A pending-bytes limit belongs to its own file. */
    H5VL_stream_config_init(&cfg);
    cfg.max_pending_bytes = 64; /* below one 1 KiB write */
    fapl_lim              = H5Pcreate(H5P_FILE_ACCESS);
    if (H5Pset_fapl_stream(fapl_lim, &cfg) < 0) {
        printf("  FAIL  H5Pset_fapl_stream (limit)\n");
        return 1;
    }
    CHECK(write_one("t_config_lim.h5", fapl_lim) == -1, "max_pending_bytes=64 refuses a 1 KiB write");
    CHECK(write_one("t_config_def.h5", fapl_def) == 0, "and a file without the limit still takes it");

    /* 3. The environment variable overrides the FAPL. */
    setenv("VOL_STREAM_STAGE_PAYLOAD", "1", 1);
    CHECK(write_one("t_config_off.h5", fapl_off) == 0 && staged("t_config_off.h5") == 1,
          "VOL_STREAM_STAGE_PAYLOAD=1 overrides stage_payload=0 on the FAPL");
    unsetenv("VOL_STREAM_STAGE_PAYLOAD");

    /* 4. The string form HDF5_VOL_CONNECTOR uses. */
    vol_id = H5VL_stream_register();
    {
        void *info = NULL;
        char *str  = NULL;
        hid_t fapl_str;

        CHECK(H5VLconnector_str_to_info("under_vol=0;under_info={};stage_payload=0;max_pending_bytes=4096",
                                        vol_id, &info) >= 0 &&
                  info,
              "settings parse from the connector string");
        if (info && H5VLconnector_info_to_str(info, vol_id, &str) >= 0 && str) {
            CHECK(strstr(str, "stage_payload=0") && strstr(str, "max_pending_bytes=4096"),
                  "and serialize back: \"%s\"", str);
            H5free_memory(str);
        }
        if (info) {
            fapl_str = H5Pcreate(H5P_FILE_ACCESS);
            if (H5Pset_vol(fapl_str, vol_id, info) >= 0) {
                CHECK(write_one("t_config_str.h5", fapl_str) == 0 && staged("t_config_str.h5") == 0,
                      "a FAPL built from the string applies them");
            }
            H5Pclose(fapl_str);
            H5VLfree_connector_info(vol_id, info);
        }

        info = NULL;
        H5E_BEGIN_TRY
        {
            CHECK(H5VLconnector_str_to_info("under_vol=0;under_info={};stage_paylod=0", vol_id, &info) < 0,
                  "a misspelled key is refused, not ignored");
        }
        H5E_END_TRY
        if (info)
            H5VLfree_connector_info(vol_id, info);
    }

    H5Pclose(fapl_off);
    H5Pclose(fapl_def);
    H5Pclose(fapl_lim);
    unlink("t_config_off.h5");
    unlink("t_config_def.h5");
    unlink("t_config_lim.h5");
    unlink("t_config_str.h5");

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
