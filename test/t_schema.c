/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M10 exit gate: a reader that knows *nothing* about a running stream
 * discovers what it carries and subscribes using only what discovery told
 * it.
 *
 * Every other transport test in this suite hard-codes the writer's shape on
 * both sides -- t_subscribe.c's NSUB, examples/heat_diffusion's grid size
 * passed to both programs on the command line -- because until now there was
 * no other way: H5Fsubscribe() takes a dataspace, and a reader had to invent
 * one. That is workable for a purpose-built monitor and impossible for a
 * generic consumer (a visualization tool has to populate a variable list
 * before a user can pick anything from it), which is what this closes.
 *
 * So the reader here deliberately declares no dimensions, no element count
 * and no datatype of its own. It asks H5Fget_stream_schema() and uses the
 * ids that come back.
 *
 *   1. The writer commits step 0: /temp (2-D, chunked, unlimited in
 *      dimension 0), an attribute on it, and /aux.
 *   2. The reader asks for the schema and checks all three appear, with the
 *      right classes, rank and extent -- and that the attribute is reported
 *      as one.
 *   3. It subscribes to /temp using the *discovered* dataspace, and
 *      validates the pushed bytes through the *discovered* datatype.
 *   4. The writer then resizes /temp and adds /extra. The reader asks again
 *      and requires the schema to have followed both -- the change-detection
 *      half, which is what keeps a steady stream from republishing every
 *      step.
 *
 * Only compiled/run when VOL_STREAM_HAVE_MERCURY is on; see
 * test/CMakeLists.txt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define FNAME "t_schema.h5"

#define TEMP_PATH  "/temp"
#define ATTR_PATH  "/temp@units"
#define AUX_PATH   "/aux"
#define EXTRA_PATH "/extra"

#define TEMP_ROWS0 4 /* /temp is TEMP_ROWS0 x TEMP_COLS in step 0 ... */
#define TEMP_ROWS1 6 /* ... and resized to TEMP_ROWS1 x TEMP_COLS later */
#define TEMP_COLS  3
#define NAUX       5

#define STEP0_DONE_SENTINEL  "t_schema.step0_done"
#define SUBSCRIBED_SENTINEL  "t_schema.subscribed"
#define WRITES_DONE_SENTINEL "t_schema.writes_done"
#define READER_DONE_SENTINEL "t_schema.reader_done"

static int nerrors_g = 0;

static void
check(int cond, const char *what)
{
    if (cond)
        printf("  ok    %s\n", what);
    else {
        printf("  FAIL  %s\n", what);
        nerrors_g++;
    }
}

static int
wait_for_sentinel(const char *path, int max_iters)
{
    int i;

    for (i = 0; i < max_iters; i++) {
        FILE *f = fopen(path, "r");

        if (f) {
            fclose(f);
            return 0;
        }
        usleep(100000);
    }
    return -1;
}

static void
touch_sentinel(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f);
}

/* The schema is a set, not a list in any promised order -- the writer
 * publishes paths in first-capture order, which is an implementation
 * detail no consumer should depend on. */
static const H5F_stream_var_t *
find_var(const H5F_stream_var_t *vars, size_t n, const char *path)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (vars[i].path && 0 == strcmp(vars[i].path, path))
            return &vars[i];
    return NULL;
}

static int
run_reader(void)
{
    hid_t             vol_id, fapl, fid;
    H5F_stream_var_t *vars = NULL;
    size_t            n_vars = 0;
    uint64_t          schema_step = 0;
    int               i;
    size_t            temp_elems = 0; /* everything below is *discovered* */
    size_t            temp_esize = 0;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("reader: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL fapl\n");
        return 1;
    }
    /* The group sidecar, not the file: only its existence guarantees a
     * durable superblock to open (see t_subscribe.c's matching wait). */
    for (i = 0; i < 100; i++) {
        FILE *f = fopen(FNAME ".vsgroup", "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000);
    }
    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open %s\n", FNAME);
        return 1;
    }

    /* 15 s rather than a poll loop: the call retries internally while the
     * writer has committed nothing yet, which is the whole reason it takes a
     * timeout. */
    if (H5Fget_stream_schema(fid, 15000, &schema_step, &n_vars, &vars) < 0) {
        printf("reader: FAIL get_stream_schema (writer running? VOL_STREAM_NA set?)\n");
        return 1;
    }

    printf("reader: writer published %zu variable(s) as of step %llu\n", n_vars,
           (unsigned long long)schema_step);
    for (i = 0; i < (int)n_vars; i++) {
        int      rank = H5Sget_simple_extent_ndims(vars[i].space_id);
        hsize_t  dims[8];
        char     shape[64];
        int      d;
        size_t   used = 0;

        shape[0] = '\0';
        if (rank > 0 && rank <= 8 && H5Sget_simple_extent_dims(vars[i].space_id, dims, NULL) >= 0)
            for (d = 0; d < rank; d++)
                used += (size_t)snprintf(shape + used, sizeof(shape) - used, "%s%llu", d ? "x" : "",
                                         (unsigned long long)dims[d]);
        else
            snprintf(shape, sizeof(shape), "scalar");
        printf("        %-14s %s %s (%zu bytes/element)\n", vars[i].path,
               vars[i].is_attr ? "attribute" : "dataset  ", shape, H5Tget_size(vars[i].type_id));
    }

    check(n_vars == 3, "schema lists exactly the three objects the step wrote");

    {
        const H5F_stream_var_t *temp = find_var(vars, n_vars, TEMP_PATH);
        const H5F_stream_var_t *attr = find_var(vars, n_vars, ATTR_PATH);
        const H5F_stream_var_t *aux  = find_var(vars, n_vars, AUX_PATH);
        hsize_t                 dims[2];

        check(temp != NULL, "schema carries " TEMP_PATH);
        check(attr != NULL, "schema carries the attribute " ATTR_PATH);
        check(aux != NULL, "schema carries " AUX_PATH);
        if (!temp || !attr || !aux)
            return 1;

        check(attr->is_attr == 1 && temp->is_attr == 0, "an attribute is reported as an attribute");
        check(H5Tget_class(temp->type_id) == H5T_FLOAT, TEMP_PATH " is reported as floating point");
        check(H5Tget_class(aux->type_id) == H5T_INTEGER, AUX_PATH " is reported as integer");
        check(H5Sget_simple_extent_ndims(temp->space_id) == 2, TEMP_PATH " is reported as 2-D");
        check(H5Sget_simple_extent_dims(temp->space_id, dims, NULL) >= 0 && dims[0] == TEMP_ROWS0 &&
                  dims[1] == TEMP_COLS,
              "the reported extent is the writer's own");
        /* A schema describes the object, so its dataspace must be usable as
         * one -- a selection left over from whichever write last touched the
         * path would silently narrow every consumer that copied it. */
        check(H5Sget_select_npoints(temp->space_id) == (hssize_t)(TEMP_ROWS0 * TEMP_COLS),
              "the reported dataspace has everything selected, not one write's selection");

        temp_elems = (size_t)(TEMP_ROWS0 * TEMP_COLS);
        temp_esize = H5Tget_size(temp->type_id);

        /* The point of the exercise: subscribe with the discovered
         * dataspace. Nothing in this process ever named a dimension. */
        {
            const char *paths[1] = {TEMP_PATH};
            hid_t       spaces[1];

            spaces[0] = temp->space_id;
            if (H5Fsubscribe(fid, 1, paths, spaces, NULL) < 0) {
                printf("reader: FAIL subscribe using the discovered dataspace\n");
                return 1;
            }
        }
    }

    H5Ffree_stream_schema(n_vars, vars);
    vars   = NULL;
    n_vars = 0;
    touch_sentinel(SUBSCRIBED_SENTINEL);

    {
        uint64_t phys = 0, elem_start = 0, elem_count = 0;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;

        if (H5Fget_subscribed_data(fid, 20000, &phys, &path, &buf, &size, &elem_start, &elem_count) < 0) {
            printf("reader: FAIL no data pushed for the discovered subscription\n");
            return 1;
        }
        check(path && 0 == strcmp(path, TEMP_PATH), "the push is for the subscribed path");
        check(elem_count == temp_elems && size == temp_elems * temp_esize,
              "the push covers exactly what the schema said the object was");

        /* Decoded through the discovered datatype, not a compiled-in one. */
        if (size == temp_elems * temp_esize && temp_esize == sizeof(double)) {
            const double *vals = (const double *)buf;
            size_t        k;
            int           ok = 1;

            for (k = 0; k < temp_elems; k++)
                if (vals[k] != (double)k * 1.5)
                    ok = 0;
            check(ok, "the pushed values are the writer's own");
        }
        free(path);
        free(buf);
    }

    if (wait_for_sentinel(WRITES_DONE_SENTINEL, 200) < 0) {
        printf("reader: FAIL writer never signalled writes done\n");
        return 1;
    }

    /* The second half: a schema is republished when it changes, and only
     * then. Both changes here are the kinds that matter to a consumer -- an
     * object appearing, and one changing shape under it. */
    {
        uint64_t schema_step2 = 0;

        if (H5Fget_stream_schema(fid, 15000, &schema_step2, &n_vars, &vars) < 0) {
            printf("reader: FAIL second get_stream_schema\n");
            return 1;
        }
        {
            const H5F_stream_var_t *temp  = find_var(vars, n_vars, TEMP_PATH);
            const H5F_stream_var_t *extra = find_var(vars, n_vars, EXTRA_PATH);
            hsize_t                 dims[2];

            check(n_vars == 4 && extra != NULL, "a path created in a later step joins the schema");
            check(temp && H5Sget_simple_extent_dims(temp->space_id, dims, NULL) >= 0 &&
                      dims[0] == TEMP_ROWS1 && dims[1] == TEMP_COLS,
                  "a resized object is republished at its new extent");
            check(schema_step2 > schema_step, "the republished schema is stamped with the later step");
        }
        H5Ffree_stream_schema(n_vars, vars);
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    /* Leave before the writer destroys the group -- see t_subscribe.c. */
    touch_sentinel(READER_DONE_SENTINEL);
    return nerrors_g ? 1 : 0;
}

static int
run_writer(void)
{
    hid_t   vol_id, fapl, fid, dcpl;
    hid_t   temp_space, aux_space, temp_ds;
    hsize_t temp_dims[2]    = {TEMP_ROWS0, TEMP_COLS};
    hsize_t temp_maxdims[2] = {H5S_UNLIMITED, TEMP_COLS};
    hsize_t chunk[2]        = {2, TEMP_COLS};
    hsize_t aux_dims        = NAUX;
    double  temp_vals[TEMP_ROWS1 * TEMP_COLS];
    int     aux_vals[NAUX];
    int     i;

    unlink(FNAME ".vsgroup");
    unlink(FNAME);

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

    for (i = 0; i < TEMP_ROWS1 * TEMP_COLS; i++)
        temp_vals[i] = (double)i * 1.5;
    for (i = 0; i < NAUX; i++)
        aux_vals[i] = i * 7;

    if ((temp_space = H5Screate_simple(2, temp_dims, temp_maxdims)) < 0 ||
        (aux_space = H5Screate_simple(1, &aux_dims, NULL)) < 0 ||
        (dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0 || H5Pset_chunk(dcpl, 2, chunk) < 0) {
        printf("writer: FAIL create dataspaces\n");
        return 1;
    }

    /* Step 0 -- the step the reader discovers. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("writer: FAIL begin_step 0\n");
        return 1;
    }
    if ((temp_ds = H5Dcreate2(fid, TEMP_PATH, H5T_NATIVE_DOUBLE, temp_space, H5P_DEFAULT, dcpl,
                              H5P_DEFAULT)) < 0 ||
        H5Dwrite(temp_ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, temp_vals) < 0) {
        printf("writer: FAIL write " TEMP_PATH "\n");
        return 1;
    }
    {
        hid_t attr_space, attr;
        int   units = 1;

        if ((attr_space = H5Screate(H5S_SCALAR)) < 0 ||
            (attr = H5Acreate2(temp_ds, "units", H5T_NATIVE_INT, attr_space, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
            H5Awrite(attr, H5T_NATIVE_INT, &units) < 0) {
            printf("writer: FAIL write " ATTR_PATH "\n");
            return 1;
        }
        H5Aclose(attr);
        H5Sclose(attr_space);
    }
    {
        hid_t aux_ds;

        if ((aux_ds = H5Dcreate2(fid, AUX_PATH, H5T_NATIVE_INT, aux_space, H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT)) < 0 ||
            H5Dwrite(aux_ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, aux_vals) < 0) {
            printf("writer: FAIL write " AUX_PATH "\n");
            return 1;
        }
        H5Dclose(aux_ds);
    }
    if (H5Fend_step(fid) < 0) {
        printf("writer: FAIL end_step 0\n");
        return 1;
    }
    touch_sentinel(STEP0_DONE_SENTINEL);

    if (wait_for_sentinel(SUBSCRIBED_SENTINEL, 200) < 0) {
        printf("writer: FAIL reader never subscribed\n");
        return 1;
    }

    /* Step 1 -- the data the reader validates through the discovered type. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
        H5Dwrite(temp_ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, temp_vals) < 0 ||
        H5Fend_step(fid) < 0) {
        printf("writer: FAIL step 1\n");
        return 1;
    }

    /* Step 2 -- both kinds of schema change at once: an object that did not
     * exist before, and one that changed shape. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
        printf("writer: FAIL begin_step 2\n");
        return 1;
    }
    {
        hsize_t grown[2] = {TEMP_ROWS1, TEMP_COLS};
        hid_t   extra_ds;

        if (H5Dset_extent(temp_ds, grown) < 0 ||
            H5Dwrite(temp_ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, temp_vals) < 0) {
            printf("writer: FAIL resize " TEMP_PATH "\n");
            return 1;
        }
        if ((extra_ds = H5Dcreate2(fid, EXTRA_PATH, H5T_NATIVE_INT, aux_space, H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT)) < 0 ||
            H5Dwrite(extra_ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, aux_vals) < 0) {
            printf("writer: FAIL write " EXTRA_PATH "\n");
            return 1;
        }
        H5Dclose(extra_ds);
    }
    if (H5Fend_step(fid) < 0) {
        printf("writer: FAIL end_step 2\n");
        return 1;
    }
    touch_sentinel(WRITES_DONE_SENTINEL);

    H5Dclose(temp_ds);
    H5Sclose(temp_space);
    H5Sclose(aux_space);
    H5Pclose(dcpl);

    if (wait_for_sentinel(READER_DONE_SENTINEL, 200) < 0)
        printf("writer: reader never signalled done (proceeding to close anyway)\n");

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    return 0;
}

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status;
    int   nerrors        = 0;

    printf("vol-stream M10: live schema discovery\n");

    /* A default, not a requirement -- see t_subscribe.c's own note. */
    setenv("VOL_STREAM_NA", "na+sm", 0);
    /* Before the fork, not inside run_writer(): the reader waits for the
     * rendezvous sidecar to appear, and a sidecar left behind by a previous
     * run of this test satisfies that wait instantly -- pointing the reader
     * at a group whose writer is long gone, and at a file the current writer
     * is about to unlink. Measured on a rerun-in-place loop: 4 failures in
     * 12 with the unlink inside run_writer(), where the child can win the
     * race against its own parent's cleanup. Clearing them here removes the
     * race rather than narrowing it. */
    unlink(FNAME);
    unlink(FNAME ".vsgroup");
    unlink(STEP0_DONE_SENTINEL);
    unlink(SUBSCRIBED_SENTINEL);
    unlink(WRITES_DONE_SENTINEL);
    unlink(READER_DONE_SENTINEL);

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
    unlink(STEP0_DONE_SENTINEL);
    unlink(SUBSCRIBED_SENTINEL);
    unlink(WRITES_DONE_SENTINEL);
    unlink(READER_DONE_SENTINEL);

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
