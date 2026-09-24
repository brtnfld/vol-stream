/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Library calls that walk open objects while a step is open.
 *
 * A dataset created in the open step has no native object until the step
 * commits. HDF5 still asks for one whenever it walks the open dataset IDs --
 * the flush a file close runs, object counts, a second handle's close -- and
 * dereferenced the NULL it used to get, crashing. And closing the file with
 * that dataset still open discarded the step under it, so the dataset's own
 * close then wrote into freed step state and crashed too.
 *
 * Each case runs in a child process, so a crash is reported as a failure
 * rather than ending the test. No transport needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_open_step_objects.h5"

static int nerrors = 0;

/* A file with step 0 open and dataset /x created in it, still open. */
static hid_t
open_step(hid_t *ds)
{
    hid_t   fapl = H5Pcreate(H5P_FILE_ACCESS), fid, sp;
    hsize_t n    = 4;

    if (H5Pset_fapl_stream(fapl, NULL) < 0 || (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (sp = H5Screate_simple(1, &n, NULL)) < 0 || H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (*ds = H5Dcreate2(fid, "/x", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0)
        return H5I_INVALID_HID;
    H5Sclose(sp);
    H5Pclose(fapl);
    return fid;
}

/* Run one case in a child. want: 1 if the case's own result must be success. */
static void
run(const char *what, int which)
{
    pid_t pid;
    int   st = 0;

    fflush(NULL);
    if ((pid = fork()) == 0) {
        hid_t ds, fid = open_step(&ds);
        int   ok      = fid >= 0;

        H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
        if (ok && which == 0) /* the flush a file close runs, with /x still open */
            ok = H5Fclose(fid) >= 0 && H5Dclose(ds) >= 0;
        else if (ok && which == 1)
            ok = H5Fget_obj_count(fid, H5F_OBJ_ALL) >= 1;
        else if (ok && which == 2) {
            hid_t ids[4];

            ok = H5Fget_obj_ids(fid, H5F_OBJ_DATASET, 4, ids) >= 0;
        }
        else if (ok && which == 3)
            ok = H5Fflush(fid, H5F_SCOPE_LOCAL) >= 0;
        else if (ok && which == 4) {
            /* A second, native handle to the same file in the same process
             * is not supported, and its close fails -- but must not crash. */
            hid_t nf = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT);

            if (nf >= 0)
                H5Fclose(nf);
        }
        _exit(ok ? 0 : 1);
    }
    if (pid < 0 || waitpid(pid, &st, 0) < 0) {
        printf("  FAIL  %s: could not run\n", what);
        nerrors++;
    }
    else if (WIFSIGNALED(st)) {
        printf("  FAIL  %s: crashed (signal %d)\n", what, WTERMSIG(st));
        nerrors++;
    }
    else if (WEXITSTATUS(st) != 0) {
        printf("  FAIL  %s: failed\n", what);
        nerrors++;
    }
    else
        printf("  ok    %s\n", what);
    unlink(FNAME);
}

int
main(void)
{
    printf("vol-stream: library walks of open objects while a step is open\n");
    run("H5Fclose() with a dataset created in the open step still open, then its close", 0);
    run("H5Fget_obj_count() mid-step", 1);
    run("H5Fget_obj_ids(H5F_OBJ_DATASET) mid-step", 2);
    run("H5Fflush(H5F_SCOPE_LOCAL) mid-step", 3);
    run("a second native handle opened and closed mid-step does not crash", 4);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
