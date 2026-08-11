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
 * Heterogeneous per-rank object sets (rank 0 creating a dataset rank 1
 * never touches) and the Subfiling-style I/O-concentrator aggregation
 * topology are out of scope for this increment -- see
 * H5VL__stream_replay_step()'s comment for why this simpler case does not
 * need cross-rank manifest aggregation at all.
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NSTEPS 2

/* Deterministic per-(step, writer-rank, local-index) value -- both write
 * and read sides compute this the same way, so a reader with an unrelated
 * decomposition can still check every element exactly. */
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

    if (global_size % world_size != 0) {
        if (world_rank == 0)
            fprintf(stderr, "write: global_size %d not divisible by %d ranks\n", global_size, world_size);
        return 1;
    }
    per_rank = global_size / world_size;
    dims[0]  = (hsize_t)global_size;
    start[0] = (hsize_t)(world_rank * per_rank);
    count[0] = (hsize_t)per_rank;

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
        char           name[32];
        const uint64_t logical = (uint64_t)(100 + s);
        hid_t          ds, fspace_sel;

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

    if (global_size % world_size != 0) {
        if (world_rank == 0)
            fprintf(stderr, "read: global_size %d not divisible by %d ranks\n", global_size, world_size);
        return 1;
    }
    per_rank        = global_size / world_size;
    writer_per_rank = global_size / writer_ranks;
    start[0]        = (hsize_t)(world_rank * per_rank);
    count[0]        = (hsize_t)per_rank;

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
            int global_idx        = world_rank * per_rank + i;
            int origin_writer_rank = global_idx / writer_per_rank;
            int local_i             = global_idx % writer_per_rank;
            int expected            = expected_value(s, origin_writer_rank, local_i);

            if (buf[i] != expected) {
                fprintf(stderr, "  FAIL  reader rank %d step %d global_idx %d: got %d expected %d\n",
                        world_rank, s, global_idx, buf[i], expected);
                nerrors++;
            }
        }

        H5Sclose(memspace);
        H5Sclose(filespace);
        H5Dclose(ds);
    }

    free(buf);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    MPI_Allreduce(&nerrors, &total_errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (total_errors == 0 && world_rank == 0)
        printf("  ok    read phase: %d ranks (decomposition independent of the %d writer ranks), "
               "all values correct\n",
               world_size, writer_ranks);

    return total_errors > 0 ? 1 : 0;
}

int
main(int argc, char **argv)
{
    int rc = 1;

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
