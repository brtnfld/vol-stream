/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * A reader can tell a finished stream from one that has not started.
 *
 * A reader recognises the writer from its step announcements or its
 * answers; one that opens the file after the writer has gone has neither,
 * and waited forever. The writer now leaves "<file>.vsdone" when it closes
 * the stream, and removes a stale one when it starts.
 *
 *   1. A writer commits two steps and closes: the marker exists and names
 *      two steps.
 *   2. A reader opens afterwards: H5Fstep_status() reports end of stream
 *      within a few seconds, H5Fwait_step_ready() fails at once rather than
 *      waiting out its timeout, and both steps still read from the file.
 *   3. A new writer truncates the file: the marker is gone while it runs,
 *      and back when it closes.
 *
 * Each writer is its own process, so it has fully left its group. na+sm.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME  "t_writer_done.h5"
#define MARKER FNAME ".vsdone"
#define N      4

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

static double
now_s(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static hid_t
stream_fapl(void)
{
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);

    if (fapl < 0 || H5Pset_fapl_stream(fapl, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0)
        return H5I_INVALID_HID;
    return fapl;
}

/* A writer process: nsteps steps of /x, then close. With check_running, it
 * reports (exit 2) if the marker exists while it is still open. */
static int
writer(int nsteps, int check_running)
{
    hsize_t n = N;
    int     v[N] = {0}, s;
    hid_t   fapl = stream_fapl(), fid, sp, ds = H5I_INVALID_HID;

    if (fapl < 0 || (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0 ||
        (sp = H5Screate_simple(1, &n, NULL)) < 0)
        return 10;
    for (s = 0; s < nsteps; s++) {
        v[0] = s;
        if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
            (s == 0 && (ds = H5Dcreate2(fid, "/x", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0) ||
            H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, v) < 0 || H5Fend_step(fid) < 0)
            return 11;
    }
    if (check_running && access(MARKER, F_OK) == 0)
        return 2;
    H5Dclose(ds);
    H5Sclose(sp);
    H5Fclose(fid);
    H5Pclose(fapl);
    return 0;
}

static int
run_writer(int nsteps, int check_running)
{
    pid_t pid;
    int   status = 0;

    fflush(NULL);
    if ((pid = fork()) < 0)
        return -1;
    if (pid == 0)
        _exit(writer(nsteps, check_running));
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static int
marker_says(int nsteps)
{
    char  line[256] = "", want[64];
    FILE *f         = fopen(MARKER, "r");

    if (!f)
        return 0;
    if (!fgets(line, sizeof(line), f))
        line[0] = '\0';
    fclose(f);
    snprintf(want, sizeof(want), "after %d committed step", nsteps);
    return strstr(line, want) != NULL;
}

int
main(void)
{
    hid_t             fapl, fid;
    H5F_step_status_t st  = H5F_STEP_NOT_IN_STEP;
    uint64_t          phys, wall;
    double            t0, waited;
    int               s, eos = 0, rc, got[N], reads_ok = 1;

    printf("vol-stream: a reader can tell a finished stream from one that has not started (na+sm)\n");
    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(FNAME);
    unlink(FNAME ".vsgroup");
    unlink(MARKER);

    /* 1. */
    rc = run_writer(2, 0);
    CHECK(rc == 0 && access(MARKER, F_OK) == 0 && marker_says(2),
          "a writer that closes leaves %s, naming its 2 committed steps", MARKER);

    /* 2. */
    if ((fapl = stream_fapl()) < 0 || (fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("  FAIL  a reader cannot open the finished stream\n");
        return 1;
    }
    t0 = now_s();
    while (now_s() - t0 < 5.0) {
        if (H5Fstep_status(fid, &st) >= 0 && st == H5F_STEP_EOS) {
            eos = 1;
            break;
        }
        usleep(100000);
    }
    CHECK(eos, "a reader opening after the writer left reports the end of the stream (%.1f s)", now_s() - t0);
    t0 = now_s();
    H5E_BEGIN_TRY
    {
        rc = (int)H5Fwait_step_ready(fid, 10000, &phys, &wall);
    }
    H5E_END_TRY
    waited = now_s() - t0;
    CHECK(rc < 0 && waited < 3.0, "and H5Fwait_step_ready() fails at once (%.1f s of a 10 s timeout)", waited);
    for (s = 0; s < 2; s++) {
        hid_t ds;

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || (ds = H5Dopen2(fid, "/x", H5P_DEFAULT)) < 0 ||
            H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) < 0 || got[0] != s) {
            reads_ok = 0;
            break;
        }
        H5Dclose(ds);
    }
    CHECK(reads_ok, "both steps still read from the file");
    H5Fclose(fid);
    H5Pclose(fapl);

    /* 3. */
    rc = run_writer(1, 1);
    CHECK(rc == 0, "a new writer of the file removes the old marker while it runs%s",
          rc == 2 ? " (it was still there)" : "");
    CHECK(access(MARKER, F_OK) == 0 && marker_says(1), "and leaves a fresh one, naming its 1 step, when it closes");

    unlink(FNAME);
    unlink(FNAME ".vsgroup");
    unlink(MARKER);
    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
