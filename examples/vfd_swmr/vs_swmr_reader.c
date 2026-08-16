/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* PoC reader: a SEPARATE process reading the live file while the writer
 * still holds it open.
 *
 * Uses the NATIVE VOL deliberately -- not vol-stream. The claim under test is
 * that VFD SWMR gives live visibility of the PERSISTENCE path (the
 * /step/<n>/ groups appearing in the file). vol-stream's own reader index is
 * built by scanning at open time and does not grow live, so involving it
 * would confound the measurement.
 *
 * argv[1]: 1 = enable VFD SWMR (default), 0 = control arm without it.
 *
 * The control arm is the documented baseline: a second process cannot read a
 * live HDF5 file at all.
 */

#include "vs_swmr_common.h"

int
main(int argc, char **argv)
{
    int enable_swmr = (argc > 1) ? atoi(argv[1]) : 1;

    hid_t fapl = H5I_INVALID_HID, fid = H5I_INVALID_HID;
    long  last = -1;
    int   polls, saw_growth = 0, first_seen = -1;
    int   data_reads = 0, data_bad = 0;

    /* Wait for the writer to create the file. */
    for (polls = 0; polls < 100; polls++) {
        FILE *f = fopen(VS_SWMR_FNAME, "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000);
    }

    if ((fapl = vs_swmr_fapl(0 /* reader */, enable_swmr)) < 0) {
        fprintf(stderr, "reader: cannot build FAPL\n");
        return 1;
    }

    /* Give the writer a moment to get its first tick out. */
    usleep(800000);

    H5E_BEGIN_TRY { fid = H5Fopen(VS_SWMR_FNAME, H5F_ACC_RDONLY, fapl); }
    H5E_END_TRY

    if (fid < 0) {
        printf("reader: CANNOT OPEN the live file (VFD SWMR %s)\n", enable_swmr ? "ON" : "OFF");
        printf("reader: RESULT=no-open\n");
        H5Pclose(fapl);
        return 2;
    }

    printf("reader: opened the live file (VFD SWMR %s)\n", enable_swmr ? "ON" : "OFF");
    fflush(stdout);

    /* Poll for new /step/<n>/ groups while the writer keeps committing. */
    for (polls = 0; polls < 40; polls++) {
        long n = vs_swmr_count_steps(fid);

        if (n >= 0) {
            if (first_seen < 0) {
                first_seen = (int)n;
                printf("reader: first observation: %ld step(s) visible\n", n);
                fflush(stdout);
            }
            if (n != last) {
                if (last >= 0 && n > last) {
                    char  path[64];
                    hid_t ds;

                    saw_growth = 1;

                    /* Structure appearing is not enough -- read the CONTENT
                     * of the newest step. Step data is raw data, so this is
                     * what flush_raw_data actually buys. */
                    snprintf(path, sizeof(path), "/step/%ld%s", n - 1, VS_SWMR_DSET);
                    H5E_BEGIN_TRY { ds = H5Dopen2(fid, path, H5P_DEFAULT); }
                    H5E_END_TRY

                    if (ds < 0) {
                        printf("reader: %ld -> %ld, but CANNOT OPEN %s\n", last, n, path);
                    }
                    else {
                        hid_t   sp    = H5Dget_space(ds);
                        hssize_t nelem = H5Sget_simple_extent_npoints(sp);
                        int     *buf  = malloc((size_t)nelem * sizeof(int));

                        if (buf && H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) >= 0) {
                            /* Writer fills element i with value i. */
                            int ok = (buf[0] == 0 && buf[nelem - 1] == (int)nelem - 1);

                            printf("reader: %ld -> %ld  read %s: %lld elems, first=%d last=%d  [%s]\n",
                                   last, n, path, (long long)nelem, buf[0], buf[nelem - 1],
                                   ok ? "DATA OK" : "DATA MISMATCH");
                            if (!ok)
                                data_bad = 1;
                            else
                                data_reads++;
                        }
                        else {
                            printf("reader: %ld -> %ld, opened %s but READ FAILED\n", last, n, path);
                            data_bad = 1;
                        }
                        free(buf);
                        H5Sclose(sp);
                        H5Dclose(ds);
                    }
                    fflush(stdout);
                }
                last = n;
            }
        }
        usleep(400000); /* one tick */
    }

    printf("reader: final visible step count = %ld\n", last);
    printf("reader: live content reads: %d ok, bad=%d\n", data_reads, data_bad);
    printf("reader: RESULT=%s\n",
           (saw_growth && data_reads > 0 && !data_bad) ? "live-growth-and-data-verified"
           : saw_growth ? "growth-but-data-problem" : "opened-but-no-growth");

    H5Fclose(fid);
    H5Pclose(fapl);
    return (saw_growth && data_reads > 0 && !data_bad) ? 0 : 3;
}
