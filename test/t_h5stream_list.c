/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * h5stream's `list` subcommand (M9).
 *
 * Writes a stream whose shape is known exactly -- three steps, different
 * objects in each, and explicit logical ids -- then runs the tool over it and
 * checks what it prints.
 *
 * What is asserted, and why each matters:
 *   1. The step count is right, and each step lists exactly the objects
 *      written into it. A tool that reported the union across steps, or lost
 *      the per-step grouping, would still look plausible.
 *   2. ".manifest" and ".payload" do NOT appear. They are how the connector
 *      records a step, not anything the application wrote, and showing them
 *      would be actively misleading about the file's contents.
 *   3. The logical ids are reported. That is the one thing only the connector
 *      can answer -- h5dump can see the step groups, but not which logical
 *      iteration each carries.
 *
 * The tool is invoked as a subprocess and its stdout parsed, which is what a
 * user actually experiences; checking the library calls it makes instead
 * would not catch a formatting or grouping mistake.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_h5stream_list.h5"
#define OUTFILE "t_h5stream_list.out"

static int nerrors = 0;

static int
make_dataset(hid_t fid, const char *name, int value)
{
    hid_t   sp, ds;
    hsize_t one = 1;

    if ((sp = H5Screate_simple(1, &one, NULL)) < 0)
        return -1;
    if ((ds = H5Dcreate2(fid, name, H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        H5Sclose(sp);
        return -1;
    }
    if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value) < 0) {
        H5Dclose(ds);
        H5Sclose(sp);
        return -1;
    }
    H5Dclose(ds);
    H5Sclose(sp);
    return 0;
}

/* Whether `needle` appears anywhere in the captured tool output. */
static int
contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

int
main(int argc, char **argv)
{
    hid_t    vol_id, fapl, fid;
    char     cmd[1024];
    char    *out;
    long     len;
    FILE    *f;
    uint64_t lids0[1] = {100};
    uint64_t lids1[1] = {200};
    uint64_t lids2[1] = {300};

    if (argc < 2) {
        printf("  FAIL  usage: %s <path-to-h5stream>\n", argv[0]);
        return 1;
    }

    printf("vol-stream: h5stream list\n");

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

    /* Three steps, deliberately different contents, explicit logical ids. */
    if (H5Fbegin_step(fid, 1, lids0, 0) < 0 || make_dataset(fid, "/alpha", 1) < 0 ||
        make_dataset(fid, "/beta", 2) < 0 || H5Fend_step(fid) < 0) {
        printf("  FAIL  step 0\n");
        return 1;
    }
    if (H5Fbegin_step(fid, 1, lids1, 0) < 0 || make_dataset(fid, "/gamma", 3) < 0 ||
        H5Fend_step(fid) < 0) {
        printf("  FAIL  step 1\n");
        return 1;
    }
    if (H5Fbegin_step(fid, 1, lids2, 0) < 0 || make_dataset(fid, "/delta", 4) < 0 ||
        H5Fend_step(fid) < 0) {
        printf("  FAIL  step 2\n");
        return 1;
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    /* Run the tool exactly as a user would, and capture stdout. */
    snprintf(cmd, sizeof(cmd), "\"%s\" list %s > %s 2>/dev/null", argv[1], FNAME, OUTFILE);
    if (system(cmd) != 0) {
        printf("  FAIL  h5stream list exited non-zero\n");
        return 1;
    }
    if (NULL == (f = fopen(OUTFILE, "rb"))) {
        printf("  FAIL  no tool output\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (NULL == (out = (char *)malloc((size_t)len + 1))) {
        fclose(f);
        printf("  FAIL  out of memory\n");
        return 1;
    }
    if (fread(out, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(out);
        printf("  FAIL  reading tool output\n");
        return 1;
    }
    out[len] = '\0';
    fclose(f);

    if (contains(out, "3 step(s)"))
        printf("  ok    reports the right number of steps\n");
    else {
        printf("  FAIL  step count not reported as 3\n");
        nerrors++;
    }

    /* Per-step grouping: each object under its own step heading. */
    {
        const char *s0 = strstr(out, "step 0:");
        const char *s1 = strstr(out, "step 1:");
        const char *s2 = strstr(out, "step 2:");

        if (!s0 || !s1 || !s2) {
            printf("  FAIL  not all three steps appear\n");
            nerrors++;
        }
        else {
            int ok = 1;

            /* alpha and beta belong to step 0 only: they must appear between
             * the step 0 and step 1 headings. */
            if (!(strstr(s0, "alpha") && strstr(s0, "alpha") < s1)) {
                printf("  FAIL  /alpha not listed under step 0\n");
                ok = 0;
            }
            if (!(strstr(s0, "beta") && strstr(s0, "beta") < s1)) {
                printf("  FAIL  /beta not listed under step 0\n");
                ok = 0;
            }
            if (!(strstr(s1, "gamma") && strstr(s1, "gamma") < s2)) {
                printf("  FAIL  /gamma not listed under step 1\n");
                ok = 0;
            }
            if (!strstr(s2, "delta")) {
                printf("  FAIL  /delta not listed under step 2\n");
                ok = 0;
            }
            if (ok)
                printf("  ok    each object is listed under the step that wrote it\n");
            else
                nerrors++;
        }
    }

    /* The connector's bookkeeping must not be presented as content. */
    if (contains(out, ".manifest") || contains(out, ".payload")) {
        printf("  FAIL  .manifest/.payload shown as file contents -- they are the connector's own "
               "bookkeeping\n");
        nerrors++;
    }
    else
        printf("  ok    connector bookkeeping is not listed as content\n");

    /* Logical ids: the part h5dump could not tell you. */
    if (contains(out, "100") && contains(out, "200") && contains(out, "300"))
        printf("  ok    logical step ids are reported\n");
    else {
        printf("  FAIL  logical ids 100/200/300 not all reported\n");
        nerrors++;
    }

    free(out);
    unlink(OUTFILE);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
