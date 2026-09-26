/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Variable-length data reaches subscribers.
 *
 * Its in-memory form is pointers (char * for a VL string, hvl_t for a VL
 * sequence), meaningless in another process, so the writer pushes the
 * serialized form capture already made, and H5Fget_subscribed_data() decodes
 * it into one allocation -- the pointer array first, the bytes after it --
 * which the caller frees with one free().
 *
 * One step writes /vl (VL strings, one of them NULL and one empty), /seq (VL
 * int sequences of lengths 0..3) and /ints; the reader subscribes to all
 * three, whole. Each must arrive exact, every pointer must point inside the
 * returned buffer, and a whole-object subscription must report no fallback.
 *
 * Two processes, na+sm, same shape as test/t_subscribe.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define N     4

static const char *STRS[N] = {"a", "", NULL, "dddd"};
#define FNAME "t_vl_push.h5"

#define READY_SENTINEL "t_vl_push.reader_ready"
#define DONE_SENTINEL  "t_vl_push.reader_done"

static int
wait_for(const char *path, int tenths)
{
    int i;

    for (i = 0; i < tenths; i++) {
        if (access(path, F_OK) == 0)
            return 0;
        usleep(100000);
    }
    return -1;
}

static void
touch(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f);
}

static int
run_reader(void)
{
    hid_t   vol_id, fapl, fid, space;
    hsize_t dims = N;
    int     got_ints = 0, got_vl = 0, got_seq = 0, rc = 0;

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL setup\n");
        return 1;
    }
    if (wait_for(FNAME ".vsgroup", 100) < 0 || (fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open/join\n");
        return 1;
    }
    if ((space = H5Screate_simple(1, &dims, NULL)) < 0) {
        printf("reader: FAIL dataspace\n");
        return 1;
    }
    {
        const char *paths[3]  = {"/vl", "/seq", "/ints"};
        const hid_t spaces[3] = {space, space, space};

        if (H5Fsubscribe(fid, 3, paths, spaces, NULL) < 0) {
            printf("reader: FAIL subscribe\n");
            return 1;
        }
    }
    H5Sclose(space);
    touch(READY_SENTINEL);

    /* Everything a step pushes is queued before its step-ready, so once the
     * step is announced, whatever is not queued was never sent. */
    {
        uint64_t phys = 0, wall = 0;

        if (H5Fwait_step_ready(fid, 20000, &phys, &wall) < 0) {
            printf("  FAIL  the step was never announced\n");
            rc = 1;
        }
    }
    for (;;) {
        uint64_t phys = 0, es = 0, ec = 0;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;
        uint32_t flags = 0xffffffffu;

        if (H5Fget_subscribed_data(fid, 0, &phys, &path, &buf, &size, &es, &ec, &flags) < 0)
            break;
        if (path && strcmp(path, "/vl") == 0) {
            char **v   = (char **)buf;
            int    ok  = size == N * sizeof(char *) && es == 0 && ec == N && flags == 0;
            int    k;

            for (k = 0; ok && k < N; k++) {
                const char *want = STRS[k];

                if (!want)
                    ok = v[k] == NULL;
                else
                    ok = v[k] && strcmp(v[k], want) == 0 && (char *)v[k] > (char *)buf &&
                         (char *)v[k] < (char *)buf + size + 64;
            }
            got_vl = ok;
        }
        else if (path && strcmp(path, "/seq") == 0) {
            hvl_t *v  = (hvl_t *)buf;
            int    ok = size == N * sizeof(hvl_t) && es == 0 && ec == N && flags == 0;
            int    k, j;

            for (k = 0; ok && k < N; k++) {
                ok = v[k].len == (size_t)k && (k == 0 ? v[k].p == NULL : v[k].p != NULL);
                for (j = 0; ok && j < k; j++)
                    ok = ((int *)v[k].p)[j] == 10 * k + j;
            }
            got_seq = ok;
        }
        else if (path && strcmp(path, "/ints") == 0) {
            const int *v = (const int *)buf;
            int        k;

            got_ints = size == N * sizeof(int) && es == 0 && ec == N;
            for (k = 0; got_ints && k < N; k++)
                got_ints = v[k] == 10 + k;
        }
        free(path);
        free(buf); /* one free(), even for the variable-length values */
    }

    if (!got_ints) {
        printf("  FAIL  /ints did not arrive intact\n");
        rc = 1;
    }
    else
        printf("  ok    /ints arrived intact\n");
    if (!got_vl) {
        printf("  FAIL  the variable-length strings /vl did not arrive exact (NULL, empty and all)\n");
        rc = 1;
    }
    else
        printf("  ok    /vl arrived: VL strings, NULL and empty kept apart, in one buffer, no fallback flags\n");
    if (!got_seq) {
        printf("  FAIL  the variable-length sequences /seq did not arrive exact\n");
        rc = 1;
    }
    else
        printf("  ok    /seq arrived: VL int sequences of lengths 0-3, in one buffer\n");

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    touch(DONE_SENTINEL);
    return rc;
}

static int
run_writer(void)
{
    hid_t       vol_id, fapl, fid, space, strtype, seqtype, vl_ds, seq_ds, int_ds;
    hsize_t     dims = N;
    int         ints[N] = {10, 11, 12, 13};
    int         data[N][N];
    hvl_t       seqs[N];
    int         k, j;

    for (k = 0; k < N; k++) {
        for (j = 0; j < k; j++)
            data[k][j] = 10 * k + j;
        seqs[k].len = (size_t)k;
        seqs[k].p   = k ? data[k] : NULL;
    }

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 || H5Pset_file_locking(fapl, false, true) < 0 ||
        (fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("writer: FAIL setup (transport up?)\n");
        return 1;
    }
    if (wait_for(READY_SENTINEL, 300) < 0) {
        printf("writer: FAIL reader never subscribed\n");
        return 1;
    }
    if ((space = H5Screate_simple(1, &dims, NULL)) < 0 || (strtype = H5Tcopy(H5T_C_S1)) < 0 ||
        H5Tset_size(strtype, H5T_VARIABLE) < 0 || H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        (vl_ds = H5Dcreate2(fid, "/vl", strtype, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(vl_ds, strtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, STRS) < 0 ||
        (seqtype = H5Tvlen_create(H5T_NATIVE_INT)) < 0 ||
        (seq_ds = H5Dcreate2(fid, "/seq", seqtype, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(seq_ds, seqtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, seqs) < 0 ||
        (int_ds = H5Dcreate2(fid, "/ints", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
        H5Dwrite(int_ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, ints) < 0 || H5Fend_step(fid) < 0) {
        printf("writer: FAIL write\n");
        return 1;
    }
    H5Dclose(vl_ds);
    H5Dclose(seq_ds);
    H5Dclose(int_ds);
    H5Tclose(seqtype);
    H5Tclose(strtype);
    H5Sclose(space);

    wait_for(DONE_SENTINEL, 300);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}

static void
clean(void)
{
    unlink(READY_SENTINEL);
    unlink(DONE_SENTINEL);
    unlink(FNAME ".vsgroup");
    unlink(FNAME ".vsdone");
    unlink(FNAME);
}

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status, nerrors = 0;

    printf("vol-stream: a variable-length dataset is not pushed (na+sm)\n");
    setenv("VOL_STREAM_NA", "na+sm", 0);
    clean();

    fflush(NULL);
    if ((pid = fork()) < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        int rc = run_reader();

        fflush(NULL);
        _exit(rc);
    }
    writer_status = run_writer();
    if (waitpid(pid, &reader_status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    clean();

    if (writer_status != 0) {
        printf("\nwriter process reported failure\n");
        nerrors++;
    }
    if (!(WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 0)) {
        printf("\nreader process reported failure (status=%d)\n", reader_status);
        nerrors++;
    }
    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
