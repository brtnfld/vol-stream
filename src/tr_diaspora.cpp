/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Purpose: PROTOTYPE second transport backend, implementing the same
 *          vs_tr_*() surface as tr_mercury.c on top of the Diaspora Stream
 *          API (https://github.com/diaspora-project/diaspora-stream-api)
 *          rather than on Mercury/Margo/Flock directly.
 *
 *          The point is to reach Mofka -- and, through the same API and at
 *          no extra cost, Kafka/Redpanda/MSK (the "octopus" driver) and the
 *          built-in "files" driver -- without H5VLstream.c changing at all.
 *          This file therefore #includes tr_mercury.h rather than declaring
 *          its own prototypes: the compiler then enforces that the seam is
 *          honoured exactly, and any drift in the interface is a build
 *          error here rather than a surprise at run time.
 *
 *          Selected at configure time (VOL_STREAM_TRANSPORT=diaspora), not
 *          at run time: both backends define the same symbols, so exactly
 *          one is linked. A runtime-selectable build would need the vs_tr_*
 *          surface behind a vtable, which is a refactor worth doing only
 *          once this backend has earned its place.
 *
 *          WHAT MAPS, AND WHAT DOES NOT. The Diaspora model is Kafka's: a
 *          producer appends events to a topic's partitions and is oblivious
 *          to who consumes them. vol-stream's model is the opposite bet --
 *          consumers declare what they want and the *writer* marshals only
 *          that (see README.md's "Design in one paragraph"). So:
 *
 *            - Lifecycle, step markers, the data path, per-consumer
 *              acknowledgement and schema publication all map cleanly, and
 *              acknowledgement maps to something strictly better than ours
 *              (Diaspora persists a per-consumer-name offset, so a restarted
 *              reader resumes where it left off).
 *
 *            - Subscription narrowing maps, but *relocates* from the writer
 *              to the reader: a Diaspora DataSelector runs on the consumer,
 *              deciding which bytes to pull from the server, and the server
 *              honours it (DataDescriptor::makeSubView/makeStridedView).
 *              That saves the server->consumer leg only; the producer has
 *              already shipped the whole write. Everything vol-stream does
 *              in the writer to avoid putting bytes on the wire at all is
 *              therefore weaker here, not absent.
 *
 *            - The five writer-side callbacks (refilter, refilter_shape,
 *              convert, predicate, selection) have no counterpart, because
 *              there is no channel by which a Diaspora producer could learn
 *              what any consumer wants. They are accepted and recorded so
 *              that vs_tr_diaspora_unsupported() can report honestly at run
 *              time, and deliberately not silently ignored.
 *
 *            - vs_tr_writer_wait_subscribers(), vs_tr_writer_min_acked_step()
 *              and vs_tr_reader_get_current_step() likewise have no
 *              counterpart: the first two need producer-visible consumer
 *              state, and the third needs a "where is the writer now" query
 *              that an append-only log does not offer. Each returns -1 with
 *              a recorded reason rather than a plausible-looking wrong
 *              answer -- H5VLstream.c already treats all three as
 *              best-effort, so a caller that ignores the return value
 *              degrades rather than breaks.
 *
 *          WIRE FORMAT. A Diaspora event is (JSON metadata, opaque data).
 *          One vs_tr_writer_push_data() call becomes one event whose data
 *          part is the payload bytes and whose metadata carries what
 *          tr_mercury.c would have put in flatbuffer fields:
 *
 *            {"k":"data", "step":N, "path":"/g/d", "es":elem_size,
 *             "s":elem_start, "c":elem_count,
 *             "t":"<base64 H5Tencode>", "sp":"<base64 H5Sencode2>"}
 *
 *          The opaque HDF5 blobs have to be base64'd because Diaspora
 *          metadata is JSON, which costs 33% on those fields -- acceptable
 *          because they are small and per-write, but a real cost our own
 *          binary format does not pay. Keeping them out of the data part is
 *          deliberate: a DataSelector only gets to see metadata before
 *          deciding what to pull, so anything the selector must reason about
 *          has to live there.
 */

#include <diaspora/Driver.hpp>
#include <diaspora/TopicHandle.hpp>
#include <diaspora/Producer.hpp>
#include <diaspora/Consumer.hpp>

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unistd.h>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "tr_mercury.h"
}

namespace {

/* ------------------------------------------------------------------ */
/* base64, for the opaque H5Tencode/H5Sencode2/H5Pencode2 blobs that   */
/* have to ride inside JSON metadata (see the file comment).           */
/* ------------------------------------------------------------------ */

const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string
b64_encode(const uint8_t *p, uint64_t n)
{
    std::string out;
    if (!p || !n)
        return out;
    out.reserve(((n + 2) / 3) * 4);
    for (uint64_t i = 0; i < n; i += 3) {
        uint32_t v    = static_cast<uint32_t>(p[i]) << 16;
        int      have = 1;
        if (i + 1 < n) { v |= static_cast<uint32_t>(p[i + 1]) << 8; have = 2; }
        if (i + 2 < n) { v |= static_cast<uint32_t>(p[i + 2]);      have = 3; }
        out += B64[(v >> 18) & 0x3f];
        out += B64[(v >> 12) & 0x3f];
        out += (have > 1) ? B64[(v >> 6) & 0x3f] : '=';
        out += (have > 2) ? B64[v & 0x3f]        : '=';
    }
    return out;
}

std::vector<uint8_t>
b64_decode(const std::string &s)
{
    int8_t rev[256];
    std::memset(rev, -1, sizeof(rev));
    for (int i = 0; i < 64; i++)
        rev[static_cast<unsigned char>(B64[i])] = static_cast<int8_t>(i);

    std::vector<uint8_t> out;
    uint32_t             acc = 0;
    int                  bits = 0;
    for (char c : s) {
        if (c == '=')
            break;
        int8_t d = rev[static_cast<unsigned char>(c)];
        if (d < 0)
            continue;
        acc = (acc << 6) | static_cast<uint32_t>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
        }
    }
    return out;
}

/* malloc()'d copy, because every out-parameter in tr_mercury.h is
 * documented as caller-frees with free(). */
void *
dup_malloc(const void *src, uint64_t n)
{
    if (!n)
        return nullptr;
    void *p = std::malloc(n);
    if (p && src)
        std::memcpy(p, src, n);
    return p;
}

char *
dup_str(const std::string &s)
{
    char *p = static_cast<char *>(std::malloc(s.size() + 1));
    if (p)
        std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

/* One reader-side subscription: the 1-D element range this process asked
 * for, exactly as vs_tr_reader_subscribe() records it in tr_mercury.c. */
struct sub_t {
    uint64_t             sel_start = 0;
    uint64_t             sel_count = UINT64_MAX;
    std::vector<uint8_t> dcpl_enc; /* recorded; unusable, see file comment */
    std::vector<uint8_t> pred_enc; /* recorded; unusable, see file comment */
    bool                 has_type = false;
    std::vector<uint8_t> type_enc;
    std::vector<uint8_t> space_enc; /* the subscriber's own selection */
};

/* A pulled event, already narrowed, waiting for vs_tr_reader_wait_data(). */
struct item_t {
    uint64_t             step = 0;
    std::string          path;
    std::vector<uint8_t> buf;
    uint64_t             elem_start = 0;
    uint64_t             elem_count = 0;
    std::vector<uint8_t> type_enc;
};

/* Overlap of a write's [w_start, w_start+w_count) with a subscription's
 * [s_start, s_start+s_count), in elements. Returns false for no overlap.
 * Deliberately the same arithmetic the DataSelector below performs, so the
 * bytes pulled and the range reported to the caller cannot disagree. */
bool
overlap(uint64_t w_start, uint64_t w_count, uint64_t s_start, uint64_t s_count, uint64_t *o_start,
        uint64_t *o_count)
{
    uint64_t w_end = (w_count > UINT64_MAX - w_start) ? UINT64_MAX : w_start + w_count;
    uint64_t s_end = (s_count > UINT64_MAX - s_start) ? UINT64_MAX : s_start + s_count;
    uint64_t lo    = w_start > s_start ? w_start : s_start;
    uint64_t hi    = w_end < s_end ? w_end : s_end;
    if (lo >= hi)
        return false;
    *o_start = lo;
    *o_count = hi - lo;
    return true;
}


/* Timeout conventions do not match, and pass-through is wrong.
 * tr_mercury.h documents "timeout_ms == 0 polls without blocking" for
 * vs_tr_reader_wait_data(). Diaspora's Future::wait(int) treats any
 * timeout <= 0 as "block until set" (files-driver FutureState::wait does
 * `while(!is_set) cv.wait(lock)` for that case), so handing 0 straight
 * through hangs forever -- which is exactly what it did the first time this
 * was run. Translate: our 0 becomes the shortest real poll, and anything
 * larger is clamped into int range. */
int
dsa_timeout(uint64_t timeout_ms)
{
    if (timeout_ms == 0)
        return 5; /* shortest useful poll; the files driver's own poll loop
                   * ticks at 1ms, so anything less just spins */
    if (timeout_ms > (uint64_t)INT_MAX)
        return INT_MAX;
    return (int)timeout_ms;
}

} /* anonymous namespace */

/* The opaque handle tr_mercury.h promises. Same name, same opacity; the
 * contents are this backend's business only. */
struct vs_tr_t {
    diaspora::Driver                    driver;
    std::optional<diaspora::TopicHandle> topic;
    std::optional<diaspora::TopicHandle> schema_topic;
    std::optional<diaspora::Producer>   producer;
    std::optional<diaspora::Producer>   schema_producer;
    std::optional<diaspora::Consumer>   consumer;
    diaspora::ThreadPool                pool;

    std::string root;       /* driver root/bootstrap string */
    std::string group_file; /* sidecar path, written by the writer */
    std::string topic_name;
    bool        is_writer = false;

    uint64_t cur_step      = 0;
    uint64_t cur_wall_ns   = 0;
    bool     have_step     = false;

    std::mutex                   lock;
    std::map<std::string, sub_t> subs;
    std::vector<item_t>          ready;   /* narrowed, awaiting wait_data() */

    /* The control topic. Diaspora gives a producer no way to see consumers:
     * there is no "list my consumers" call, and event.acknowledge() updates a
     * server-side per-consumer offset that the producer cannot read back. So
     * the two things the writer needs to know about its readers -- how many
     * have subscribed, and how far each has consumed -- are carried as
     * ordinary events on a side topic that readers produce to and the writer
     * consumes. This is vol-stream's subscription protocol rebuilt on top of
     * Diaspora's, which is precisely the cost of a consumer-oblivious model:
     * the mechanism does not come for free, it comes back as application
     * code. */
    std::string                       rid;  /* this process's identity on the control topic */
    std::optional<diaspora::TopicHandle> control_topic;
    std::optional<diaspora::Producer>    control_producer; /* reader side */
    std::optional<diaspora::Consumer>    control_consumer; /* writer side */
    std::set<std::string>                seen_subs;        /* distinct rids that subscribed */
    std::map<std::string, uint64_t>      acked;            /* rid -> highest step acked */
    /* Which paths anyone has subscribed to, learned from the control topic.
     * This is the writer-side subscription knowledge that tr_mercury.c gets
     * from a subscribe RPC, arriving here over a side channel instead. It is
     * what lets vs_tr_writer_push_data() decline to send a path nobody wants
     * -- without it the producer ships every byte of every write to the
     * broker, which is exactly the whole-stream push this project rejected
     * ADIOS2's SST for. */
    std::set<std::string>                sub_paths;

    /* Payload bytes actually produced, for vs_tr_writer_bytes_pushed(). */
    uint64_t bytes_pushed = 0;

    /* Retained so vs_tr_reader_ack_step() has something to acknowledge:
     * Diaspora acknowledges a specific Event, not a step number. */
    std::optional<diaspora::Event> last_event;

    /* A pull() that times out does NOT cancel the underlying request: the
     * next event to arrive silently satisfies that abandoned future, and is
     * lost to a caller who has since issued a fresh pull(). This cost a real
     * bug -- the writer's very first control-topic announcement vanished into
     * the future left behind by the start-up drain. So every consumer's
     * in-flight future is retained and re-waited rather than re-issued. */
    std::optional<diaspora::Future<std::optional<diaspora::Event>>> pending_data;
    std::optional<diaspora::Future<std::optional<diaspora::Event>>> pending_control;

    /* Reader-side selection refinement (see vs_tr_set_selection_cb()). */
    vs_tr_selection_fn selection_fn = nullptr;

    /* Requested-but-unimplementable features, for honest reporting rather
     * than silent degradation (see vs_tr_diaspora_unsupported()). */
    unsigned unsupported = 0;
};

/* How long to keep polling a topic that has gone quiet before concluding
 * it has been drained. There is no end-of-log signal to wait for -- a
 * streaming consumer blocks for the producer's next event instead -- so
 * "latest schema" can only be found by reading forward until nothing more
 * arrives within this window. See vs_tr_reader_get_schema(). */
#define DSA_DRAIN_MS 200

/* Poll granularity once the control topic has started yielding events: long
 * enough not to spin, short enough that a rendezvous is not dominated by it. */
#define DSA_CTL_POLL_MS 20

/* Bits for vs_tr_t::unsupported. */
#define VS_DSA_UNSUP_REFILTER    0x01u
#define VS_DSA_UNSUP_SHAPE       0x02u
#define VS_DSA_UNSUP_CONVERT     0x04u
#define VS_DSA_UNSUP_PREDICATE   0x08u
#define VS_DSA_UNSUP_SELECTION   0x10u
#define VS_DSA_UNSUP_WAITSUBS    0x20u
#define VS_DSA_UNSUP_MINACKED    0x40u
#define VS_DSA_UNSUP_CURSTEP     0x80u

/* Cap on runs, mirroring tr_mercury.c's: past it the callback coalesces
 * rather than truncating, since over-sending is inefficiency and
 * under-sending is data loss. */
#define DSA_MAX_RUNS 64

/* The subscriber's actual selection within one write, as a list of element
 * runs. Falls back to the single overlap run -- the bounding-span behaviour --
 * whenever the selection cannot be resolved: no callback registered, either
 * space missing, or the callback declining.
 *
 * Called from both the DataSelector (to decide which bytes to pull) and the
 * delivery path (to split what arrived into per-run items). Deterministic and
 * shared precisely so those two cannot disagree about what was requested. */
static std::vector<vs_tr_run_t>
runs_for(vs_tr_t *tr, const sub_t &sub, const nlohmann::json &j, uint64_t *out_ws, uint64_t *out_es)
{
    std::vector<vs_tr_run_t> out;
    uint64_t es = j.value("es", uint64_t{1});
    uint64_t ws = j.value("s", uint64_t{0});
    uint64_t wc = j.value("c", uint64_t{0});
    if (out_ws) *out_ws = ws;
    if (out_es) *out_es = es;

    uint64_t os = 0, oc = 0;
    if (!overlap(ws, wc, sub.sel_start, sub.sel_count, &os, &oc))
        return out; /* no intersection at all */

    vs_tr_run_t bounding{os, oc};

    if (!tr->selection_fn || sub.space_enc.empty() || !j.contains("sp")) {
        out.push_back(bounding);
        return out;
    }
    std::vector<uint8_t> wsp = b64_decode(j["sp"].get<std::string>());
    if (wsp.empty()) {
        out.push_back(bounding);
        return out;
    }

    vs_tr_run_t runs[DSA_MAX_RUNS];
    int n = tr->selection_fn(sub.space_enc.data(), sub.space_enc.size(), wsp.data(), wsp.size(), os,
                             oc, runs, DSA_MAX_RUNS);
    if (n < 0) {
        out.push_back(bounding); /* declined -- send the whole range */
        return out;
    }
    /* The callback reports runs RELATIVE to the range it was handed, not in
     * absolute element coordinates -- tr_mercury.c makes this explicit by
     * seeding its own fallback as {0, overlap_count} rather than
     * {overlap_start, overlap_count}. Reading them as absolute produced runs
     * shifted by the overlap start: a [1,3) subscription served [0,2), which
     * is silent data corruption rather than a visible failure. */
    for (int i = 0; i < n; i++)
        out.push_back(vs_tr_run_t{os + runs[i].start, runs[i].count});
    return out; /* n == 0 legitimately means "touches nothing here" */
}

/* Wait on a consumer's outstanding pull, issuing one only if none is already
 * in flight. See the pending_data/pending_control comment: re-issuing after a
 * timeout drops events. Returns an empty optional on timeout, leaving the
 * request pending for the next call. */
static std::optional<diaspora::Event>
pull_retained(diaspora::Consumer &c,
              std::optional<diaspora::Future<std::optional<diaspora::Event>>> &pending,
              int timeout_ms)
{
    if (!pending)
        pending = c.pull();
    std::optional<diaspora::Event> ev = pending->wait(timeout_ms);
    if (ev)
        pending.reset(); /* satisfied -- the next call issues a fresh pull */
    return ev;
}

/* Drain the control topic into tr->seen_subs / tr->acked. Writer side only.
 * Returns the number of control events absorbed this call. */
static size_t
pump_control(vs_tr_t *tr, uint64_t timeout_ms)
{
    if (!tr || !tr->control_consumer)
        return 0;
    size_t n = 0;
    for (;;) {
        std::optional<diaspora::Event> ev;
        try {
            ev = pull_retained(*tr->control_consumer, tr->pending_control,
                               n ? DSA_CTL_POLL_MS : dsa_timeout(timeout_ms));
        }
        catch (const diaspora::Exception &) {
            break;
        }
        if (!ev)
            break;
        try {
            const auto &j   = ev->metadata().json();
            std::string k   = j.value("k", std::string{});
            std::string rid = j.value("rid", std::string{});
            if (rid.empty())
                continue;
            if (k == "sub") {
                tr->seen_subs.insert(rid);
                std::string path = j.value("path", std::string{});
                if (!path.empty())
                    tr->sub_paths.insert(path);
            }
            else if (k == "ack") {
                uint64_t st = j.value("step", uint64_t{0});
                auto     it = tr->acked.find(rid);
                if (it == tr->acked.end() || st > it->second)
                    tr->acked[rid] = st;
                /* An acking reader is by definition a subscriber. */
                tr->seen_subs.insert(rid);
            }
        }
        catch (...) {
            /* Malformed control event: ignore rather than fail the writer. */
        }
        n++;
    }
    return n;
}

/* Push one control event, reader side. Best-effort: a control topic that is
 * unreachable must not fail the data path. */
static void
announce(vs_tr_t *tr, const char *kind, const char *path, uint64_t step)
{
    if (!tr || !tr->control_producer)
        return;
    try {
        nlohmann::json j;
        j["k"]   = kind;
        j["rid"] = tr->rid;
        if (path)
            j["path"] = path;
        if (step != UINT64_MAX)
            j["step"] = step;
        tr->control_producer->push(diaspora::Metadata{j}, diaspora::DataView{});
        tr->control_producer->flush().wait(-1);
    }
    catch (const diaspora::Exception &) {
    }
}

extern "C" {

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/* na_str is a Mercury address spec in tr_mercury.c ("ofi+tcp", "na+sm").
 * Here it names the Diaspora driver and its bootstrap, "<driver>:<root>":
 *   files:/scratch/topics     -- the built-in files driver
 *   mofka:/path/to/group.json -- a deployed Mofka service
 * A bare path means "files", so an existing VOL_STREAM_NA is not silently
 * reinterpreted as something exotic. */
vs_tr_t *
vs_tr_start(const char *na_str)
{
    std::string spec = na_str ? na_str : "files:/tmp/vol-stream-topics";
    std::string type = "files";
    std::string root = spec;

    auto colon = spec.find(':');
    if (colon != std::string::npos && spec.compare(0, 1, "/") != 0) {
        type = spec.substr(0, colon);
        root = spec.substr(colon + 1);
    }

    try {
        diaspora::Metadata options;
        /* Both the files driver and Mofka take their bootstrap under a
         * driver-specific key; only the files driver is exercised here. */
        if (type == "files")
            options.json()["root_path"] = root;
        else
            options.json()["group_file"] = root;

        auto tr    = new vs_tr_t{diaspora::Driver::New(type.c_str(), options)};
        tr->root   = root;
        /* Identity on the control topic. Per transport handle, not per
         * process: a test that runs a writer and a reader in one process
         * must not have them collide. */
        {
            static std::atomic<unsigned> seq{0};
            tr->rid = std::to_string(static_cast<long>(getpid())) + "-" +
                      std::to_string(seq.fetch_add(1));
        }
        /* One thread: the DataSelector and DataAllocator below close over
         * tr->subs, and tr_mercury.h documents the callbacks as not
         * thread-safe against a concurrent push anyway. */
        tr->pool   = tr->driver.makeThreadPool(diaspora::ThreadCount{1});
        return tr;
    }
    catch (const diaspora::Exception &) {
        return nullptr;
    }
}

void
vs_tr_stop(vs_tr_t *tr)
{
    if (!tr)
        return;
    try {
        if (tr->producer)
            tr->producer->flush().wait(-1);
        if (tr->schema_producer)
            tr->schema_producer->flush().wait(-1);
    }
    catch (const diaspora::Exception &) {
        /* Teardown is best-effort, exactly as vs_tr_stop() is today. */
    }
    delete tr;
}

/* The one place the seam genuinely leaks: this exists so H5VLstream.c can
 * hand tr_bake.c's embedded BAKE provider the same margo instance (M7's
 * Spill policy). There is no margo instance here -- the files driver has no
 * Mercury at all, and Mofka owns its own engine. Returning NULL is correct
 * and is caught by H5VLstream.c's existing NULL check, which disables spill.
 * Under Mofka, spill is the partition manager's job (Warabi + abt-io), so
 * the right end state is for tr_bake.c not to exist on this path at all. */
margo_instance_id
vs_tr_get_mid(vs_tr_t *)
{
    return MARGO_INSTANCE_NULL;
}

/* ------------------------------------------------------------------ */
/* Group membership -> topics                                          */
/* ------------------------------------------------------------------ */

/* group_file is the sidecar path tr_mercury.c writes the Flock group id to,
 * and its basename becomes the topic name here.
 *
 * The sidecar itself must still be written, even though Diaspora needs
 * nothing from it. It is the *handshake*: H5VLstream.c's readers, and the
 * test harnesses, poll for "<filename>.vsgroup" to appear before opening the
 * file at all (user-guide.md 4.2, "the sidecar is the handshake"). An earlier
 * version of this backend skipped it on the grounds that the topic is the
 * rendezvous -- with the result that every reader blocked forever on a file
 * that never appeared, so no reader ever subscribed, so the writer believed
 * it had no subscribers and pushed nothing at all. Three empty topics and a
 * whole suite of failures that looked like model limitations and were not. */
static std::string
topic_of(const char *group_file)
{
    std::string s = group_file ? group_file : "vol_stream";
    auto        slash = s.find_last_of('/');
    if (slash != std::string::npos)
        s = s.substr(slash + 1);
    for (auto &c : s)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            c = '_';
    return s.empty() ? std::string{"vol_stream"} : s;
}

int
vs_tr_writer_start_group(vs_tr_t *tr, const char *group_file)
{
    if (!tr)
        return -1;
    try {
        tr->topic_name = topic_of(group_file);
        /* createTopic is idempotent-by-contract ("if it does not exist
         * yet"), so a rerun against the same root does not fail. Default
         * validator/selector/serializer: an HDF5 write has nothing to
         * validate at the JSON level, and one partition keeps step order
         * total (see the ordering note in the mapping study). */
        tr->driver.createTopic(tr->topic_name, diaspora::Metadata{});
        tr->driver.createTopic(tr->topic_name + "__schema", diaspora::Metadata{});
        tr->driver.createTopic(tr->topic_name + "__control", diaspora::Metadata{});
        tr->topic         = tr->driver.openTopic(tr->topic_name);
        tr->schema_topic  = tr->driver.openTopic(tr->topic_name + "__schema");
        tr->control_topic = tr->driver.openTopic(tr->topic_name + "__control");
        tr->producer     = tr->topic->producer("vol-stream-writer", tr->pool,
                                               diaspora::BatchSize{1},
                                               diaspora::Ordering::Strict);
        tr->schema_producer = tr->schema_topic->producer("vol-stream-schema", tr->pool,
                                                         diaspora::BatchSize{1},
                                                         diaspora::Ordering::Strict);
        /* Consumer name derived from this writer's own rid, so it reads the
         * control topic from offset 0 rather than resuming someone else's
         * position. Then drain what is already there: announcements left by
         * a previous run against the same topic root are not this writer's
         * subscribers, and counting them would satisfy
         * vs_tr_writer_wait_subscribers() with nobody listening. */
        tr->control_consumer = tr->control_topic->consumer("vs-ctl-" + tr->rid, tr->pool,
                                                           diaspora::BatchSize{1});
        pump_control(tr, 0);
        tr->seen_subs.clear();
        tr->acked.clear();
        tr->sub_paths.clear();

        /* Publish the handshake. Content is the topic name: nothing reads it
         * today (the name is re-derived from the path either way), but a
         * reader that wants to check it is looking at the right stream can,
         * and an empty file would be indistinguishable from a truncated one. */
        if (group_file) {
            FILE *f = std::fopen(group_file, "w");
            if (f) {
                std::fprintf(f, "%s\n", tr->topic_name.c_str());
                std::fclose(f);
                tr->group_file = group_file;
            }
        }

        tr->is_writer = true;
        return 0;
    }
    catch (const diaspora::Exception &) {
        return -1;
    }
}

int
vs_tr_reader_join_group(vs_tr_t *tr, const char *group_file)
{
    if (!tr)
        return -1;
    /* Fail if the writer has not published the handshake yet. tr_mercury.c's
     * reader likewise cannot join a group whose id file is absent, and
     * H5VLstream.c documents the retry as the caller's job. Opening the topic
     * anyway would "succeed" against a topic this process just created
     * implicitly, and then hang waiting for a writer that is not there. */
    {
        FILE *f = group_file ? std::fopen(group_file, "r") : nullptr;
        if (!f)
            return -1;
        std::fclose(f);
    }

    try {
        tr->topic_name = topic_of(group_file);
        tr->topic        = tr->driver.openTopic(tr->topic_name);
        tr->schema_topic = tr->driver.openTopic(tr->topic_name + "__schema");
        tr->control_topic = tr->driver.openTopic(tr->topic_name + "__control");
        tr->control_producer = tr->control_topic->producer("vs-ctl-" + tr->rid, tr->pool,
                                                           diaspora::BatchSize{1},
                                                           diaspora::Ordering::Strict);

        /* The DataSelector is where reader-driven narrowing lives on this
         * backend. It sees only metadata, before any payload byte moves off
         * the server, and returns the sub-view it wants -- so a bounded
         * subscription still costs only its overlap on the server->consumer
         * leg. What it cannot do is stop the producer from having sent the
         * whole write in the first place. */
        auto selector = diaspora::DataSelector{
            [tr](const diaspora::Metadata &md, const diaspora::DataDescriptor &d)
                -> diaspora::DataDescriptor {
                try {
                    const auto &j = md.json();
                    if (!j.contains("k") || j["k"] != "data")
                        return diaspora::DataDescriptor{}; /* step marker: no data */

                    std::string path = j.value("path", std::string{});
                    std::lock_guard<std::mutex> g(tr->lock);
                    auto                        it = tr->subs.find(path);
                    if (it == tr->subs.end())
                        return diaspora::DataDescriptor{}; /* not subscribed: decline */

                    uint64_t es = 1, ws = 0;
                    auto     runs = runs_for(tr, it->second, j, &ws, &es);
                    if (runs.empty())
                        return diaspora::DataDescriptor{}; /* touches nothing here */
                    if (runs.size() == 1) {
                        uint64_t wc = j.value("c", uint64_t{0});
                        if (runs[0].start == ws && runs[0].count == wc)
                            return d; /* whole thing */
                        return d.makeSubView((runs[0].start - ws) * es, runs[0].count * es);
                    }
                    /* A genuinely non-contiguous selection. The server honours
                     * an unstructured descriptor, so only the selected bytes
                     * come back -- not the bounding box. */
                    std::vector<diaspora::DataDescriptor::Contiguous> segs;
                    segs.reserve(runs.size());
                    for (const auto &r : runs)
                        segs.push_back({(r.start - ws) * es, r.count * es});
                    return d.makeUnstructuredView(segs);
                }
                catch (...) {
                    return diaspora::DataDescriptor{};
                }
            }};

        auto allocator = diaspora::DataAllocator{
            [](const diaspora::Metadata &, const diaspora::DataDescriptor &d) -> diaspora::DataView {
                /* Freed in vs_tr_reader_wait_data() after the copy out. A
                 * real implementation would hand HDF5's own destination
                 * buffer straight to the RDMA, which this API supports and
                 * is one of its better properties. */
                if (!d.size())
                    return diaspora::DataView{};
                char *p = static_cast<char *>(std::malloc(d.size()));
                return diaspora::DataView{p, d.size()};
            }};

        tr->consumer = tr->topic->consumer("vol-stream-reader", tr->pool, diaspora::BatchSize{1},
                                          selector, allocator);
        return 0;
    }
    catch (const diaspora::Exception &) {
        return -1;
    }
}

int
vs_tr_reader_leave_group(vs_tr_t *tr)
{
    if (!tr || !tr->consumer)
        return -1;
    try {
        /* ConsumerInterface::unsubscribe() exists but is not reachable from
         * the public diaspora::Consumer handle -- no accessor and no cast to
         * the interface (unlike diaspora::Event, which offers one). Dropping
         * the handle is the only public way to stop consuming; worth an
         * upstream issue, since a consumer that wants to stop but keep its
         * topic open has no clean way to say so. */
        tr->consumer.reset();
        return 0;
    }
    catch (const diaspora::Exception &) {
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Step markers                                                        */
/* ------------------------------------------------------------------ */

int
vs_tr_writer_broadcast_step_ready(vs_tr_t *tr, uint64_t physical_step, uint64_t wall_time_ns)
{
    if (!tr || !tr->producer)
        return -1;
    try {
        nlohmann::json j;
        j["k"]    = "step";
        j["step"] = physical_step;
        j["wt"]   = wall_time_ns;
        tr->producer->push(diaspora::Metadata{j}, diaspora::DataView{});
        /* tr_mercury.c's broadcast has returned once every live member has
         * the notification; the nearest equivalent is making the event
         * durable before returning, so a reader cannot miss it. */
        tr->producer->flush().wait(-1);
        tr->cur_step    = physical_step;
        tr->cur_wall_ns = wall_time_ns;
        tr->have_step   = true;
        return 0;
    }
    catch (const diaspora::Exception &) {
        return -1;
    }
}

/* Drain events until a step marker turns up, parking any data events in
 * tr->ready for vs_tr_reader_wait_data(). One queue, two drains -- the same
 * shape as tr_mercury.c's separate pending/data queues, for the same
 * reason: a reader waiting on a step must not lose payload that arrived
 * first. */
static int
pump(vs_tr_t *tr, uint64_t timeout_ms, bool want_step)
{
    if (!tr || !tr->consumer)
        return -1;
    int budget = dsa_timeout(timeout_ms);
    for (;;) {
        std::optional<diaspora::Event> ev;
        try {
            ev = pull_retained(*tr->consumer, tr->pending_data, budget);
        }
        catch (const diaspora::Exception &) {
            return -1;
        }
        if (!ev)
            return -1; /* timeout */

        const auto &j = ev->metadata().json();
        std::string k = j.value("k", std::string{});

        if (k == "step") {
            tr->cur_step    = j.value("step", uint64_t{0});
            tr->cur_wall_ns = j.value("wt", uint64_t{0});
            tr->have_step   = true;
            if (want_step)
                return 0;
            continue;
        }
        if (k == "data") {
            item_t it;
            it.step     = j.value("step", uint64_t{0});
            it.path     = j.value("path", std::string{});
            uint64_t es = j.value("es", uint64_t{1});
            uint64_t ws = j.value("s", uint64_t{0});
            uint64_t wc = j.value("c", uint64_t{0});

            std::lock_guard<std::mutex> g(tr->lock);
            auto                        sit = tr->subs.find(it.path);
            if (sit == tr->subs.end())
                continue;
            auto runs = runs_for(tr, sit->second, j, &ws, &es);
            if (runs.empty())
                continue;
            if (j.contains("t"))
                it.type_enc = b64_decode(j["t"].get<std::string>());

            /* The payload arrives as the runs concatenated, in order -- the
             * same order runs_for() produced for the DataSelector. Split it
             * back into one item per run, each truthfully labelled with its
             * own elem_start/elem_count, exactly as tr_mercury.c sends one
             * push per run. */
            uint64_t total = 0;
            for (const auto &r : runs)
                total += r.count * es;

            std::vector<uint8_t> flat(total);
            const auto          &dv  = ev->data();
            uint64_t             off = 0;
            for (size_t sg = 0; sg < dv.segments().size() && off < total; sg++) {
                uint64_t take = dv.segments()[sg].size;
                if (take > total - off)
                    take = total - off;
                std::memcpy(flat.data() + off, dv.segments()[sg].ptr, take);
                off += take;
                std::free(dv.segments()[sg].ptr);
            }

            uint64_t cursor = 0;
            for (const auto &r : runs) {
                item_t ri;
                ri.step       = it.step;
                ri.path       = it.path;
                ri.type_enc   = it.type_enc;
                ri.elem_start = r.start;
                ri.elem_count = r.count;
                uint64_t nb   = r.count * es;
                if (cursor + nb > flat.size())
                    break; /* short payload -- deliver what is whole */
                ri.buf.assign(flat.begin() + cursor, flat.begin() + cursor + nb);
                cursor += nb;
                tr->ready.push_back(std::move(ri));
            }
            tr->last_event = ev; /* for vs_tr_reader_ack_step()'s acknowledge() */
            if (!want_step)
                return 0;
            continue;
        }
        /* Unknown kind: ignore, forward compatible. */
    }
}

int
vs_tr_reader_wait_step_ready(vs_tr_t *tr, uint64_t timeout_ms, uint64_t *physical_step,
                              uint64_t *wall_time_ns)
{
    if (pump(tr, timeout_ms, true) != 0)
        return -1;
    if (physical_step)
        *physical_step = tr->cur_step;
    if (wall_time_ns)
        *wall_time_ns = tr->cur_wall_ns;
    return 0;
}

/* NO EQUIVALENT. tr_mercury.c answers this with an RPC to the writer, which
 * is how a late joiner is seeded with the current step instead of blocking
 * for the next one. An append-only log has no "where is the producer now"
 * query: the only way to find the writer's latest step is to consume
 * forward to the end of the partition, which for a late joiner means
 * replaying everything it missed -- the opposite of seeding. Returns the
 * last step this reader actually saw, and -1 if it has seen none. */
int
vs_tr_reader_get_current_step(vs_tr_t *tr, uint64_t *physical_step, uint64_t *wall_time_ns)
{
    if (!tr)
        return -1;
    tr->unsupported |= VS_DSA_UNSUP_CURSTEP;
    if (!tr->have_step)
        return -1;
    if (physical_step)
        *physical_step = tr->cur_step;
    if (wall_time_ns)
        *wall_time_ns = tr->cur_wall_ns;
    return 0;
}

/* Implemented on the control topic rather than by asking Diaspora, which
 * cannot answer it. Counts distinct rids, so one reader subscribing to three
 * paths is one subscriber -- tr_mercury.c's documented semantics. n_expected
 * of 0 is 0, and a timeout is -1, same as there. */
int
vs_tr_writer_wait_subscribers(vs_tr_t *tr, uint64_t n_expected, uint64_t timeout_ms)
{
    if (!tr || !tr->control_consumer)
        return -1;
    if (n_expected == 0)
        return 0;

    /* The whole timeout is spent inside pull(), so unlike tr_mercury.c there
     * is no sleep here and no progress engine to keep alive: the Diaspora
     * consumer's own thread does the waiting. */
    uint64_t remaining = timeout_ms;
    for (;;) {
        if (tr->seen_subs.size() >= static_cast<size_t>(n_expected))
            return 0;
        if (remaining == 0)
            return -1;
        uint64_t slice = remaining > 250 ? 250 : remaining;
        pump_control(tr, slice);
        remaining -= slice;
    }
}

/* Two halves, because Diaspora splits what one RPC does for us.
 *
 * event.acknowledge() advances this consumer's persisted offset, which is
 * strictly better than what tr_mercury.c offers: a restarted reader resumes
 * where it left off. But that offset lives server-side and the producer
 * cannot read it, so it does nothing for the writer's backpressure. The step
 * number therefore also goes out on the control topic, which is what
 * vs_tr_writer_min_acked_step() actually reads. */
int
vs_tr_reader_ack_step(vs_tr_t *tr, uint64_t physical_step)
{
    if (!tr || !tr->consumer)
        return -1;
    if (tr->last_event) {
        try {
            tr->last_event->acknowledge();
        }
        catch (const diaspora::Exception &) {
            /* Best-effort, as tr_mercury.h documents this call to be. */
        }
    }
    announce(tr, "ack", nullptr, physical_step);
    return 0;
}

/* Also on the control topic. Returns 1 with *min_acked_step set if at least
 * one reader has ever acked, 0 if none has -- "no pressure", the caller
 * applies no queue policy. Note the return convention: 0, not -1, is the
 * documented no-readers answer.
 *
 * Weaker than tr_mercury.c's in one respect that matters: that version skips
 * readers who have left the group, because Flock's SWIM detector tells it who
 * is still alive. A control topic has no liveness signal at all, so a reader
 * that dies after acking step 5 pins the minimum at 5 forever and stalls a
 * Block policy. Fixing it properly needs a heartbeat and an expiry, which is
 * failure detection rebuilt by hand -- the same "it comes back as application
 * code" cost as the rest of this topic. */
int
vs_tr_writer_min_acked_step(vs_tr_t *tr, uint64_t *min_acked_step)
{
    if (!tr || !tr->control_consumer)
        return 0;
    pump_control(tr, 0); /* absorb whatever has arrived, without blocking */
    if (tr->acked.empty())
        return 0;
    uint64_t min = UINT64_MAX;
    for (const auto &kv : tr->acked)
        if (kv.second < min)
            min = kv.second;
    if (min_acked_step)
        *min_acked_step = min;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Subscriptions                                                       */
/* ------------------------------------------------------------------ */

int
vs_tr_reader_subscribe(vs_tr_t *tr, const char *path, uint64_t sel_start, uint64_t sel_count,
                        const uint8_t *dcpl_enc, uint64_t dcpl_enc_len, const uint8_t *space_enc,
                        uint64_t space_enc_len)
{
    if (!tr || !path)
        return -1;
    std::unique_lock<std::mutex> lk(tr->lock);
    sub_t                       &s = tr->subs[path];
    s.sel_start                   = sel_start;
    s.sel_count                   = sel_count;
    if (space_enc && space_enc_len)
        s.space_enc.assign(space_enc, space_enc + space_enc_len);
    else
        s.space_enc.clear();
    if (dcpl_enc && dcpl_enc_len) {
        /* Recorded but unusable: re-filtering to a per-subscriber pipeline
         * is a writer-side act, and there is no writer-side hook. */
        s.dcpl_enc.assign(dcpl_enc, dcpl_enc + dcpl_enc_len);
        tr->unsupported |= VS_DSA_UNSUP_REFILTER;
    }
    lk.unlock();
    /* Tell the writer someone is listening. Counted as a subscriber, not a
     * subscription -- vs_tr_writer_wait_subscribers() dedupes by rid, so one
     * reader subscribing to three paths is still one, matching
     * tr_mercury.c's documented semantics. */
    announce(tr, "sub", path, UINT64_MAX);
    return 0;
}

int
vs_tr_reader_subscribe_type(vs_tr_t *tr, const char *path, const uint8_t *type_enc,
                             uint64_t type_enc_len)
{
    if (!tr || !path)
        return -1;
    std::lock_guard<std::mutex> g(tr->lock);
    auto                        it = tr->subs.find(path);
    if (it == tr->subs.end())
        return -1;
    it->second.has_type = (type_enc && type_enc_len);
    if (it->second.has_type)
        it->second.type_enc.assign(type_enc, type_enc + type_enc_len);
    /* Narrowing to a smaller type is possible on this backend, but only at
     * the reader, after full-precision bytes have already crossed both
     * legs. Accepted so the reader still gets the type it asked for. */
    tr->unsupported |= VS_DSA_UNSUP_CONVERT;
    return 0;
}

int
vs_tr_reader_subscribe_predicate(vs_tr_t *tr, const char *path, const uint8_t *pred_enc,
                                  uint64_t pred_enc_len)
{
    if (!tr || !path)
        return -1;
    std::lock_guard<std::mutex> g(tr->lock);
    auto                        it = tr->subs.find(path);
    if (it == tr->subs.end())
        return -1;
    if (pred_enc && pred_enc_len)
        it->second.pred_enc.assign(pred_enc, pred_enc + pred_enc_len);
    else
        it->second.pred_enc.clear();
    /* A DataSelector could evaluate a predicate only against metadata, so
     * this would work if -- and only if -- the writer published per-write
     * summary statistics (min/max) in the event metadata, ADIOS2-style.
     * Evaluating against values means pulling the values, which is what the
     * predicate exists to avoid. Reported, not silently dropped: a dropped
     * predicate over-sends, which tr_mercury.h calls out as the reason this
     * particular call distinguishes failure. */
    tr->unsupported |= VS_DSA_UNSUP_PREDICATE;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Data path                                                           */
/* ------------------------------------------------------------------ */

int
vs_tr_writer_push_data(vs_tr_t *tr, uint64_t physical_step, const char *path, const void *buf,
                        uint64_t elem_size, uint64_t write_start, uint64_t write_count,
                        const uint8_t *type_enc, uint64_t type_enc_len, const uint8_t *, uint64_t,
                        void *, const uint8_t *space_enc, uint64_t space_enc_len)
{
    if (!tr || !tr->producer || !path)
        return -1;
    try {
        nlohmann::json j;
        j["k"]    = "data";
        j["step"] = physical_step;
        j["path"] = path;
        j["es"]   = elem_size;
        j["s"]    = write_start;
        j["c"]    = write_count;
        if (type_enc && type_enc_len)
            j["t"] = b64_encode(type_enc, type_enc_len);
        if (space_enc && space_enc_len)
            j["sp"] = b64_encode(space_enc, space_enc_len);

        /* Path-level pushdown, the one piece of writer-side narrowing that
         * survives on this backend: absorb any control-topic traffic, then
         * decline outright to send a path nobody has subscribed to. Without
         * this the producer ships every write to the broker and lets the
         * consumer's DataSelector discard it -- narrowing only the
         * broker->reader leg, which is the whole-stream push this project
         * rejected ADIOS2's SST for.
         *
         * What still cannot be done here is everything *within* a subscribed
         * path: element-range overlap, per-subscriber precision, predicate
         * runs. Those need per-subscriber marshalling at push time, and one
         * event on a topic has exactly one payload for all consumers. So a
         * subscribed path still crosses whole. */
        pump_control(tr, 0);
        if (!tr->sub_paths.empty() && tr->sub_paths.find(path) == tr->sub_paths.end())
            return 0; /* nobody wants it -- not an error, just nothing sent */

        /* One event carries the whole write. tr_mercury.c would instead
         * send each subscriber only its own overlap (and, with a predicate,
         * one RPC per matching run) -- that per-subscriber fan-out is
         * exactly what has no counterpart here. */
        diaspora::DataView data{const_cast<void *>(buf), elem_size * write_count};
        tr->producer->push(diaspora::Metadata{j}, data);
        /* Counted once, not per consumer: one event carries one payload no
         * matter how many read it. That asymmetry with tr_mercury.c (which
         * counts per recipient) is real and not an accounting choice -- it is
         * the whole difference between per-subscriber marshalling and a
         * broadcast log, and it is why a subscribed path crosses whole here
         * however little of it any reader wants. */
        tr->bytes_pushed += elem_size * write_count;
        /* DataView does not own its memory and buf belongs to the caller
         * for the duration of this call only, so the push must complete
         * before returning. This forfeits the asynchrony a real backend
         * would want, and is a prototype limitation, not a Diaspora one. */
        tr->producer->flush().wait(-1);
        return 0;
    }
    catch (const diaspora::Exception &) {
        return -1;
    }
}

int
vs_tr_reader_wait_data(vs_tr_t *tr, uint64_t timeout_ms, uint64_t *physical_step, char **out_path,
                        void **out_buf, uint64_t *out_size, uint64_t *out_elem_start,
                        uint64_t *out_elem_count, uint8_t **out_dcpl_enc, uint64_t *out_dcpl_enc_len,
                        uint8_t **out_type_enc, uint64_t *out_type_enc_len, uint32_t *out_filter_mask)
{
    if (!tr)
        return -1;
    if (tr->ready.empty() && pump(tr, timeout_ms, false) != 0)
        return -1;
    if (tr->ready.empty())
        return -1;

    item_t it = std::move(tr->ready.front());
    tr->ready.erase(tr->ready.begin());

    if (physical_step)
        *physical_step = it.step;
    if (out_path)
        *out_path = dup_str(it.path);
    if (out_buf)
        *out_buf = dup_malloc(it.buf.data(), it.buf.size());
    if (out_size)
        *out_size = it.buf.size();
    if (out_elem_start)
        *out_elem_start = it.elem_start;
    if (out_elem_count)
        *out_elem_count = it.elem_count;
    if (out_type_enc)
        *out_type_enc = static_cast<uint8_t *>(dup_malloc(it.type_enc.data(), it.type_enc.size()));
    if (out_type_enc_len)
        *out_type_enc_len = it.type_enc.size();
    /* Never re-filtered on this backend (no writer-side hook), so the
     * reverse-the-filtering out-params are always "not filtered". */
    if (out_dcpl_enc)
        *out_dcpl_enc = nullptr;
    if (out_dcpl_enc_len)
        *out_dcpl_enc_len = 0;
    if (out_filter_mask)
        *out_filter_mask = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Schema (M10)                                                        */
/* ------------------------------------------------------------------ */

int
vs_tr_writer_publish_schema(vs_tr_t *tr, uint64_t physical_step, const uint8_t *blob, uint64_t len)
{
    if (!tr || !tr->schema_producer)
        return -1;
    try {
        nlohmann::json j;
        j["k"]    = "schema";
        j["step"] = physical_step;
        j["len"]  = len;
        diaspora::DataView d{const_cast<void *>(static_cast<const void *>(blob)), len};
        tr->schema_producer->push(diaspora::Metadata{j}, len ? d : diaspora::DataView{});
        tr->schema_producer->flush().wait(-1);
        return 0;
    }
    catch (const diaspora::Exception &) {
        return -1;
    }
}

/* Maps, but awkwardly: "the latest schema is the only one anybody asks for"
 * (tr_mercury.h) is a natural RPC and an unnatural log query. There is no
 * "give me the last event" primitive, so this consumes the schema topic
 * forward and keeps the newest -- which is correct but costs O(steps
 * published) on a long-running stream. A compacted topic, or carrying the
 * schema in the topic's own Metadata, would be the real fix. */
int
vs_tr_reader_get_schema(vs_tr_t *tr, uint64_t timeout_ms, uint64_t *out_physical_step,
                         uint8_t **out_blob, uint64_t *out_len)
{
    if (!tr || !tr->schema_topic)
        return -1;
    try {
        auto sel = diaspora::DataSelector{
            [](const diaspora::Metadata &, const diaspora::DataDescriptor &d)
                -> diaspora::DataDescriptor { return d; }};
        auto alloc = diaspora::DataAllocator{
            [](const diaspora::Metadata &, const diaspora::DataDescriptor &d) -> diaspora::DataView {
                if (!d.size())
                    return diaspora::DataView{};
                return diaspora::DataView{static_cast<char *>(std::malloc(d.size())), d.size()};
            }};
        /* A fresh consumer name each call, so this always reads from the
         * start of the partition rather than from a remembered offset. */
        static unsigned seq = 0;
        auto            c   = tr->schema_topic->consumer(
            "vol-stream-schema-reader-" + std::to_string(seq++), tr->pool, diaspora::BatchSize{1},
            sel, alloc);

        bool                 got = false;
        uint64_t             step = 0;
        std::vector<uint8_t> blob;
        for (;;) {
            auto ev = c.pull().wait(got ? DSA_DRAIN_MS : dsa_timeout(timeout_ms));
            if (!ev)
                break; /* drained, or nothing published yet */
            const auto &j = ev->metadata().json();
            if (j.value("k", std::string{}) != "schema")
                continue;
            step = j.value("step", uint64_t{0});
            blob.clear();
            for (const auto &s : ev->data().segments()) {
                const uint8_t *p = static_cast<const uint8_t *>(s.ptr);
                blob.insert(blob.end(), p, p + s.size);
                std::free(s.ptr);
            }
            got = true;
        }
        if (!got)
            return -1;
        if (out_physical_step)
            *out_physical_step = step;
        if (out_blob)
            *out_blob = static_cast<uint8_t *>(dup_malloc(blob.data(), blob.size()));
        if (out_len)
            *out_len = blob.size();
        return 0;
    }
    catch (const diaspora::Exception &) {
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* The five writer-side callbacks: NO EQUIVALENT                        */
/* ------------------------------------------------------------------ */

/* Each of these is registered by H5VLstream.c and, on tr_mercury.c, is
 * consulted per subscriber inside the writer before any byte is sent. A
 * Diaspora producer has no idea a consumer exists, so there is no point at
 * which any of them could run. Accepted (so registration is not an error
 * and H5VLstream.c needs no #ifdef) and recorded, never quietly ignored. */

void
vs_tr_set_refilter_cb(vs_tr_t *tr, vs_tr_refilter_fn fn)
{
    if (tr && fn)
        tr->unsupported |= VS_DSA_UNSUP_REFILTER;
}

void
vs_tr_set_refilter_shape_cb(vs_tr_t *tr, vs_tr_refilter_shape_fn fn)
{
    if (tr && fn)
        tr->unsupported |= VS_DSA_UNSUP_SHAPE;
}

void
vs_tr_set_convert_cb(vs_tr_t *tr, vs_tr_convert_fn fn)
{
    if (tr && fn)
        tr->unsupported |= VS_DSA_UNSUP_CONVERT;
}

void
vs_tr_set_predicate_cb(vs_tr_t *tr, vs_tr_predicate_fn fn)
{
    if (tr && fn)
        tr->unsupported |= VS_DSA_UNSUP_PREDICATE;
}

/* Unlike the other four, this one IS used here -- on the reader, to narrow a
 * pulled event to the subscriber's real selection rather than its bounding
 * box (see runs_for()). It is still recorded, because what it cannot do on
 * this backend is stop those bytes leaving the writer. */
void
vs_tr_set_selection_cb(vs_tr_t *tr, vs_tr_selection_fn fn)
{
    if (tr)
        tr->selection_fn = fn;
}

uint64_t
vs_tr_writer_bytes_pushed(vs_tr_t *tr)
{
    return tr ? tr->bytes_pushed : 0;
}

/* Not part of tr_mercury.h: this backend's own reporting hook, so a caller
 * (or a test) can ask what it asked for that this transport cannot do. */
unsigned
vs_tr_diaspora_unsupported(vs_tr_t *tr)
{
    return tr ? tr->unsupported : 0u;
}

} /* extern "C" */
