/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M6 exit gate (first increment -- see docs/dev-plan.md's M6 section and
 * the scope note in H5VL__stream_replay_step()'s own comment in
 * src/H5VLstream.c): a collective writer job and a separate reader job,
 * run with DIFFERENT (coprime) rank counts over the same file, via
 * test/run_parallel_test.sh.
 *
 * Every writer rank collectively creates the same set of datasets (the
 * ordinary parallel-HDF5 pattern -- all ranks call H5Dcreate2() with
 * matching name/type/global-space), each contributing its own contiguous
 * hyperslab via an independent H5Dwrite(). H5Fbegin_step()/H5Fend_step()
 * are collective over MPI_COMM_WORLD (see H5VL__stream_detect_mpi_comm()).
 *
 * The reader job -- a different rank count, so its own decomposition of
 * the global dataset does not line up with any single writer rank's slice
 * -- uses M3's existing reader machinery unmodified (H5Fbegin_step() to
 * advance, H5Dopen2() with a bare name resolving through /step/<n>/) and
 * verifies every element against the exact value the formula in
 * do_write() put there. That is the "byte-exact... coprime rank counts"
 * proof: no h5diff/native-reference comparison needed on top of it, since
 * checking every value against a closed-form expected result is a more
 * direct exactness check than a file-level diff would add.
 *
 * M6.5 (docs/dev-plan.md): each writer rank r ALSO creates its own
 * genuinely private dataset "priv<r>_<step>" -- a name no other rank ever
 * touches -- alongside the shared one above. That is heterogeneous per-rank
 * object creation, which needs H5VL__stream_replay_step_parallel()'s
 * cross-rank manifest aggregation (see its comment): under M6's own
 * first-increment scope, every rank had to create the identical object set
 * for ordinary parallel-HDF5 collective-create semantics to suffice on
 * their own. The reader job's rank 0 verifies every private dataset from
 * every writer rank still exists and is correct, on top of the shared
 * dataset's own M×N redistribution check.
 *
 * The Subfiling-style I/O-concentrator aggregation topology has also
 * landed (see H5VL__stream_replay_concentrated_writes()'s comment), but it
 * is opt-in via VOL_STREAM_CONCENTRATION and off by default, so this file
 * itself needs no changes to exercise it -- test/run_parallel_test.sh's
 * --concentration flag sets the env var around the write phase only and
 * checks the concentrator actually logged aggregating another rank's
 * writes. Still out of scope: H5Sselect_project_intersection-based reader
 * resolution -- the concentrator topology only changes which rank issues
 * each write, not what a reader does.
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NSTEPS 2

/* Deterministic per-(step, writer-rank, local-index) value -- both write
 * and read sides compute this the same way, so a reader with an unrelated
 * decomposition can still check every element exactly. */
/* Even-as-possible 1-D decomposition: with a remainder, the first
 * (global % nranks) ranks each take one extra element. Used by both phases,
 * so writers and readers can disagree about rank count without either having
 * to divide evenly -- which is the realistic M x N case, and was previously
 * refused outright ("global_size N not divisible by R ranks"). */
static void
slab_of(int global, int nranks, int r, int *start, int *count)
{
    int base = global / nranks;
    int rem  = global % nranks;

    *count = base + (r < rem ? 1 : 0);
    *start = r * base + (r < rem ? r : rem);
}

/* Inverse of slab_of(): which rank owns global index g, and its local offset
 * within that rank's slab. The reader needs this to know which writer
 * produced a given element once the two decompositions no longer line up. */
static void
owner_of(int global, int nranks, int g, int *rank, int *local)
{
    int base     = global / nranks;
    int rem      = global % nranks;
    int boundary = rem * (base + 1); /* end of the ranks carrying an extra */

    if (g < boundary) {
        *rank  = g / (base + 1);
        *local = g % (base + 1);
    }
    else {
        int off = g - boundary;

        *rank  = rem + off / base;
        *local = off % base;
    }
}

static int
expected_value(int step, int writer_rank, int local_index)
{
    return step * 1000 + writer_rank * 100 + local_index;
}

static int
do_write(const char *fname, int global_size)
{
    int     world_rank, world_size, per_rank, s, i;
    hid_t   vol_id, fapl, fid, filespace, memspace;
    hsize_t dims[1], start[1], count[1];
    int    *buf;

    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    /* Only a rank with nothing to write would be degenerate; an uneven split
     * is fine and deliberately exercised. */
    if (global_size < world_size) {
        if (world_rank == 0)
            fprintf(stderr, "write: global_size %d smaller than %d ranks\n", global_size, world_size);
        return 1;
    }
    {
        int my_start, my_count;

        slab_of(global_size, world_size, world_rank, &my_start, &my_count);
        per_rank = my_count;
        dims[0]  = (hsize_t)global_size;
        start[0] = (hsize_t)my_start;
        count[0] = (hsize_t)my_count;
    }

    if ((vol_id = H5VL_stream_register()) < 0) {
        fprintf(stderr, "rank %d: FAIL register\n", world_rank);
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_fapl_mpio(fapl, MPI_COMM_WORLD, MPI_INFO_NULL) < 0) {
        fprintf(stderr, "rank %d: FAIL fapl\n", world_rank);
        return 1;
    }
    if ((fid = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        fprintf(stderr, "rank %d: FAIL create\n", world_rank);
        return 1;
    }

    /* Every rank creates the same dataset from the same global dataspace --
     * the standard parallel-HDF5 collective-create pattern this increment
     * relies on (see H5VL__stream_replay_step()'s comment). */
    if ((filespace = H5Screate_simple(1, dims, NULL)) < 0 || (memspace = H5Screate_simple(1, count, NULL)) < 0) {
        fprintf(stderr, "rank %d: FAIL create dataspaces\n", world_rank);
        return 1;
    }

    if (NULL == (buf = (int *)malloc(sizeof(int) * (size_t)per_rank))) {
        fprintf(stderr, "rank %d: FAIL malloc\n", world_rank);
        return 1;
    }

    for (s = 0; s < NSTEPS; s++) {
        char           name[32], priv_name[32];
        const uint64_t logical = (uint64_t)(100 + s);
        hid_t          ds, fspace_sel, priv_space, priv_ds;
        int            priv_val;

        snprintf(name, sizeof(name), "d%d", s);
        for (i = 0; i < per_rank; i++)
            buf[i] = expected_value(s, world_rank, i);

        if (H5Fbegin_step(fid, 1, &logical, (uint64_t)(s + 1) * 1000) < 0) {
            fprintf(stderr, "rank %d: FAIL begin_step %d\n", world_rank, s);
            return 1;
        }

        if ((ds = H5Dcreate2(fid, name, H5T_NATIVE_INT, filespace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) <
            0) {
            fprintf(stderr, "rank %d: FAIL create dataset %d\n", world_rank, s);
            return 1;
        }

        if ((fspace_sel = H5Scopy(filespace)) < 0 ||
            H5Sselect_hyperslab(fspace_sel, H5S_SELECT_SET, start, NULL, count, NULL) < 0) {
            fprintf(stderr, "rank %d: FAIL select hyperslab %d\n", world_rank, s);
            return 1;
        }

        if (H5Dwrite(ds, H5T_NATIVE_INT, memspace, fspace_sel, H5P_DEFAULT, buf) < 0) {
            fprintf(stderr, "rank %d: FAIL write %d\n", world_rank, s);
            return 1;
        }

        H5Sclose(fspace_sel);
        H5Dclose(ds);

        /* M6.5: a genuinely private object -- no other rank ever creates or
         * writes "priv<world_rank>_<s>", exercising cross-rank manifest
         * aggregation for real rather than just deduplicating identical
         * creates. */
        snprintf(priv_name, sizeof(priv_name), "priv%d_%d", world_rank, s);
        priv_val = world_rank * 10000 + s;
        if ((priv_space = H5Screate(H5S_SCALAR)) < 0) {
            fprintf(stderr, "rank %d: FAIL create priv dataspace %d\n", world_rank, s);
            return 1;
        }
        if ((priv_ds = H5Dcreate2(fid, priv_name, H5T_NATIVE_INT, priv_space, H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT)) < 0 ||
            H5Dwrite(priv_ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &priv_val) < 0) {
            fprintf(stderr, "rank %d: FAIL create/write priv dataset %d\n", world_rank, s);
            return 1;
        }
        H5Dclose(priv_ds);
        H5Sclose(priv_space);

        if (H5Fend_step(fid) < 0) {
            fprintf(stderr, "rank %d: FAIL end_step %d\n", world_rank, s);
            return 1;
        }
    }

    free(buf);
    H5Sclose(memspace);
    H5Sclose(filespace);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    if (world_rank == 0)
        printf("  ok    write phase: %d ranks, %d steps, global_size=%d\n", world_size, NSTEPS,
               global_size);
    return 0;
}

static int
do_read(const char *fname, int global_size, int writer_ranks)
{
    int     world_rank, world_size, per_rank, writer_per_rank, s, i, nerrors = 0, total_errors = 0;
    hid_t   vol_id, fapl, fid;
    hsize_t start[1], count[1];
    int    *buf;

    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    if (global_size < world_size) {
        if (world_rank == 0)
            fprintf(stderr, "read: global_size %d smaller than %d ranks\n", global_size, world_size);
        return 1;
    }
    {
        int my_start, my_count;

        slab_of(global_size, world_size, world_rank, &my_start, &my_count);
        per_rank = my_count;
        start[0] = (hsize_t)my_start;
        count[0] = (hsize_t)my_count;
    }
    (void)writer_per_rank; /* superseded by owner_of() below */

    if ((vol_id = H5VL_stream_register()) < 0) {
        fprintf(stderr, "rank %d: FAIL register\n", world_rank);
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_fapl_mpio(fapl, MPI_COMM_WORLD, MPI_INFO_NULL) < 0) {
        fprintf(stderr, "rank %d: FAIL fapl\n", world_rank);
        return 1;
    }
    if ((fid = H5Fopen(fname, H5F_ACC_RDONLY, fapl)) < 0) {
        fprintf(stderr, "rank %d: FAIL open\n", world_rank);
        return 1;
    }

    if (NULL == (buf = (int *)malloc(sizeof(int) * (size_t)per_rank))) {
        fprintf(stderr, "rank %d: FAIL malloc\n", world_rank);
        return 1;
    }

    for (s = 0; s < NSTEPS; s++) {
        char  name[32];
        hid_t ds, filespace, memspace;

        snprintf(name, sizeof(name), "d%d", s);

        /* M3's reader cursor -- unmodified by M6. */
        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            fprintf(stderr, "rank %d: FAIL begin_step (reader) %d\n", world_rank, s);
            return 1;
        }

        if ((ds = H5Dopen2(fid, name, H5P_DEFAULT)) < 0) {
            fprintf(stderr, "rank %d: FAIL open dataset %d\n", world_rank, s);
            return 1;
        }
        if ((filespace = H5Dget_space(ds)) < 0 ||
            H5Sselect_hyperslab(filespace, H5S_SELECT_SET, start, NULL, count, NULL) < 0 ||
            (memspace = H5Screate_simple(1, count, NULL)) < 0) {
            fprintf(stderr, "rank %d: FAIL select %d\n", world_rank, s);
            return 1;
        }
        if (H5Dread(ds, H5T_NATIVE_INT, memspace, filespace, H5P_DEFAULT, buf) < 0) {
            fprintf(stderr, "rank %d: FAIL read %d\n", world_rank, s);
            return 1;
        }

        for (i = 0; i < per_rank; i++) {
            int global_idx = (int)start[0] + i;
            int origin_writer_rank, local_i, expected;

            /* The reader's slab and the writer's need not align at all, so
             * resolve each element's producer explicitly rather than by
             * dividing -- see owner_of(). */
            owner_of(global_size, writer_ranks, global_idx, &origin_writer_rank, &local_i);
            expected = expected_value(s, origin_writer_rank, local_i);

            if (buf[i] != expected) {
                fprintf(stderr, "  FAIL  reader rank %d step %d global_idx %d: got %d expected %d\n",
                        world_rank, s, global_idx, buf[i], expected);
                nerrors++;
            }
        }

        H5Sclose(memspace);
        H5Sclose(filespace);
        H5Dclose(ds);

        /* M6.5: every writer rank's private dataset must exist and be
         * correct -- the direct proof that heterogeneous per-rank creates
         * actually landed via cross-rank manifest aggregation, not just
         * the shared dataset's own M×N redistribution. Rank 0 only:
         * nothing here needs redistributing, and HDF5's independent
         * (non-collective) metadata-read default makes an unbalanced
         * per-rank open safe. */
        if (world_rank == 0) {
            int w;

            for (w = 0; w < writer_ranks; w++) {
                char  priv_name[32];
                hid_t priv_ds;
                int   priv_val = -1, expected_priv = w * 10000 + s;

                snprintf(priv_name, sizeof(priv_name), "priv%d_%d", w, s);
                if ((priv_ds = H5Dopen2(fid, priv_name, H5P_DEFAULT)) < 0) {
                    fprintf(stderr, "  FAIL  open private dataset '%s'\n", priv_name);
                    nerrors++;
                    continue;
                }
                if (H5Dread(priv_ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &priv_val) < 0) {
                    fprintf(stderr, "  FAIL  read private dataset '%s'\n", priv_name);
                    nerrors++;
                }
                else if (priv_val != expected_priv) {
                    fprintf(stderr, "  FAIL  private dataset '%s': got %d expected %d\n", priv_name,
                            priv_val, expected_priv);
                    nerrors++;
                }
                H5Dclose(priv_ds);
            }
        }
    }

    free(buf);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    MPI_Allreduce(&nerrors, &total_errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (total_errors == 0 && world_rank == 0)
        printf("  ok    read phase: %d ranks (decomposition independent of the %d writer ranks), "
               "all shared + %d private per-rank values correct\n",
               world_size, writer_ranks, writer_ranks);

    return total_errors > 0 ? 1 : 0;
}

/* CI-only diagnostic: print the PMI-related env vars this process sees
 * BEFORE calling MPI_Init(), gated behind VOL_STREAM_DEBUG_PMI_ENV so it
 * costs nothing normally. Ground truth for whether hydra_pmi_proxy set
 * them at all on a given runner, vs. guessing at mpiexec flags -- see
 * test/run_parallel_test.sh's own comment on the CI-only MPICH
 * singleton-fallback investigation. */
static void
debug_print_pmi_env(void)
{
    static const char *names[] = {"PMI_RANK",       "PMI_SIZE",   "PMI_FD",
                                   "PMI_PORT",       "PMI_KVSNAME", "PMI_ID",
                                   "PMI_DEBUG",      NULL};
    int i;

    if (!getenv("VOL_STREAM_DEBUG_PMI_ENV"))
        return;

    for (i = 0; names[i]; i++) {
        const char *v = getenv(names[i]);
        fprintf(stderr, "  pmi-env  pid=%d %s=%s\n", (int)getpid(), names[i], v ? v : "(unset)");
    }
}

int
main(int argc, char **argv)
{
    int rc = 1;

    debug_print_pmi_env();
    MPI_Init(&argc, &argv);

    if (argc < 4) {
        int world_rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
        if (world_rank == 0)
            fprintf(stderr,
                    "usage: %s write <file> <global_size>\n"
                    "       %s read  <file> <global_size> <writer_ranks>\n",
                    argv[0], argv[0]);
    }
    else if (strcmp(argv[1], "write") == 0)
        rc = do_write(argv[2], atoi(argv[3]));
    else if (strcmp(argv[1], "read") == 0 && argc >= 5)
        rc = do_read(argv[2], atoi(argv[3]), atoi(argv[4]));
    else {
        int world_rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
        if (world_rank == 0)
            fprintf(stderr, "unknown mode '%s'\n", argv[1]);
    }

    MPI_Finalize();
    return rc;
}
