/* Standalone exercise of the Diaspora transport backend (src/tr_diaspora.cpp)
 * through the exact vs_tr_*() surface tr_mercury.h declares -- deliberately
 * written in C, so it proves a C caller (H5VLstream.c) can drive the C++
 * backend unchanged.
 *
 * Not wired into CMakeLists.txt yet: that needs the VOL_STREAM_TRANSPORT
 * option that picks exactly one backend, which is the next step rather than
 * this one. Build and run standalone:
 *
 *   SPACK=build/spack-env/.spack-env/view
 *   export PKG_CONFIG_PATH=/tmp/margo0242/lib64/pkgconfig:$SPACK/lib/pkgconfig:$SPACK/lib64/pkgconfig
 *   g++ -std=c++17 -c -o /tmp/tr_diaspora.o src/tr_diaspora.cpp \
 *       -I/tmp/diaspora-install/include -Isrc $(pkg-config --cflags margo)
 *   gcc -std=c99 -c -o /tmp/t_diaspora.o test/t_diaspora.c -Isrc $(pkg-config --cflags margo)
 *   g++ -o /tmp/t_diaspora /tmp/t_diaspora.o /tmp/tr_diaspora.o \
 *       -L/tmp/diaspora-install/lib -ldiaspora-stream-api \
 *       -Wl,-rpath,/tmp/diaspora-install/lib
 *   mkdir -p /tmp/vs-diaspora-test && /tmp/t_diaspora /tmp/vs-diaspora-test
 *
 * /tmp/diaspora-install is the Diaspora Stream API plus its five C++ deps
 * (nlohmann_json, json-schema-validator, TCLAP, fmt+spdlog, yaml-cpp), none
 * of which this machine had. No Mercury, margo or thallium is needed for the
 * built-in "files" driver -- only for the Mofka driver.
 *
 * Asserts on count and size as well as values: an earlier all-1-D suite in
 * this project hid a real N-D data-loss bug by only ever checking values. */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "tr_mercury.h"

extern unsigned vs_tr_diaspora_unsupported(vs_tr_t *tr);

static int failures = 0;

#define CHECK(cond, fmt, ...)                                                                      \
    do {                                                                                           \
        if (cond) {                                                                                \
            printf("  ok   " fmt "\n", ##__VA_ARGS__);                                             \
        }                                                                                          \
        else {                                                                                     \
            printf("  FAIL " fmt "\n", ##__VA_ARGS__);                                             \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* Stubs for the five writer-side callbacks. Never invoked on this backend --
 * registering them is how we check the backend reports that honestly. The
 * signatures must match tr_mercury.h exactly, which is itself part of what
 * this test checks. */
static int
stub_refilter(const void *raw_buf, uint64_t elem_size, uint64_t count, const uint8_t *dcpl_enc,
              uint64_t dcpl_enc_len, const uint8_t *type_enc, uint64_t type_enc_len,
              const uint8_t *native_dcpl_enc, uint64_t native_dcpl_enc_len, void *native_ctx,
              void **out_buf, uint64_t *out_len, uint32_t *out_filter_mask)
{
    (void)raw_buf; (void)elem_size; (void)count; (void)dcpl_enc; (void)dcpl_enc_len;
    (void)type_enc; (void)type_enc_len; (void)native_dcpl_enc; (void)native_dcpl_enc_len;
    (void)native_ctx; (void)out_buf; (void)out_len; (void)out_filter_mask;
    return -1;
}

static int
stub_predicate(const void *raw_buf, uint64_t elem_size, uint64_t count, const uint8_t *pred_enc,
               uint64_t pred_enc_len, const uint8_t *type_enc, uint64_t type_enc_len,
               vs_tr_run_t *runs, int max_runs)
{
    (void)raw_buf; (void)elem_size; (void)count; (void)pred_enc; (void)pred_enc_len;
    (void)type_enc; (void)type_enc_len; (void)runs; (void)max_runs;
    return -1;
}

int
main(int argc, char **argv)
{
    const char *root  = (argc > 1) ? argv[1] : "/tmp/vs-diaspora-test";
    char        spec[512];
    char        group[512];
    int         rc;

    snprintf(spec, sizeof(spec), "files:%s", root);
    snprintf(group, sizeof(group), "%s/series.h5.vsgroup", root);

    printf("== transport start ==\n");
    vs_tr_t *w = vs_tr_start(spec);
    CHECK(w != NULL, "writer vs_tr_start(\"%s\")", spec);
    if (!w)
        return 1;
    vs_tr_t *r = vs_tr_start(spec);
    CHECK(r != NULL, "reader vs_tr_start()");
    if (!r)
        return 1;

    /* The seam's five writer-side hooks: registration must be accepted (so
     * H5VLstream.c needs no #ifdef) and then reported as unsupported. */
    vs_tr_set_refilter_cb(w, stub_refilter);
    vs_tr_set_predicate_cb(w, stub_predicate);

    printf("== group -> topic ==\n");
    rc = vs_tr_writer_start_group(w, group);
    CHECK(rc == 0, "vs_tr_writer_start_group() = %d", rc);
    rc = vs_tr_reader_join_group(r, group);
    CHECK(rc == 0, "vs_tr_reader_join_group() = %d", rc);

    printf("== bounded subscription ==\n");
    /* Ask for elements [4,12) of a 16-element write: the narrowing path. */
    rc = vs_tr_reader_subscribe(r, "/data", 4, 8, NULL, 0, NULL, 0);
    CHECK(rc == 0, "vs_tr_reader_subscribe(\"/data\", start=4, count=8) = %d", rc);

    printf("== push 16 int32 elements at offset 0 ==\n");
    int32_t buf[16];
    for (int i = 0; i < 16; i++)
        buf[i] = 1000 + i;
    const uint8_t fake_type[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
    rc = vs_tr_writer_push_data(w, 7, "/data", buf, sizeof(int32_t), 0, 16, fake_type,
                                sizeof(fake_type), NULL, 0, NULL, NULL, 0);
    CHECK(rc == 0, "vs_tr_writer_push_data() = %d", rc);
    rc = vs_tr_writer_broadcast_step_ready(w, 7, 123456789ULL);
    CHECK(rc == 0, "vs_tr_writer_broadcast_step_ready(step=7) = %d", rc);

    printf("== reader receives only its overlap ==\n");
    uint64_t step = 0, elem_start = 0, elem_count = 0, size = 0, tlen = 0, dlen = 0;
    char    *path = NULL;
    void    *out  = NULL;
    uint8_t *tenc = NULL, *denc = NULL;
    uint32_t fmask = 0xFFFFFFFFu;
    rc = vs_tr_reader_wait_data(r, 5000, &step, &path, &out, &size, &elem_start, &elem_count, &denc,
                                &dlen, &tenc, &tlen, &fmask);
    CHECK(rc == 0, "vs_tr_reader_wait_data() = %d", rc);
    if (rc == 0) {
        CHECK(step == 7, "step = %" PRIu64 " (want 7)", step);
        CHECK(path && strcmp(path, "/data") == 0, "path = \"%s\" (want \"/data\")",
              path ? path : "(null)");
        CHECK(elem_start == 4, "elem_start = %" PRIu64 " (want 4)", elem_start);
        CHECK(elem_count == 8, "elem_count = %" PRIu64 " (want 8)", elem_count);
        CHECK(size == 8 * sizeof(int32_t), "size = %" PRIu64 " bytes (want 32)", size);

        int values_ok = (out != NULL);
        if (out) {
            const int32_t *got = (const int32_t *)out;
            for (uint64_t i = 0; i < elem_count && i < 8; i++)
                if (got[i] != 1000 + 4 + (int32_t)i) {
                    printf("       element %" PRIu64 ": got %d want %d\n", i, got[i],
                           1000 + 4 + (int32_t)i);
                    values_ok = 0;
                }
        }
        CHECK(values_ok, "payload values are elements 4..11 (1004..1011)");

        CHECK(tlen == sizeof(fake_type) && tenc && memcmp(tenc, fake_type, tlen) == 0,
              "type_enc survived the base64/JSON round trip (%" PRIu64 " bytes)", tlen);
        CHECK(dlen == 0 && denc == NULL, "dcpl_enc reported as not-refiltered");
        CHECK(fmask == 0, "filter_mask reported as 0");

        free(path);
        free(out);
        free(tenc);
        free(denc);
    }

    printf("== step marker ==\n");
    uint64_t s2 = 0, wt = 0;
    rc = vs_tr_reader_wait_step_ready(r, 5000, &s2, &wt);
    CHECK(rc == 0, "vs_tr_reader_wait_step_ready() = %d", rc);
    CHECK(s2 == 7, "step = %" PRIu64 " (want 7)", s2);
    CHECK(wt == 123456789ULL, "wall_time_ns = %" PRIu64 " (want 123456789)", wt);

    printf("== schema publish / get ==\n");
    const uint8_t schema[] = {'S', 'C', 'H', 'E', 'M', 'A', 0x00, 0x7F};
    rc = vs_tr_writer_publish_schema(w, 7, schema, sizeof(schema));
    CHECK(rc == 0, "vs_tr_writer_publish_schema() = %d", rc);
    uint64_t sstep = 0, slen = 0;
    uint8_t *sblob = NULL;
    rc = vs_tr_reader_get_schema(r, 2000, &sstep, &sblob, &slen);
    CHECK(rc == 0, "vs_tr_reader_get_schema() = %d", rc);
    if (rc == 0) {
        CHECK(sstep == 7, "schema step = %" PRIu64 " (want 7)", sstep);
        CHECK(slen == sizeof(schema) && sblob && memcmp(sblob, schema, slen) == 0,
              "schema blob round-tripped (%" PRIu64 " bytes)", slen);
        free(sblob);
    }

    /* The general case. A single write starting at element 0 exercises only
     * the easy path; the bugs live in partial and empty overlap. Subscription
     * is still [4,12). */
    printf("== partial overlap: write [10,16) vs subscription [4,12) ==\n");
    int32_t tail[6];
    for (int i = 0; i < 6; i++)
        tail[i] = 2000 + i; /* element 10+i */
    rc = vs_tr_writer_push_data(w, 8, "/data", tail, sizeof(int32_t), 10, 6, NULL, 0, NULL, 0, NULL,
                                NULL, 0);
    CHECK(rc == 0, "vs_tr_writer_push_data(start=10, count=6) = %d", rc);

    step = elem_start = elem_count = size = 0;
    path = NULL; out = NULL; tenc = NULL; denc = NULL;
    rc = vs_tr_reader_wait_data(r, 5000, &step, &path, &out, &size, &elem_start, &elem_count, &denc,
                                &dlen, &tenc, &tlen, &fmask);
    CHECK(rc == 0, "vs_tr_reader_wait_data() = %d", rc);
    if (rc == 0) {
        CHECK(step == 8, "step = %" PRIu64 " (want 8)", step);
        CHECK(elem_start == 10, "elem_start = %" PRIu64 " (want 10)", elem_start);
        CHECK(elem_count == 2, "elem_count = %" PRIu64 " (want 2, the [10,12) overlap)", elem_count);
        CHECK(size == 2 * sizeof(int32_t), "size = %" PRIu64 " bytes (want 8)", size);
        int ok2 = out != NULL;
        if (out) {
            const int32_t *g = (const int32_t *)out;
            /* Elements 10 and 11 are the first two of the write. */
            if (g[0] != 2000 || g[1] != 2001) {
                printf("       got {%d,%d} want {2000,2001}\n", g[0], g[1]);
                ok2 = 0;
            }
        }
        CHECK(ok2, "payload is elements 10..11 (2000..2001), not the whole write");
        free(path); free(out); free(tenc); free(denc);
    }

    printf("== no overlap: write [12,16) vs subscription [4,12) ==\n");
    int32_t beyond[4] = {3000, 3001, 3002, 3003};
    rc = vs_tr_writer_push_data(w, 9, "/data", beyond, sizeof(int32_t), 12, 4, NULL, 0, NULL, 0,
                                NULL, NULL, 0);
    CHECK(rc == 0, "vs_tr_writer_push_data(start=12, count=4) = %d", rc);
    path = NULL; out = NULL; tenc = NULL; denc = NULL;
    rc = vs_tr_reader_wait_data(r, 400, &step, &path, &out, &size, &elem_start, &elem_count, &denc,
                                &dlen, &tenc, &tlen, &fmask);
    CHECK(rc == -1, "vs_tr_reader_wait_data() = %d (want -1: nothing delivered, not an empty push)",
          rc);
    free(path); free(out); free(tenc); free(denc);

    printf("== unsubscribed path is declined ==\n");
    int32_t other[4] = {1, 2, 3, 4};
    rc = vs_tr_writer_push_data(w, 10, "/not-subscribed", other, sizeof(int32_t), 0, 4, NULL, 0,
                                NULL, 0, NULL, NULL, 0);
    CHECK(rc == 0, "vs_tr_writer_push_data(\"/not-subscribed\") = %d", rc);
    path = NULL; out = NULL;
    rc = vs_tr_reader_wait_data(r, 400, &step, &path, &out, &size, &elem_start, &elem_count, &denc,
                                &dlen, &tenc, &tlen, &fmask);
    CHECK(rc == -1, "vs_tr_reader_wait_data() = %d (want -1: path not subscribed)", rc);
    free(path); free(out);

    printf("== rendezvous and backpressure, via the control topic ==\n");
    /* The reader subscribed to "/data" above, which announces on the control
     * topic. The writer must be able to see that without any help from
     * Diaspora, which cannot tell a producer who its consumers are. */
    rc = vs_tr_writer_wait_subscribers(w, 1, 3000);
    CHECK(rc == 0, "vs_tr_writer_wait_subscribers(1) = %d (want 0: the reader was seen)", rc);

    /* Counts subscribers, not subscriptions: a second path from the same
     * reader must not make it two. */
    rc = vs_tr_reader_subscribe(r, "/data2", 0, UINT64_MAX, NULL, 0, NULL, 0);
    CHECK(rc == 0, "vs_tr_reader_subscribe(\"/data2\") = %d", rc);
    rc = vs_tr_writer_wait_subscribers(w, 2, 500);
    CHECK(rc == -1, "vs_tr_writer_wait_subscribers(2) = %d (want -1: still one reader)", rc);

    uint64_t min_acked = 0;
    rc = vs_tr_writer_min_acked_step(w, &min_acked);
    CHECK(rc == 0, "vs_tr_writer_min_acked_step() = %d (want 0: nobody has acked yet)", rc);

    rc = vs_tr_reader_ack_step(r, 7);
    CHECK(rc == 0, "vs_tr_reader_ack_step(7) = %d", rc);
    rc = vs_tr_writer_min_acked_step(w, &min_acked);
    CHECK(rc == 1, "vs_tr_writer_min_acked_step() = %d (want 1: one acking reader)", rc);
    CHECK(min_acked == 7, "min_acked_step = %" PRIu64 " (want 7)", min_acked);

    /* A later ack advances it; an older one must not move it backwards. */
    rc = vs_tr_reader_ack_step(r, 9);
    CHECK(rc == 0, "vs_tr_reader_ack_step(9) = %d", rc);
    vs_tr_writer_min_acked_step(w, &min_acked);
    CHECK(min_acked == 9, "min_acked_step advanced to %" PRIu64 " (want 9)", min_acked);
    vs_tr_reader_ack_step(r, 4);
    vs_tr_writer_min_acked_step(w, &min_acked);
    CHECK(min_acked == 9, "a stale ack left it at %" PRIu64 " (want 9, not 4)", min_acked);

    printf("== features still with no Diaspora counterpart ==\n");
    rc = vs_tr_reader_subscribe_predicate(r, "/data", (const uint8_t *)"pred", 4);
    CHECK(rc == -1, "vs_tr_reader_subscribe_predicate() = %d (want -1, unsupported)", rc);

    unsigned unsup_w = vs_tr_diaspora_unsupported(w);
    unsigned unsup_r = vs_tr_diaspora_unsupported(r);
    printf("  writer unsupported bitmask = 0x%02x, reader = 0x%02x\n", unsup_w, unsup_r);
    CHECK((unsup_w & 0x01) != 0, "refilter callback reported unsupported");
    CHECK((unsup_w & 0x08) != 0, "predicate callback reported unsupported");
    CHECK((unsup_w & 0x20) == 0, "wait_subscribers NO LONGER reported unsupported");
    CHECK((unsup_w & 0x40) == 0, "min_acked_step NO LONGER reported unsupported");
    CHECK((unsup_r & 0x08) != 0, "reader-side predicate reported unsupported");

    printf("== the seam's one leak ==\n");
    CHECK(vs_tr_get_mid(w) == MARGO_INSTANCE_NULL,
          "vs_tr_get_mid() = NULL (no margo instance; BAKE spill disabled)");

    printf("== teardown ==\n");
    rc = vs_tr_reader_leave_group(r);
    CHECK(rc == 0, "vs_tr_reader_leave_group() = %d", rc);
    vs_tr_stop(r);
    vs_tr_stop(w);
    printf("  ok   vs_tr_stop() both sides\n");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
