/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * A second real use case for vol-stream, alongside heat_diffusion: the
 * Gray-Scott reaction-diffusion system. Where heat_diffusion relaxes to a
 * static steady state and then stops being interesting, Gray-Scott's
 * "mitosis" regime keeps dividing and morphing for thousands of solver
 * iterations, which is a better showcase of a subscriber watching a
 * genuinely live, long-running simulation rather than one that visibly
 * settles within a few dozen steps.
 *
 * Two coupled fields, U and V, both streamed every step -- a real
 * multi-object step, unlike heat_diffusion's single dataset:
 *
 *   dU/dt = Du*Laplacian(U) - U*V^2 + F*(1-U)
 *   dV/dt = Dv*Laplacian(V) + U*V^2 - (F+k)*V
 *
 * Periodic (toroidal) boundary conditions, the standard choice for this
 * system so patterns can flow across an edge rather than reflecting off
 * one. See rd_common.h for the parameter preset and its citation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"
#include "rd_common.h"

static double *
rd_alloc_field(int n)
{
    return (double *)calloc((size_t)n * (size_t)n, sizeof(double));
}

/* U=1, V=0 everywhere except a small seeded square in the center, with a
 * little noise so the split isn't perfectly symmetric (a fixed seed keeps
 * the run reproducible run to run, same as heat_diffusion). */
static void
rd_init(double *u, double *v, int n)
{
    int i, j, r = n / 10 > 2 ? n / 10 : 2;
    int lo = n / 2 - r, hi = n / 2 + r;

    srand(42);
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            u[i * n + j] = 1.0;
            v[i * n + j] = 0.0;
        }
    for (i = lo; i < hi; i++)
        for (j = lo; j < hi; j++) {
            u[i * n + j] = 0.5 + 0.02 * ((double)rand() / RAND_MAX - 0.5);
            v[i * n + j] = 0.25 + 0.02 * ((double)rand() / RAND_MAX - 0.5);
        }
}

/* One Gray-Scott step, periodic boundary conditions. */
static void
rd_step(const double *u, const double *v, double *unew, double *vnew, int n)
{
    int i, j;

    for (i = 0; i < n; i++) {
        int im = (i - 1 + n) % n, ip = (i + 1) % n;

        for (j = 0; j < n; j++) {
            int    jm = (j - 1 + n) % n, jp = (j + 1) % n;
            double uc = u[i * n + j], vc = v[i * n + j];
            double lu = u[im * n + j] + u[ip * n + j] + u[i * n + jm] + u[i * n + jp] - 4.0 * uc;
            double lv = v[im * n + j] + v[ip * n + j] + v[i * n + jm] + v[i * n + jp] - 4.0 * vc;
            double uvv = uc * vc * vc;

            unew[i * n + j] = uc + (RD_DU * lu - uvv + RD_FEED * (1.0 - uc));
            vnew[i * n + j] = vc + (RD_DV * lv + uvv - (RD_FEED + RD_KILL) * vc);
        }
    }
}

static double
rd_mean(const double *f, int n)
{
    double sum = 0.0;
    int    i, total = n * n;

    for (i = 0; i < total; i++)
        sum += f[i];
    return sum / total;
}

int
main(int argc, char **argv)
{
    int n        = argc > 1 ? atoi(argv[1]) : RD_DEFAULT_N;
    int nsteps   = argc > 2 ? atoi(argv[2]) : 150;
    int substeps = argc > 3 ? atoi(argv[3]) : 25;
    int delay_ms = argc > 4 ? atoi(argv[4]) : 40;

    hid_t   vol_id, fapl, fid;
    double *u, *un, *utmp;
    double *v, *vn, *vtmp;
    hsize_t dims[2];
    int     s, k;

    if (n < 5) {
        fprintf(stderr, "writer: grid size must be >= 5\n");
        return 1;
    }

    u  = rd_alloc_field(n);
    un = rd_alloc_field(n);
    v  = rd_alloc_field(n);
    vn = rd_alloc_field(n);
    if (!u || !un || !v || !vn) {
        fprintf(stderr, "writer: FAIL allocate %dx%d fields\n", n, n);
        return 1;
    }
    rd_init(u, v, n);

    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(RD_FNAME);
    unlink(RD_FNAME ".vsgroup");
    unlink(RD_READY_SENTINEL);

    if ((vol_id = H5VL_stream_register()) < 0) {
        fprintf(stderr, "writer: FAIL register vol-stream\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        fprintf(stderr, "writer: FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(RD_FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        fprintf(stderr, "writer: FAIL create %s (transport up? VOL_STREAM_NA set?)\n", RD_FNAME);
        return 1;
    }

    printf("writer: %dx%d grid, %d step(s) x %d solver iteration(s), F=%.4f k=%.4f -- %s\n", n, n, nsteps,
           substeps, RD_FEED, RD_KILL, RD_FNAME);

    printf("writer: waiting up to 8s for a subscriber...\n");
    if (rd_wait_for_file(RD_READY_SENTINEL, 8000) == 0)
        printf("writer: subscriber attached, starting\n");
    else
        printf("writer: no subscriber attached in time, proceeding solo\n");

    dims[0] = (hsize_t)n;
    dims[1] = (hsize_t)n;

    for (s = 0; s < nsteps; s++) {
        hid_t space, ds_u, ds_v;

        for (k = 0; k < substeps; k++) {
            rd_step(u, v, un, vn, n);
            utmp = u;
            u    = un;
            un   = utmp;
            vtmp = v;
            v    = vn;
            vn   = vtmp;
        }

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            fprintf(stderr, "writer: FAIL begin_step %d\n", s);
            return 1;
        }
        if ((space = H5Screate_simple(2, dims, NULL)) < 0 ||
            (ds_u = H5Dcreate2(fid, RD_DATASET_U, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT)) < 0 ||
            H5Dwrite(ds_u, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, u) < 0) {
            fprintf(stderr, "writer: FAIL write U, step %d\n", s);
            return 1;
        }
        H5Dclose(ds_u);
        if ((ds_v = H5Dcreate2(fid, RD_DATASET_V, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT)) < 0 ||
            H5Dwrite(ds_v, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, v) < 0) {
            fprintf(stderr, "writer: FAIL write V, step %d\n", s);
            return 1;
        }
        H5Dclose(ds_v);
        H5Sclose(space);
        if (H5Fend_step(fid) < 0) {
            fprintf(stderr, "writer: FAIL end_step %d\n", s);
            return 1;
        }

        printf("writer: step %4d/%-4d  mean(U)=%.4f  mean(V)=%.4f\n", s + 1, nsteps, rd_mean(u, n),
               rd_mean(v, n));
        fflush(stdout);

        if (delay_ms > 0)
            usleep(delay_ms * 1000);
    }

    printf("writer: done stepping, settling before close...\n");
    fflush(stdout);
    usleep(1500000);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    free(u);
    free(un);
    free(v);
    free(vn);

    printf("writer: done, %d step(s) written to %s\n", nsteps, RD_FNAME);
    return 0;
}
