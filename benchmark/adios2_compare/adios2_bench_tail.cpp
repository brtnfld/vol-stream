// The O(N) counterpart to adios2_bench.cpp: same growing array, but the
// writer Puts only the NEW tail slice each step (ADIOS2's own idiomatic
// use of SetSelection() for a streaming append, not a full rewrite), and
// the reader Gets only that same delta. Matches vol-stream's test/
// b_stream_grow_tail.c, built the same session H5VL__stream_carry_
// forward_resized() made vol-stream's own tail-only pattern give complete
// snapshots (docs/dev-plan.md) -- this is the fair, final comparison at
// the pattern both sides would actually use for a real growing stream,
// not the O(N^2) full-rewrite adios2_bench.cpp measures.
//
// See adios2_bench.cpp's own header comment for the shared design notes
// (timestamps via small files, not shared memory; ADIOS2_USE_MPI opt-in;
// SST vs BP staging) -- not repeated here.

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

    adios2::Engine writer = io.Open("adios2_bench_tail_series", adios2::Mode::Write);

    std::ofstream times("adios2_bench_tail_writer_times.txt");
    std::vector<int> tail(CHUNK);

    for (int s = 0; s < NSTEPS; s++) {
        size_t n     = (size_t)(s + 1) * CHUNK;
        size_t start = (size_t)s * CHUNK;

        for (int i = 0; i < CHUNK; i++)
            tail[i] = (int)(start + (size_t)i);

        long long t0 = now_ns();

        writer.BeginStep();
        adios2::Variable<int> var = io.InquireVariable<int>("series");
        if (!var)
            var = io.DefineVariable<int>("series", {(size_t)NSTEPS * CHUNK}, {0}, {(size_t)CHUNK});
        var.SetShape({n});
        var.SetSelection({{start}, {(size_t)CHUNK}}); /* the tail only -- the O(N) write */
        writer.Put(var, tail.data());
        writer.EndStep();

        long long t1 = now_ns();
        times << s << " " << t0 << " " << t1 << "\n";
        times.flush();
    }

    writer.Close();
    touch("adios2_bench_tail_writer_done.txt");
    return 0;
}

static int
run_reader(int rank)
{
    if (wait_for("adios2_bench_tail_series.sst", 500) < 0) {
        std::fprintf(stderr, "reader: writer's .sst rendezvous file never appeared\n");
        return 1;
    }

    adios2::ADIOS adios(MPI_COMM_WORLD);
    adios2::IO    io = adios.DeclareIO("reader");
    io.SetEngine("SST");

    adios2::Engine reader = io.Open("adios2_bench_tail_series", adios2::Mode::Read);

    std::ofstream times("adios2_bench_tail_reader_times.txt");
    int    nerrors = 0;
    size_t have = 0; /* elements read so far -- what "new tail" means to this reader */

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

        auto   shape = var.Shape();
        size_t n     = shape.empty() ? 0 : shape[0];
        size_t new_n = n - have; /* only the tail this step actually added */
        std::vector<int> buf(new_n);

        var.SetSelection({{have}, {new_n}});
        reader.Get(var, buf.data());
        reader.EndStep();

        long long t1    = now_ns();
        size_t    bytes = new_n * sizeof(int);

        times << s << " " << t1 << " " << bytes << "\n";
        times.flush();

        if (new_n != (size_t)CHUNK || (new_n > 0 && (buf[0] != (int)have || buf[new_n - 1] != (int)(n - 1)))) {
            std::fprintf(stderr, "reader: step %d wrong (new_n=%zu expected=%d)\n", s, new_n, CHUNK);
            nerrors++;
        }
        have = n;
    }

    reader.Close();
    touch("adios2_bench_tail_reader_done.txt");
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
