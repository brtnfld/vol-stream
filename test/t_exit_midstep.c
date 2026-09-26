/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * A writer that exits in the middle of a step, leaving open a dataset it
 * created in that step, exits normally.
 *
 * It used to crash as HDF5 shut down. The connector's H5atclose() callback
 * closed the native stand-in that H5VL_stream_get_object() returns for such
 * a dataset; closing the stand-in's file walks every open dataset ID, the
 * leftover one among them, whose stand-in was by then gone. CI saw it when
 * examples/detector_pipeline's writer returned early on an error.
 *
 * A child commits step 0, then opens step 1, creates a dataset, writes it,
 * and returns without closing anything. The parent checks the child exited
 * with its own status rather than a signal, and that the file holds step 0
 * and not the step that never committed.
 *
 * No transport needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_exit_midstep.h5"

static int
child(void)
{
    hsize_t n = 4, unlim = H5S_UNLIMITED, one = 1;
    int     v[4] = {1, 2, 3, 4};
    hid_t   fapl = H5Pcreate(H5P_FILE_ACCESS), fid, sp, dcpl, ds;

    if (H5Pset_fapl_stream(fapl, NULL) < 0 || (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (sp = H5Screate_simple(1, &n, &unlim)) < 0 || (dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
        H5Pset_chunk(dcpl, 1, &one) < 0)
        return 10;

    /* Step 0 commits. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (ds = H5Dcreate2(fid, "/committed", H5T_NATIVE_INT, sp, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, v) < 0 || H5Dclose(ds) < 0 ||
        H5Fend_step(fid) < 0)
        return 11;

    /* Step 1 never does: the writer returns with it open. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (ds = H5Dcreate2(fid, "/abandoned", H5T_NATIVE_INT, sp, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0 ||
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, v) < 0)
        return 12;
    return 3; /* HDF5 shuts down with the step, the file and ds all open */
}

int
main(void)
{
    pid_t pid;
    int   status = 0, nerrors = 0;
    hid_t nf;

    printf("vol-stream: a writer that exits mid-step exits normally\n");
    unlink(FNAME);
    fflush(stdout);

    if ((pid = fork()) < 0) {
        printf("  FAIL  fork\n");
        return 1;
    }
    if (pid == 0)
        exit(child());
    if (waitpid(pid, &status, 0) != pid) {
        printf("  FAIL  waitpid\n");
        return 1;
    }

    if (WIFSIGNALED(status)) {
        printf("  FAIL  the writer died with signal %d\n", WTERMSIG(status));
        nerrors++;
    }
    else if (!WIFEXITED(status) || WEXITSTATUS(status) != 3) {
        printf("  FAIL  the writer exited with %d, not 3 (setup failed?)\n", WEXITSTATUS(status));
        nerrors++;
    }
    else
        printf("  ok    the writer exited with its own status, not a signal\n");

    if ((nf = H5Fopen(FNAME, H5F_ACC_RDONLY, H5P_DEFAULT)) < 0) {
        printf("  FAIL  the file does not open natively\n");
        nerrors++;
    }
    else {
        htri_t has0, has1;

        H5E_BEGIN_TRY
        {
            has0 = H5Lexists(nf, "/step", H5P_DEFAULT) > 0 && H5Lexists(nf, "/step/0", H5P_DEFAULT) > 0 &&
                   H5Lexists(nf, "/step/0/committed", H5P_DEFAULT) > 0;
            has1 = H5Lexists(nf, "/step/1", H5P_DEFAULT) > 0;
        }
        H5E_END_TRY
        if (has0 && !has1)
            printf("  ok    the file holds step 0, and not the step that never committed\n");
        else {
            printf("  FAIL  step 0 %s, step 1 %s\n", has0 ? "present" : "missing", has1 ? "present" : "absent");
            nerrors++;
        }
        H5Fclose(nf);
    }
    unlink(FNAME);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
