/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * h5stream's `tail` subcommand (M9) -- following a genuinely live writer.
 *
 * This is the M9 exit gate's "follows a live writer" clause, and the only
 * subcommand that cannot be tested against a file sitting on disk: while a
 * writer holds a stream open, another process cannot read it at all (plain
 * h5ls mid-write prints "**NOT FOUND**"). tail therefore rides the
 * connector's transport, H5Fwait_step_ready(), and this test only means
 * anything if the writer is still running while the tool observes it.
 *
 * The harness is the part that had to be got right, and a first attempt got
 * it wrong in an instructive way. An ad-hoc writer that emits N steps and
 * exits destroys its SSG group before the reader has finished joining, so
 * the reader sees "Group not found", retries, and gives up -- which looks
 * exactly like a broken tool and is entirely the test's fault.
 *
 * The fix is to invert the dependency: the writer keeps producing steps
 * until the *reader* says it is done, rather than the reader racing a
 * fixed-length writer. So:
 *
 *   child   runs `h5stream tail --max-steps N`, capturing stdout, then
 *           touches DONE.
 *   parent  writes a step every WRITE_INTERVAL_US, checking DONE each time,
 *           and stops when the reader is finished or MAX_STEPS is reached.
 *
 * That also removes the "did the reader attach in time" race entirely: the
 * reader can join whenever it manages to, and steps will still be arriving.
 *
 * Requires the transport, so it is built only with VOL_STREAM_HAVE_MERCURY
 * and sets VOL_STREAM_NA the same way the other transport tests do.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_h5stream_tail.h5"
#define OUTFILE "t_h5stream_tail.out"
#define DONE_SENTINEL "t_h5stream_tail.reader_done"

/* The reader asks for this many; the writer will happily produce more while
 * waiting, which is the point. */
#define WANT_STEPS 3
#define MAX_STEPS 60
#define WRITE_INTERVAL_US 250000 /* 250ms */

static int
sentinel_exists(const char *path)
{
    FILE *f = fopen(path, "r");

    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

static void
touch_sentinel(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f);
}

/* Child: run the tool exactly as a user would, then report completion. */
static int
run_reader(const char *tool)
{
    char cmd[1024];
    int  rc;

    /* Give the writer a moment to create the file and publish its group. The
     * writer keeps going until DONE, so this only has to be long enough to
     * avoid opening before the file exists -- not long enough to catch any
     * particular step. */
    usleep(600000);

    snprintf(cmd, sizeof(cmd), "\"%s\" tail %s --max-steps %d --timeout-ms 20000 > %s 2>/dev/null", tool,
             FNAME, WANT_STEPS, OUTFILE);
    rc = system(cmd);

    touch_sentinel(DONE_SENTINEL);
    return (rc == 0) ? 0 : 1;
}

/* Parent: keep committing steps until the reader is done. */
static int
run_writer(void)
{
    hid_t vol_id, fapl, fid;
    int   s;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("writer: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("writer: FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("writer: FAIL create (transport up? VOL_STREAM_NA set?)\n");
        return 1;
    }

    for (s = 0; s < MAX_STEPS; s++) {
        char    name[32];
        hid_t   sp, ds;
        hsize_t one = 1;
        int     val = s;

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            printf("writer: FAIL begin_step %d\n", s);
            return 1;
        }
        snprintf(name, sizeof(name), "/d%d", s);
        if ((sp = H5Screate_simple(1, &one, NULL)) < 0 ||
            (ds = H5Dcreate2(fid, name, H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
            H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val) < 0) {
            printf("writer: FAIL write step %d\n", s);
            return 1;
        }
        H5Dclose(ds);
        H5Sclose(sp);
        if (H5Fend_step(fid) < 0) {
            printf("writer: FAIL end_step %d\n", s);
            return 1;
        }

        /* Stop as soon as the reader has what it needs -- but never before,
         * which is what keeps the SSG group alive under it. */
        if (sentinel_exists(DONE_SENTINEL))
            break;
        usleep(WRITE_INTERVAL_US);
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}

int
main(int argc, char **argv)
{
    pid_t pid;
    int   reader_status = 0, writer_status;
    int   nerrors = 0, steps_seen = 0;
    FILE *f;
    char  line[256];

    if (argc < 2) {
        printf("  FAIL  usage: %s <path-to-h5stream>\n", argv[0]);
        return 1;
    }

    printf("vol-stream: h5stream tail (live writer)\n");

    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(FNAME);
    unlink(FNAME ".vsgroup");
    unlink(OUTFILE);
    unlink(DONE_SENTINEL);

    fflush(NULL);
    if ((pid = fork()) < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        int rc = run_reader(argv[1]);

        fflush(NULL);
        _exit(rc);
    }

    writer_status = run_writer();

    if (waitpid(pid, &reader_status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    if (writer_status != 0) {
        printf("\nwriter reported failure\n");
        nerrors++;
    }
    if (!(WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 0)) {
        printf("  FAIL  h5stream tail exited non-zero (status=%d)\n", reader_status);
        nerrors++;
    }

    /* Count the steps the tool actually reported. */
    if (NULL == (f = fopen(OUTFILE, "r"))) {
        printf("  FAIL  tail produced no output at all\n");
        nerrors++;
    }
    else {
        while (fgets(line, sizeof(line), f))
            if (strncmp(line, "step ", 5) == 0)
                steps_seen++;
        fclose(f);

        printf("  info  tail reported %d step(s)\n", steps_seen);

        /* The load-bearing assertion. Reporting nothing is exactly what the
         * polling implementation did against a live writer -- it opened the
         * file, saw whatever was there, and never observed another step. */
        if (steps_seen >= WANT_STEPS)
            printf("  ok    tail followed a live writer and observed %d step(s)\n", steps_seen);
        else {
            printf("  FAIL  tail observed %d step(s), expected at least %d -- it is not following the "
                   "live writer\n",
                   steps_seen, WANT_STEPS);
            nerrors++;
        }
    }

    unlink(OUTFILE);
    unlink(DONE_SENTINEL);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
