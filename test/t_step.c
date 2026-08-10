/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M1 exit gate, plus the parts of it M2 keeps.
 *
 *   1. *Unbracketed* writes through the connector produce a file
 *      byte-identical to one written through native HDF5 with no connector
 *      at all -- the pass-through invariant, unaffected by M2 and expected
 *      to hold forever.
 *
 *   2. The step state machine is truthful and rejects misordered calls.
 *
 * M1's third assertion -- that *bracketed* writes were also byte-identical
 * to unbracketed ones -- no longer holds as of M2: bracketed writes are now
 * captured into a manifest and replayed group-based under /step/<n>/. The
 * replacement invariant (h5diff-clean replay against a native reference)
 * lives in test/t_replay.c.
 *
 * On byte-identical comparison
 * ----------------------------
 * HDF5 stores four timestamps in an object header when track_times is on, which
 * it is by default, so two files with identical content written seconds apart
 * differ in 8 bytes: four timestamp bytes in the root group's header plus its
 * checksum.
 *
 * Disabling track_times on the dataset DCPL is not enough, because the root
 * group is created with the file. The fix is to disable it on the *FCPL* as
 * well, which carries the root group's object-creation properties. With both
 * off, output is reproducible and a byte comparison becomes a meaningful
 * assertion rather than a race against the clock.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NELEM   64
#define NSTEPS  3

static int nerrors = 0;

#define EXPECT(cond, what)                                                                                   \
    do {                                                                                                     \
        if (!(cond)) {                                                                                        \
            printf("  FAIL  %s (%s:%d)\n", (what), __FILE__, __LINE__);                                      \
            nerrors++;                                                                                       \
        }                                                                                                    \
        else                                                                                                 \
            printf("  ok    %s\n", (what));                                                                  \
    } while (0)

/* Reproducible-output property lists: see the note at the top of the file. */
static hid_t
make_fcpl(void)
{
    hid_t fcpl = H5Pcreate(H5P_FILE_CREATE);
    H5Pset_obj_track_times(fcpl, false);
    return fcpl;
}

static hid_t
make_dcpl(void)
{
    hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_obj_track_times(dcpl, false);
    return dcpl;
}

/*
 * Write the same content three ways.
 *   use_connector: route through vol-stream rather than native HDF5
 *   use_steps:     bracket each dataset write in begin_step/end_step
 */
static int
write_file(const char *fn, hid_t vol_id, int use_connector, int use_steps)
{
    hid_t   fapl = H5P_DEFAULT, fcpl, fid, dcpl, sp;
    hsize_t dims[1] = {NELEM};
    int     buf[NELEM];
    int     ret = -1;

    if (use_connector) {
        if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0)
            return -1;
        if (H5Pset_vol(fapl, vol_id, NULL) < 0)
            return -1;
    }

    fcpl = make_fcpl();
    if ((fid = H5Fcreate(fn, H5F_ACC_TRUNC, fcpl, fapl)) < 0)
        goto done;

    dcpl = make_dcpl();
    sp   = H5Screate_simple(1, dims, NULL);

    for (int s = 0; s < NSTEPS; s++) {
        char           name[32];
        const uint64_t logical = (uint64_t)(500 + s * 50);
        hid_t          ds;

        snprintf(name, sizeof(name), "step_%d", s);
        for (int i = 0; i < NELEM; i++)
            buf[i] = s * 1000 + i;

        if (use_steps && H5Fbegin_step(fid, 1, &logical, 0) < 0)
            goto done_inner;

        if ((ds = H5Dcreate2(fid, name, H5T_NATIVE_INT, sp, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0)
            goto done_inner;
        if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) < 0) {
            H5Dclose(ds);
            goto done_inner;
        }
        H5Dclose(ds);

        if (use_steps && H5Fend_step(fid) < 0)
            goto done_inner;
    }
    ret = 0;

done_inner:
    H5Sclose(sp);
    H5Pclose(dcpl);
    H5Fclose(fid);
done:
    H5Pclose(fcpl);
    if (use_connector && fapl != H5P_DEFAULT)
        H5Pclose(fapl);
    return ret;
}

/* Byte comparison, reporting the first difference so a failure is diagnosable
 * rather than just "files differ". */
static int
files_identical(const char *a, const char *b, char *why, size_t why_len)
{
    FILE  *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int    same = 1;
    long   off  = 0;

    if (!fa || !fb) {
        snprintf(why, why_len, "could not open %s", fa ? b : a);
        same = 0;
        goto done;
    }

    for (;;) {
        int ca = fgetc(fa), cb = fgetc(fb);

        if (ca == EOF && cb == EOF)
            break;
        if (ca != cb) {
            if (ca == EOF || cb == EOF)
                snprintf(why, why_len, "different lengths (diverge at byte %ld)", off);
            else
                snprintf(why, why_len, "byte %ld: 0x%02x vs 0x%02x", off, ca, cb);
            same = 0;
            break;
        }
        off++;
    }

done:
    if (fa)
        fclose(fa);
    if (fb)
        fclose(fb);
    return same;
}

/* State transitions, and the calls that must be refused. */
static void
check_state_machine(hid_t vol_id)
{
    hid_t             fapl, fcpl, fid;
    H5F_step_status_t st;
    const uint64_t    ids[2] = {7, 11};
    herr_t            r;

    fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_vol(fapl, vol_id, NULL);
    fcpl = make_fcpl();
    fid  = H5Fcreate("t_step_state.h5", H5F_ACC_TRUNC, fcpl, fapl);

    if (fid < 0) {
        printf("  FAIL  could not create state-machine test file\n");
        nerrors++;
        goto done;
    }

    H5Fstep_status(fid, &st);
    EXPECT(st == H5F_STEP_NOT_IN_STEP, "fresh file reports NOT_IN_STEP");

    EXPECT(H5Fbegin_step(fid, 2, ids, 0) >= 0, "begin_step succeeds");
    H5Fstep_status(fid, &st);
    EXPECT(st == H5F_STEP_IN_STEP, "reports IN_STEP inside a step");

    /* Negative cases. HDF5 prints a stack for each failure; silence it so the
     * output shows intent rather than looking like something broke. */
    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

    r = H5Fbegin_step(fid, 0, NULL, 0);
    EXPECT(r < 0, "nested begin_step is rejected");

    EXPECT(H5Fend_step(fid) >= 0, "end_step succeeds");

    r = H5Fend_step(fid);
    EXPECT(r < 0, "end_step without an open step is rejected");

    {
        const char *paths[1] = {""};
        hid_t       sp       = H5Screate(H5S_SCALAR);
        hid_t       spaces[1];
        spaces[0] = sp;

        r = H5Fsubscribe(fid, 1, paths, spaces, NULL);
        EXPECT(r < 0, "subscribe with an empty path is rejected");

        paths[0]  = "/data";
        spaces[0] = H5I_INVALID_HID;
        r         = H5Fsubscribe(fid, 1, paths, spaces, NULL);
        EXPECT(r < 0, "subscribe with an invalid dataspace is rejected");

        H5Sclose(sp);
    }

    H5Eset_auto2(H5E_DEFAULT, (H5E_auto2_t)H5Eprint2, stderr);

    H5Fstep_status(fid, &st);
    EXPECT(st == H5F_STEP_NOT_IN_STEP, "back to NOT_IN_STEP after end_step");

    /* Steps are repeatable, not a one-shot. */
    EXPECT(H5Fbegin_step(fid, 0, NULL, 0) >= 0 && H5Fend_step(fid) >= 0, "a second step opens and closes");

    H5Fclose(fid);
done:
    H5Pclose(fcpl);
    H5Pclose(fapl);
}

int
main(void)
{
    hid_t vol_id;
    char  why[128] = "";

    printf("vol-stream M1 exit gate\n");

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("  FAIL  H5VL_stream_register\n");
        return 1;
    }

    /* Native, no connector at all -- the reference. */
    if (write_file("t_step_native.h5", H5I_INVALID_HID, 0, 0) < 0) {
        printf("  FAIL  writing native reference file\n");
        return 1;
    }
    /* Through the connector, no step bracketing. */
    if (write_file("t_step_plain.h5", vol_id, 1, 0) < 0) {
        printf("  FAIL  writing connector file without steps\n");
        return 1;
    }
    /* Through the connector, every write bracketed in a step. */
    if (write_file("t_step_bracketed.h5", vol_id, 1, 1) < 0) {
        printf("  FAIL  writing connector file with steps\n");
        return 1;
    }

    EXPECT(files_identical("t_step_native.h5", "t_step_plain.h5", why, sizeof(why)),
           "connector output is byte-identical to native");
    if (why[0])
        printf("        %s\n", why);

    /* M2 supersedes the M1 "step bracketing changes not one byte" assertion
     * that used to live here: bracketed writes now land under /step/<n>/,
     * so t_step_bracketed.h5 is expected to differ from t_step_plain.h5.
     * The replacement invariant -- h5diff-clean replay -- lives in
     * test/t_replay.c. The *unbracketed* byte-identity assertion above is
     * untouched and must hold forever. */

    /* Prove the comparison can fail. A byte-identity assertion is worthless if
     * the comparator would pass anything, so flip one byte in a copy and
     * require that it is caught. */
    {
        FILE *f = fopen("t_step_tampered.h5", "wb");
        FILE *g = fopen("t_step_plain.h5", "rb");
        int   c, n = 0;

        if (f && g) {
            while ((c = fgetc(g)) != EOF) {
                /* Byte 1024 is raw data, well past the superblock. */
                fputc((n == 1024) ? (c ^ 0xff) : c, f);
                n++;
            }
        }
        if (f)
            fclose(f);
        if (g)
            fclose(g);

        why[0] = '\0';
        EXPECT(!files_identical("t_step_plain.h5", "t_step_tampered.h5", why, sizeof(why)),
               "comparator detects a single flipped byte");
        printf("        detected: %s\n", why[0] ? why : "(nothing -- comparator is broken)");
    }

    check_state_machine(vol_id);

    H5VLclose(vol_id);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
