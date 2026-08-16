// Matched ADIOS2 benchmark for vol-stream's test/b_stream_grow.c.
//
// Same workload: a variable growing by CHUNK elements per step across
// NSTEPS steps, streamed for real between a separate writer and reader
// process, on ADIOS2's SST engine (its streaming engine, the fair
// counterpart to vol-stream's Mercury transport -- not BP file staging).
//
// ADIOS2's own idiomatic mechanism for a shape that changes across steps
// is used, rather than vol-stream's forced pattern (recreate-and-rewrite,
// the only thing available once resizing a live cross-step handle is
// refused): Variable::SetShape()/SetSelection() update a variable's global
// shape and this step's selection before each Put, no redefinition needed.
// The workload is still identical -- same element counts, same growth,
// same full-cumulative-array-every-step content -- so the wire bytes and
// timings are directly comparable even though the API path differs.
//
// Timestamps go to small per-step files rather than a shared mmap: writer
// and reader are separate mpirun-launched processes (SST's normal usage),
// not fork() children of a common parent, but CLOCK_MONOTONIC is a single
// machine-wide clock, so timestamps written by either process to disk and
// read back afterward are still directly comparable.

#define ADIOS2_USE_MPI 1
#include <adios2.h>
#include <mpi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

static const int NSTEPS = 50;
static const int CHUNK  = 8192;

static long long
now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static void
touch(const char *path)
{
    std::ofstream f(path);
}

static int
wait_for(const char *path, int max_polls)
{
    for (int i = 0; i < max_polls; i++) {
        std::ifstream f(path);
        if (f.good())
            return 0;
        usleep(20000);
    }
    return -1;
}

static int
run_writer(int rank)
{
    adios2::ADIOS adios(MPI_COMM_WORLD);
    adios2::IO    io = adios.DeclareIO("writer");
    io.SetEngine("SST");

    adios2::Engine writer = io.Open("adios2_bench_series", adios2::Mode::Write);

    std::ofstream times("adios2_bench_writer_times.txt");
    std::vector<int> vals((size_t)NSTEPS * CHUNK);

    for (int s = 0; s < NSTEPS; s++) {
        size_t n = (size_t)(s + 1) * CHUNK;
        for (size_t i = 0; i < n; i++)
            vals[i] = (int)i;

        long long t0 = now_ns();

        writer.BeginStep();
        adios2::Variable<int> var = io.InquireVariable<int>("series");
        if (!var) {
            /* First step: define once with a generous max global shape --
             * SetShape() below still changes what THIS step reports. */
            var = io.DefineVariable<int>("series", {(size_t)NSTEPS * CHUNK}, {0}, {n});
        }
        var.SetShape({n});
        var.SetSelection({{0}, {n}});
        writer.Put(var, vals.data());
        writer.EndStep();

        long long t1 = now_ns();
        times << s << " " << t0 << " " << t1 << "\n";
        times.flush();
    }

    writer.Close();
    touch("adios2_bench_writer_done.txt");
    return 0;
}

static int
run_reader(int rank)
{
    if (wait_for("adios2_bench_series.sst", 500) < 0) {
        std::fprintf(stderr, "reader: writer's .sst rendezvous file never appeared\n");
        return 1;
    }

    adios2::ADIOS adios(MPI_COMM_WORLD);
    adios2::IO    io = adios.DeclareIO("reader");
    io.SetEngine("SST");

    adios2::Engine reader = io.Open("adios2_bench_series", adios2::Mode::Read);

    std::ofstream times("adios2_bench_reader_times.txt");
    int nerrors = 0;

    for (int s = 0; s < NSTEPS; s++) {
        adios2::StepStatus status = reader.BeginStep();
        if (status != adios2::StepStatus::OK) {
            std::fprintf(stderr, "reader: BeginStep failed at logical step %d (status=%d)\n", s,
                         (int)status);
            nerrors++;
            break;
        }

        adios2::Variable<int> var = io.InquireVariable<int>("series");
        if (!var) {
            std::fprintf(stderr, "reader: variable 'series' not found at step %d\n", s);
            nerrors++;
            reader.EndStep();
            continue;
        }

        auto shape = var.Shape();
        size_t n = shape.empty() ? 0 : shape[0];
        std::vector<int> buf(n);
        var.SetSelection({{0}, {n}});
        reader.Get(var, buf.data());
        reader.EndStep(); /* SST: Get() is deferred, EndStep() is when the data actually arrives */

        long long t1 = now_ns();
        size_t bytes = n * sizeof(int);

        times << s << " " << t1 << " " << bytes << "\n";
        times.flush();

        size_t expect_n = (size_t)(s + 1) * CHUNK;
        if (n != expect_n || (n > 0 && (buf[0] != 0 || buf[n - 1] != (int)(n - 1)))) {
            std::fprintf(stderr, "reader: step %d wrong (n=%zu expected=%zu)\n", s, n, expect_n);
            nerrors++;
        }
    }

    reader.Close();
    touch("adios2_bench_reader_done.txt");
    return nerrors ? 1 : 0;
}

int
main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s writer|reader\n", argv[0]);
        MPI_Finalize();
        return 2;
    }

    int rc;
    if (std::strcmp(argv[1], "writer") == 0)
        rc = run_writer(rank);
    else if (std::strcmp(argv[1], "reader") == 0)
        rc = run_reader(rank);
    else {
        std::fprintf(stderr, "unknown role '%s'\n", argv[1]);
        rc = 2;
    }

    MPI_Finalize();
    return rc;
}
