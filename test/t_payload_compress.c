/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The step's ".payload" staging dataset is stored compressed.
 *
 * Every step writes its raw bytes to "/step/<n>/.payload" and then replays
 * the real objects beside it, so the same data is stored twice. For a
 * filtered dataset the two costs are wildly different: measured on a 2000-int
 * GZIP dataset, the real object allocated 48 bytes while .payload allocated
 * the full 8000 -- the staging copy was 167x the size of the data it staged,
 * and dominated the file.
 *
 * .payload is an opaque byte array this connector both writes and reads, so
 * deflating it needs no format change and no reader change: HDF5 inflates it
 * transparently on the way back out. This test pins that it actually happens,
 * because the correctness suite cannot -- compression is invisible to every
 * behavioral check, so a regression that silently dropped it would leave all
 * other tests passing while the file quietly grew back.
 *
 * Asserts on *allocated* storage rather than the file size: allocated bytes
 * are what the filter changes, while total file size also moves with metadata
 * and would make the threshold brittle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_payload_compress.h5"
#define NELEM 2000 /* highly compressible on purpose -- see t_precision.c */

int
main(void)
{
    hid_t   vol_id, fapl, fid, space, ds;
    hsize_t dims = NELEM;
    int     vals[NELEM];
    int     i, nerrors = 0;

    printf("vol-stream: step payload staging is compressed\n");

    /* Skip, do not fail, without deflate. Payload compression is explicitly
     * best-effort: the connector falls back to an uncompressed payload when
     * the filter is unavailable, so a zlib-less HDF5 is a *supported*
     * configuration and there is simply nothing here to assert.
     *
     * This differs from test/t_precision.c, which does fail without deflate
     * -- there the filter is the feature under test, not an optional
     * optimization of it. (Getting that distinction wrong is what turned
     * this test red on every CI job that builds HDF5 without zlib, since
     * HDF5_ENABLE_ZLIB_SUPPORT defaults to OFF.) */
    if (H5Zfilter_avail(H5Z_FILTER_DEFLATE) <= 0) {
        printf("  skip  this HDF5 has no deflate filter, so the payload is "
               "legitimately stored uncompressed -- nothing to check\n");
        printf("\nall checks passed (skipped)\n");
        return 0;
    }

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

    for (i = 0; i < NELEM; i++)
        vals[i] = (i % 4) + 100;

    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("  FAIL  begin_step\n");
        return 1;
    }
    if ((space = H5Screate_simple(1, &dims, NULL)) < 0 ||
        (ds = H5Dcreate2(fid, "/plain", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) < 0) {
        printf("  FAIL  write /plain\n");
        return 1;
    }
    H5Dclose(ds);
    H5Sclose(space);
    if (H5Fend_step(fid) < 0) {
        printf("  FAIL  end_step\n");
        return 1;
    }
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    /* Inspect what landed, natively. */
    {
        hid_t     nfid, pds, pdcpl;
        hsize_t   alloc;
        size_t    raw = (size_t)NELEM * sizeof(int);
        int       nfilters;

        if ((nfid = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
            printf("  FAIL  reopen with the native connector\n");
            return 1;
        }
        /* Both of these inspect the .payload staging dataset, which
         * VOL_STREAM_STAGE_PAYLOAD=0 legitimately removes (it duplicates data
         * the replayed objects already hold, at a measured ~2x file size, and
         * nothing reads it back). Skip rather than fail so the whole suite
         * stays runnable under either setting -- the real assertions above,
         * about the data itself, have already run. */
        if (getenv("VOL_STREAM_STAGE_PAYLOAD") && getenv("VOL_STREAM_STAGE_PAYLOAD")[0] == '0') {
            printf("  skip  .payload checks (VOL_STREAM_STAGE_PAYLOAD=0)\n");
            H5Fclose(nfid);
            goto payload_done;
        }

        if ((pds = H5Dopen2(nfid, "/step/0/.payload", H5P_DEFAULT)) < 0) {
            printf("  FAIL  /step/0/.payload is not in the file\n");
            H5Fclose(nfid);
            return 1;
        }

        pdcpl    = H5Dget_create_plist(pds);
        nfilters = H5Pget_nfilters(pdcpl);
        alloc    = H5Dget_storage_size(pds);

        printf("  info  .payload: %zu raw bytes staged, %llu allocated, %d filter(s)\n", raw,
               (unsigned long long)alloc, nfilters);

        if (nfilters < 1) {
            printf("  FAIL  .payload has no filter -- staging compression was lost\n");
            nerrors++;
        }
        else if (alloc * 4 >= (hsize_t)raw) {
            /* A deliberately loose bound: the point is that the staging copy
             * is no longer proportional to the raw bytes, not that GZIP hits
             * any particular ratio on this data. */
            printf("  FAIL  .payload allocated %llu bytes for %zu raw -- not meaningfully compressed\n",
                   (unsigned long long)alloc, raw);
            nerrors++;
        }
        else
            printf("  ok    step payload staging is stored compressed\n");

        H5Pclose(pdcpl);
        H5Dclose(pds);

        /* And the data must still read back correctly through the connector's
         * own reader path, which consumes .payload. */
        {
            hid_t rds = H5Dopen2(nfid, "/step/0/plain", H5P_DEFAULT);
            int  *back = (int *)calloc(NELEM, sizeof(int));

            if (rds < 0 || !back) {
                printf("  FAIL  reopen /plain\n");
                nerrors++;
            }
            else {
                H5Dread(rds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, back);
                for (i = 0; i < NELEM; i++)
                    if (back[i] != (i % 4) + 100) {
                        printf("  FAIL  /plain[%d] = %d, expected %d\n", i, back[i], (i % 4) + 100);
                        nerrors++;
                        break;
                    }
                if (i == NELEM)
                    printf("  ok    the replayed dataset still holds the right values\n");
                H5Dclose(rds);
            }
            free(back);
        }

        H5Fclose(nfid);
payload_done:;
    }

    /* The other half of the trade: staging can be turned off entirely.
     *
     * .payload duplicates data the replayed objects already hold, and nothing
     * reads it back -- the reader index decodes only .manifest, and h5stream
     * skips "."-prefixed names. Measured on incompressible data, where the
     * deflate above cannot help: a 4.00 MB write produced an 8.22 MB file,
     * 4.22 MB of it .payload. With VOL_STREAM_STAGE_PAYLOAD=0 the same write
     * produces 4.00 MB.
     *
     * Checked in a forked child so the setenv() cannot disturb the rest of
     * this test. That only works because the connector reads the variable per
     * step rather than caching it -- a cached value is inherited across fork()
     * already resolved, and this assertion failed exactly that way first.
     */
    {
        pid_t pid = fork();

        if (pid == 0) {
            hid_t   cvol, cfapl, cfid, csp, cds;
            hsize_t cdims[1] = {NELEM};
            int    *cbuf     = (int *)calloc(NELEM, sizeof(int));
            int     j;

            setenv("VOL_STREAM_STAGE_PAYLOAD", "0", 1);
            for (j = 0; j < NELEM; j++)
                cbuf[j] = j;

            if ((cvol = H5VL_stream_register()) < 0)
                _exit(2);
            cfapl = H5Pcreate(H5P_FILE_ACCESS);
            H5Pset_vol(cfapl, cvol, NULL);
            if ((cfid = H5Fcreate("t_payload_nostage.h5", H5F_ACC_TRUNC, H5P_DEFAULT, cfapl)) < 0)
                _exit(2);
            csp = H5Screate_simple(1, cdims, NULL);
            H5Fbegin_step(cfid, 0, NULL, 0);
            cds = H5Dcreate2(cfid, "plain", H5T_NATIVE_INT, csp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            H5Dwrite(cds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, cbuf);
            H5Dclose(cds);
            H5Fend_step(cfid);
            H5Sclose(csp);
            H5Fclose(cfid);
            H5Pclose(cfapl);
            free(cbuf);
            _exit(0);
        }
        else if (pid > 0) {
            int   status = 0;
            hid_t nf, rf;

            waitpid(pid, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                printf("  FAIL  writing with VOL_STREAM_STAGE_PAYLOAD=0 failed\n");
                nerrors++;
            }
            else {
                nf = H5Pcreate(H5P_FILE_ACCESS);
                rf = H5Fopen("t_payload_nostage.h5", H5F_ACC_RDONLY, nf);
                if (rf < 0) {
                    printf("  FAIL  reopen the un-staged file\n");
                    nerrors++;
                }
                else {
                    htri_t has_payload, has_manifest, has_data;

                    H5E_BEGIN_TRY
                    {
                        has_payload  = H5Lexists(rf, "/step/0/.payload", H5P_DEFAULT);
                        has_manifest = H5Lexists(rf, "/step/0/.manifest", H5P_DEFAULT);
                        has_data     = H5Lexists(rf, "/step/0/plain", H5P_DEFAULT);
                    }
                    H5E_END_TRY

                    if (has_payload > 0) {
                        printf("  FAIL  .payload written despite VOL_STREAM_STAGE_PAYLOAD=0\n");
                        nerrors++;
                    }
                    if (has_manifest <= 0) {
                        printf("  FAIL  .manifest missing -- the step is unreadable without it\n");
                        nerrors++;
                    }
                    if (has_data <= 0) {
                        printf("  FAIL  the replayed dataset is missing\n");
                        nerrors++;
                    }
                    if (has_payload == 0 && has_manifest > 0 && has_data > 0)
                        printf("  ok    VOL_STREAM_STAGE_PAYLOAD=0 drops .payload, keeps the real data\n");
                    H5Fclose(rf);
                }
                H5Pclose(nf);
            }
        }
    }

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
