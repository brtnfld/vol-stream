/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Purpose: Implementation of tr_mercury.h. See that header for the design
 *          note on scope and the M5 SSG integration.
 *
 *          Every RPC is answered on a plain Argobots handler pool that
 *          Margo owns (rpc_thread_count in vs_tr_start()); the connector
 *          contributes only the pthread mutex/condvar pairs guarding the
 *          reader-side pending-notification queue and the writer-side
 *          last-committed-step cache, per dev-plan.md's "the connector
 *          keeps one lock over its own step and queue state". Plain pthread
 *          primitives are used rather than Argobots ones because the thread
 *          that calls vs_tr_reader_wait_step_ready() (and
 *          vs_tr_writer_broadcast_step_ready(), from end_step()) is the
 *          application's own thread, not one Margo/Argobots manages.
 */

#include "tr_mercury.h"

#include <margo.h>
#include <mercury_macros.h>
#include <mercury_proc_string.h>
#include <ssg.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Diagnostic accounting for the data-push path, enabled by setting
 * VOL_STREAM_PUSH_STATS=1 in the environment and reported once by
 * vs_tr_stop(). Off by default and read exactly once, so a normal run pays
 * one relaxed load of an int per push and nothing else -- cheap enough to
 * leave in, and the only way to attribute writer-side step time to "waiting
 * on a subscriber" versus everything else H5Fend_step() does. */
static int      vs_push_stats_on          = -1;
static uint64_t vs_push_stats_n           = 0;
static uint64_t vs_push_stats_ns          = 0;
static uint64_t vs_push_stats_bytes       = 0;
static uint64_t vs_push_stats_last_end_ns = 0;
static uint64_t vs_push_stats_tail_ns     = 0;

static uint64_t
vs_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
} /* end vs_now_ns() */

MERCURY_GEN_PROC(vs_step_ready_in_t, ((uint64_t)(physical_step))((uint64_t)(wall_time_ns)))
MERCURY_GEN_PROC(vs_step_ready_out_t, ((int32_t)(status)))
MERCURY_GEN_PROC(vs_get_current_step_in_t, ((int32_t)(unused)))
MERCURY_GEN_PROC(vs_get_current_step_out_t, ((int32_t)(status))((uint64_t)(physical_step))((uint64_t)(wall_time_ns)))
/* M7: reader -> writer. member_id identifies the sender explicitly rather
 * than relying on the RPC's source address, since that address is not the
 * SSG member_id (see vs_tr_reader_get_current_step()'s comment on why rank
 * cannot be assumed either) and the writer's lag table is keyed by
 * member_id to stay valid across SSG's own address bookkeeping. */
MERCURY_GEN_PROC(vs_reader_ack_in_t, ((uint64_t)(member_id))((uint64_t)(physical_step)))
MERCURY_GEN_PROC(vs_reader_ack_out_t, ((int32_t)(status)))

/* M8: a length-prefixed raw byte blob -- no builtin Mercury type covers this
 * (hg_string_t is NUL-terminated text; everything else in mercury_proc.h is
 * a fixed scalar), so this is a hand-written proc rather than something
 * MERCURY_GEN_PROC can generate. HG_FREE frees buf on both sides: the
 * sender's own copy after the RPC completes, and the receiver's decoded
 * copy once the caller is done with it (margo_free_input()/
 * margo_free_output() trigger this, same as any other field). */
typedef struct vs_blob_t {
    uint64_t size;
    void    *buf;
} vs_blob_t;

static hg_return_t
hg_proc_vs_blob_t(hg_proc_t proc, void *data)
{
    vs_blob_t  *b = (vs_blob_t *)data;
    hg_return_t ret;

    switch (hg_proc_get_op(proc)) {
        case HG_ENCODE:
        case HG_DECODE:
            if (HG_SUCCESS != (ret = hg_proc_hg_uint64_t(proc, &b->size)))
                return ret;
            if (hg_proc_get_op(proc) == HG_DECODE) {
                b->buf = (b->size > 0) ? malloc(b->size) : NULL;
                if (b->size > 0 && !b->buf)
                    return HG_NOMEM_ERROR;
            }
            if (b->size > 0)
                return hg_proc_bytes(proc, b->buf, b->size);
            return HG_SUCCESS;
        case HG_FREE:
            free(b->buf);
            b->buf = NULL;
            return HG_SUCCESS;
        default:
            return HG_SUCCESS;
    }
}

/* M8: reader -> writer, "send me path's data". M8.5: bounded to the 1-D
 * element range [sel_start, sel_start+sel_count) -- see this file's M8/M8.5
 * header comment. dcpl_enc (M8.5 precision) is the subscriber's own
 * H5Pencode2() bytes for the filter pipeline it wants pushed data
 * re-filtered through; a 0-length blob means none requested, the original
 * raw-bytes behavior.
 *
 * M9: flags/pred_enc carry a predicate. VS_SUB_FLAG_PRED_ONLY means "amend
 * the predicate of the subscription this member already has for path, and
 * touch nothing else" -- it reuses this RPC rather than adding a second one
 * so that vs_tr_reader_subscribe_predicate() inherits the same find-the-
 * writer probing (see vs_tr_reader_subscribe()). matched, in the reply,
 * distinguishes "I am the writer and I found/created that subscription"
 * from "I am the writer but you never subscribed to that path": status
 * alone cannot, since status is also what identifies the writer during
 * probing, so answering non-zero would make the prober keep hunting for a
 * writer that had in fact already answered. */
#define VS_SUB_FLAG_PRED_ONLY 0x1u
/* "Amend only the wanted datatype of an existing subscription" -- the same
 * narrow-an-existing-subscription shape as PRED_ONLY above. A 0-length
 * type_enc clears the narrowing and restores the dataset's own type. */
#define VS_SUB_FLAG_TYPE_ONLY 0x2u
MERCURY_GEN_PROC(vs_subscribe_in_t, ((uint64_t)(member_id))((hg_string_t)(path))((uint64_t)(sel_start))(
                                          (uint64_t)(sel_count))((vs_blob_t)(dcpl_enc))((uint32_t)(flags))(
                                          (vs_blob_t)(pred_enc))((vs_blob_t)(space_enc))(
                                          (vs_blob_t)(type_enc)))
MERCURY_GEN_PROC(vs_subscribe_out_t, ((int32_t)(status))((int32_t)(matched)))
/* M8: writer -> reader, one entry's actual bytes. M8.5: elem_start/
 * elem_count identify which element range of the subscribed object payload
 * covers -- the overlap between what was written and what was requested,
 * not necessarily the subscriber's whole requested range. dcpl_enc/
 * type_enc/filter_mask (M8.5 precision) are set only when this push was
 * re-filtered through the subscriber's own requested pipeline (see
 * vs_tr_refilter_fn's comment) -- 0-length dcpl_enc means payload is raw,
 * unfiltered bytes, the original M8/M8.5 behavior. */
MERCURY_GEN_PROC(vs_data_push_in_t,
                  ((uint64_t)(physical_step))((hg_string_t)(path))((uint64_t)(elem_start))((uint64_t)(
                      elem_count))((vs_blob_t)(payload))((vs_blob_t)(dcpl_enc))((vs_blob_t)(type_enc))(
                      (uint32_t)(filter_mask)))
MERCURY_GEN_PROC(vs_data_push_out_t, ((int32_t)(status)))

/* M10: reader -> writer, "what is in this stream?". The reply's is_writer is
 * separate from status for the reason vs_subscribe_out_t's `matched` field
 * records: status is what identifies the writer while probing members, so it
 * cannot also mean "and I had a schema to give you" -- a fresh writer that
 * has not committed a step yet must still identify itself, or the reader
 * keeps hunting for a writer that already answered. schema is a 0-length
 * blob in that case. */
MERCURY_GEN_PROC(vs_get_schema_in_t, ((int32_t)(unused)))
MERCURY_GEN_PROC(vs_get_schema_out_t,
                  ((int32_t)(status))((int32_t)(is_writer))((uint64_t)(physical_step))((vs_blob_t)(schema)))

typedef struct vs_tr_pending_t {
    uint64_t physical_step;
    uint64_t wall_time_ns;
} vs_tr_pending_t;

/* Writer side: one data push forwarded but not yet completed.
 *
 * The push RPC used to block the writer inside margo_forward_timed() until
 * the subscriber had answered, once per (subscriber, selection run,
 * predicate run, chunk) -- so a step's pushes cost the sum of their round
 * trips, serialized, in the middle of H5Fend_step(). Measured on
 * test/b_stream_grow_tail.c that was ~3ms of a ~35ms run, and on
 * test/b_stream_grow.c ~25ms of ~290ms: 7-9% of the writer's own time spent
 * waiting on a reader that has nothing left to say.
 *
 * Now each push is issued with margo_iforward_timed() and parked here, and
 * the whole set is completed once, just before the step is announced (see
 * vs_tr_drain_pushes()). The same measurements say that window -- the
 * writer work between a step's last push and its step_ready broadcast -- is
 * ~28ms and ~237ms respectively, an order of magnitude more than the pushes
 * cost, so they now overlap it instead of adding to it.
 *
 * owned is the refilter callback's malloc'd output (NULL when the payload
 * pointed into the caller's own buffer), freed once the request completes.
 * It is safe to free the *source* bytes as soon as margo_iforward() returns
 * -- Mercury runs the input proc, copying the payload into the handle's own
 * buffer, before the call returns. That is not an assumption: it was
 * verified directly by forwarding a 1 MiB payload (past the eager path, so
 * the bulk/extra-buffer route was exercised too), overwriting the source
 * the instant iforward() returned, and confirming the receiver still saw
 * every original byte. It has to hold, because H5Fend_step() frees the
 * replay payload buffers between issuing these pushes and draining them. */
typedef struct vs_tr_inflight_t {
    hg_handle_t     handle;
    margo_request   req;
    void           *owned;
    ssg_member_id_t member_id;
} vs_tr_inflight_t;

/* Cap on pushes in flight at once. Reaching it completes the oldest to free
 * a slot, so a write fanning out to many subscribers/runs/chunks stays
 * bounded in outstanding RPCs and in memory rather than issuing thousands.
 * Well above any realistic per-write fan-out, so the common case never
 * touches the backpressure path at all. */
#define VS_TR_MAX_INFLIGHT_PUSHES 256

/* M7: writer-side lag tracking, one entry per reader that has ever acked.
 * Absence from this table (never acked, or departed and pruned) means "not
 * tracked" -- see vs_tr_writer_min_acked_step(). */
typedef struct vs_tr_lag_entry_t {
    ssg_member_id_t member_id;
    uint64_t         acked_step;
} vs_tr_lag_entry_t;

/* M8, writer side: one (subscriber, path, requested range) tuple. A reader
 * with several subscriptions appears once per path, not once per reader --
 * simplest thing that works, and small enough not to need a smarter index.
 * M8.5: sel_start/sel_count is the subscriber's own requested 1-D element
 * range; [0, UINT64_MAX) means "whole object". dcpl_enc/dcpl_enc_len (M8.5
 * precision) is this subscriber's requested filter pipeline, NULL/0 for
 * none. M9: pred_enc/pred_enc_len is this subscriber's predicate, NULL/0
 * for none -- set by a follow-up VS_SUB_FLAG_PRED_ONLY subscribe, and
 * cleared by a plain re-subscribe (a fresh subscription starts with no
 * predicate; see vs_subscribe_ult()). space_enc/space_enc_len is the
 * subscriber's own H5Sencode2() selection -- what sel_start/sel_count is
 * only the bounding span of -- NULL/0 when the subscriber sent none. */
typedef struct vs_tr_sub_entry_t {
    ssg_member_id_t member_id;
    char            *path;
    uint64_t          sel_start;
    uint64_t          sel_count;
    uint8_t          *dcpl_enc;
    uint64_t          dcpl_enc_len;
    uint8_t          *pred_enc;
    uint64_t          pred_enc_len;
    uint8_t          *space_enc;
    uint64_t          space_enc_len;
    /* The datatype this subscriber wants its data delivered AS, rather than
     * the dataset's own -- e.g. a viz client asking for 4-byte float from a
     * double field. NULL/0 means "the dataset's own type", the original
     * behavior. Opaque here; only the registered vs_tr_convert_fn decodes
     * it. */
    uint8_t          *want_type_enc;
    uint64_t          want_type_enc_len;
} vs_tr_sub_entry_t;

/* M8, reader side: one pushed data item queued for vs_tr_reader_wait_data().
 * M8.5: elem_start/elem_count is the element range buf actually covers (see
 * vs_data_push_in_t's comment). dcpl_enc/type_enc/filter_mask (M8.5
 * precision) are set only when buf is re-filtered bytes rather than raw
 * ones -- NULL/0 dcpl_enc means raw, the original M8/M8.5 behavior. */
typedef struct vs_tr_data_item_t {
    uint64_t physical_step;
    char    *path;
    void    *buf;
    uint64_t size;
    uint64_t elem_start;
    uint64_t elem_count;
    uint8_t *dcpl_enc;
    uint64_t dcpl_enc_len;
    uint8_t *type_enc;
    uint64_t type_enc_len;
    uint32_t filter_mask;
} vs_tr_data_item_t;

struct vs_tr_t {
    margo_instance_id mid;
    hg_id_t            step_ready_rpc_id;
    hg_id_t            get_current_step_rpc_id;
    hg_id_t            reader_ack_rpc_id;
    hg_id_t            subscribe_rpc_id;
    hg_id_t            data_push_rpc_id;
    hg_id_t            get_schema_rpc_id;

    /* M5: SSG group this process created (writer) or joined (reader).
     * SSG_GROUP_ID_INVALID until vs_tr_writer_start_group()/
     * vs_tr_reader_join_group() succeeds. is_writer only matters once
     * group_id is valid -- it decides whether vs_tr_stop() destroys the
     * group or leaves it. */
    ssg_group_id_t group_id;
    int             is_writer;

    /* Writer side: the last step committed, answered to a
     * get_current_step RPC (a late joiner's "coherent view") and updated
     * by every vs_tr_writer_broadcast_step_ready() call. */
    pthread_mutex_t last_step_lock;
    int             has_last_step;
    uint64_t        last_physical_step;
    uint64_t        last_wall_time_ns;

    /* M10, writer side: the schema blob most recently handed over by
     * vs_tr_writer_publish_schema(), served verbatim to a get_schema RPC.
     * Opaque bytes -- this module never looks inside. NULL until the first
     * end_step() publishes one, which is why the reply carries is_writer
     * separately from status. */
    pthread_mutex_t schema_lock;
    uint8_t        *schema_blob;
    uint64_t        schema_len;
    uint64_t        schema_step;

    /* Reader side: queue of step_ready notifications (including a
     * late-joiner seed from vs_tr_reader_join_group()) not yet consumed by
     * vs_tr_reader_wait_step_ready(). */
    pthread_mutex_t  pending_lock;
    pthread_cond_t   pending_cond;
    vs_tr_pending_t *pending;
    size_t           n_pending;
    size_t           cap_pending;
    int              stopped;

    /* M7, reader side: the group member that answered a get_current_step
     * query, cached the first time vs_tr_reader_get_current_step() finds it
     * so vs_tr_reader_ack_step() does not need to re-probe every member on
     * every step. There is exactly one writer per group in this
     * connector's model, and it does not change mid-session. */
    ssg_member_id_t writer_member_id;
    int              has_writer_member_id;

    /* M7, writer side: per-reader lag table (see vs_tr_lag_entry_t). Grown
     * lazily on a reader's first ack; entries for members no longer in the
     * group are ignored (not pruned) by vs_tr_writer_min_acked_step() --
     * simpler than hooking SSG's membership callback, and just as correct
     * since the table is small and re-checked against live membership on
     * every query. */
    pthread_mutex_t     lag_lock;
    vs_tr_lag_entry_t *lag_table;
    size_t              n_lag;
    size_t              cap_lag;

    /* M8, writer side: subscription table (see vs_tr_sub_entry_t). Grown
     * lazily. Unlike lag_table, entries here *are* pruned when SSG reports
     * the owning member dead or departed (vs_membership_cb()), because the
     * push loop pays a real RPC timeout per stale entry per step. */
    pthread_mutex_t     sub_lock;
    vs_tr_sub_entry_t *sub_table;
    size_t              n_sub;
    size_t              cap_sub;

    /* M8, reader side: queue of pushed data items not yet consumed by
     * vs_tr_reader_wait_data() -- same shape as the pending/pending_lock/
     * pending_cond step_ready queue above, kept separate since the two are
     * drained independently and carry different payloads. */
    pthread_mutex_t     data_lock;
    pthread_cond_t      data_cond;
    vs_tr_data_item_t *data_queue;
    size_t              n_data;
    size_t              cap_data;

    /* M8.5, writer side: precision-refiltering callback, see vs_tr_refilter_
     * fn's comment in tr_mercury.h. NULL (calloc()'s default) until
     * vs_tr_set_refilter_cb() is called -- every push is raw bytes, the
     * original M8/M8.5 behavior, until then. */
    vs_tr_refilter_fn refilter_fn;

    /* M8.5 follow-up, writer side: chunk-shape query callback, see
     * vs_tr_refilter_shape_fn's comment in tr_mercury.h. NULL until
     * vs_tr_set_refilter_shape_cb() is called -- a refiltered push then
     * always spans the whole run in one RPC, exactly the original M8.5
     * behavior. */
    vs_tr_refilter_shape_fn refilter_shape_fn;

    /* Datatype narrowing (see vs_tr_convert_fn in tr_mercury.h). NULL until
     * vs_tr_set_convert_cb() is called -- every push then carries the
     * dataset's own type, the pre-narrowing behavior. */
    vs_tr_convert_fn convert_fn;

    /* M9, writer side: predicate-evaluation callback, see vs_tr_predicate_
     * fn's comment in tr_mercury.h. NULL until vs_tr_set_predicate_cb() is
     * called -- a predicate is then still carried and stored, just never
     * consulted, so every push goes out whole. */
    vs_tr_predicate_fn predicate_fn;

    /* M8.5 follow-up, writer side: selection-refinement callback, see
     * vs_tr_selection_fn's comment in tr_mercury.h. NULL until
     * vs_tr_set_selection_cb() is called -- routing then stays on the flat
     * bounding span a subscription's bounds describe. */
    vs_tr_selection_fn selection_fn;

    /* Writer side: data pushes forwarded but not yet completed (see
     * vs_tr_inflight_t and vs_tr_push_one_item()). Touched only by the
     * application's own step thread -- the thread that runs replay, and so
     * the only thread that calls vs_tr_writer_push_data(),
     * vs_tr_writer_broadcast_step_ready() or vs_tr_stop() -- so unlike
     * sub_table/lag_table/data_queue this needs no lock of its own. The
     * callbacks consulted alongside it are documented (tr_mercury.h) as not
     * thread-safe against a concurrent push either, for the same reason. */
    vs_tr_inflight_t *inflight;
    size_t             n_inflight;
    size_t             cap_inflight;

    /* Members whose push failed during the current vs_tr_writer_push_data()
     * call, so the rest of that call stops forwarding to them -- the async
     * spelling of the member_unreachable guard (see vs_tr_drain_one()).
     * Reset at the top of every push call: a later write retries a member
     * from scratch, exactly as the synchronous version did. */
    ssg_member_id_t *failed;
    size_t            n_failed;
    size_t            cap_failed;
};

/* Defined with the rest of the in-flight push bookkeeping, below
 * vs_tr_writer_push_data()'s helpers; declared here for vs_tr_stop(). */
static void vs_tr_drain_pushes(vs_tr_t *tr);

/* Capture the group's current members as stable IDs, for any loop that then
 * does blocking work per member.
 *
 * An SSG *rank* is a position in a list that is re-sorted whenever a member
 * joins or leaves -- it is not an identity. Reading ssg_get_group_size()
 * once and then walking ranks 0..N-1 with a blocking RPC in the body is
 * therefore a time-of-check/time-of-use bug: a member departing mid-loop
 * shrinks the group, the top ranks stop existing, and
 * ssg_get_group_member_id_from_rank() correctly fails for them -- silently
 * skipping members that had nothing to do with the departure. That is not
 * hypothetical; it cost real, reproducible data loss in
 * vs_tr_writer_push_data() (see its comment and docs/dev-plan.md), found
 * only because test/b_push_fanout.c had eight subscribers closing while the
 * writer was still pushing.
 *
 * An ssg_member_id_t, by contrast, stays valid whether or not that member is
 * still present, so resolving it later fails for that member alone. Taking
 * the whole snapshot in one tight pass with no blocking work in it is what
 * shrinks the vulnerable window to nothing.
 *
 * Returns the number of members captured (excluding self, which every
 * caller skips anyway) and stores a malloc'd array in *out, or returns -1
 * with *out NULL. A 0 return with *out NULL just means "nobody else here",
 * which is not an error for any caller. */
static int
vs_tr_snapshot_members(vs_tr_t *tr, ssg_member_id_t self_id, ssg_member_id_t **out)
{
    ssg_member_id_t *members;
    int               group_size, i, n = 0;

    *out = NULL;

    if (tr->group_id == SSG_GROUP_ID_INVALID)
        return -1;
    if (SSG_SUCCESS != ssg_get_group_size(tr->group_id, &group_size))
        return -1;
    if (group_size <= 0)
        return 0;
    if (NULL == (members = (ssg_member_id_t *)malloc((size_t)group_size * sizeof(*members))))
        return -1;

    for (i = 0; i < group_size; i++) {
        ssg_member_id_t member_id;

        /* A failure here is now benign in a way it was not before: the loop
         * body is pure bookkeeping, so nothing has blocked yet and the view
         * cannot have moved much. A member missed here is picked up by the
         * next call. */
        if (SSG_SUCCESS != ssg_get_group_member_id_from_rank(tr->group_id, i, &member_id))
            continue;
        if (member_id == self_id)
            continue;
        members[n++] = member_id;
    }

    if (n == 0) {
        free(members);
        return 0;
    }
    *out = members;
    return n;
} /* end vs_tr_snapshot_members() */

/*-------------------------------------------------------------------------
 * Small helpers
 *-------------------------------------------------------------------------
 */
static void
vs_push_pending(vs_tr_t *tr, uint64_t physical_step, uint64_t wall_time_ns)
{
    pthread_mutex_lock(&tr->pending_lock);

    if (tr->n_pending == tr->cap_pending) {
        size_t           new_cap = tr->cap_pending ? tr->cap_pending * 2 : 8;
        vs_tr_pending_t *grown   = (vs_tr_pending_t *)realloc(tr->pending, new_cap * sizeof(*tr->pending));

        if (grown) {
            tr->pending     = grown;
            tr->cap_pending = new_cap;
        }
    }
    if (tr->n_pending < tr->cap_pending) {
        tr->pending[tr->n_pending].physical_step = physical_step;
        tr->pending[tr->n_pending].wall_time_ns  = wall_time_ns;
        tr->n_pending++;
    }
    pthread_cond_broadcast(&tr->pending_cond);
    pthread_mutex_unlock(&tr->pending_lock);
} /* end vs_push_pending() */

/* M8: takes ownership of path/buf/dcpl_enc/type_enc on success (frees them
 * itself if the queue cannot grow); the caller's own copies must not be
 * used afterward. M8.5: dcpl_enc/type_enc may each be NULL/0 (the ordinary
 * unfiltered push). */
static void
vs_push_data_item(vs_tr_t *tr, uint64_t physical_step, char *path, void *buf, uint64_t size,
                    uint64_t elem_start, uint64_t elem_count, uint8_t *dcpl_enc, uint64_t dcpl_enc_len,
                    uint8_t *type_enc, uint64_t type_enc_len, uint32_t filter_mask)
{
    pthread_mutex_lock(&tr->data_lock);

    if (tr->n_data == tr->cap_data) {
        size_t              new_cap = tr->cap_data ? tr->cap_data * 2 : 8;
        vs_tr_data_item_t *grown   = (vs_tr_data_item_t *)realloc(tr->data_queue, new_cap * sizeof(*tr->data_queue));

        if (grown) {
            tr->data_queue = grown;
            tr->cap_data    = new_cap;
        }
    }
    if (tr->n_data < tr->cap_data) {
        tr->data_queue[tr->n_data].physical_step = physical_step;
        tr->data_queue[tr->n_data].path          = path;
        tr->data_queue[tr->n_data].buf           = buf;
        tr->data_queue[tr->n_data].size          = size;
        tr->data_queue[tr->n_data].elem_start    = elem_start;
        tr->data_queue[tr->n_data].elem_count    = elem_count;
        tr->data_queue[tr->n_data].dcpl_enc      = dcpl_enc;
        tr->data_queue[tr->n_data].dcpl_enc_len  = dcpl_enc_len;
        tr->data_queue[tr->n_data].type_enc      = type_enc;
        tr->data_queue[tr->n_data].type_enc_len  = type_enc_len;
        tr->data_queue[tr->n_data].filter_mask   = filter_mask;
        tr->n_data++;
    }
    else {
        free(path);
        free(buf);
        free(dcpl_enc);
        free(type_enc);
    }
    pthread_cond_broadcast(&tr->data_cond);
    pthread_mutex_unlock(&tr->data_lock);
} /* end vs_push_data_item() */

static char *
vs_tr_self_address(vs_tr_t *tr)
{
    hg_addr_t self = HG_ADDR_NULL;
    hg_size_t size  = 0;
    char     *buf   = NULL;

    if (HG_SUCCESS != margo_addr_self(tr->mid, &self))
        return NULL;
    if (HG_SUCCESS != margo_addr_to_string(tr->mid, NULL, &size, self) || size == 0) {
        margo_addr_free(tr->mid, self);
        return NULL;
    }
    if (NULL == (buf = (char *)malloc(size))) {
        margo_addr_free(tr->mid, self);
        return NULL;
    }
    if (HG_SUCCESS != margo_addr_to_string(tr->mid, buf, &size, self)) {
        free(buf);
        margo_addr_free(tr->mid, self);
        return NULL;
    }
    margo_addr_free(tr->mid, self);
    return buf;
} /* end vs_tr_self_address() */

/* Free one subscription entry's owned storage and blank it.  Shared by the
 * membership purge below and by vs_tr_stop()'s teardown, so the set of
 * owned pointers is listed in exactly one place. */
static void
vs_tr_sub_entry_clear(vs_tr_sub_entry_t *e)
{
    free(e->path);
    free(e->dcpl_enc);
    free(e->pred_enc);
    free(e->space_enc);
    free(e->want_type_enc);
    memset(e, 0, sizeof(*e));
} /* end vs_tr_sub_entry_clear() */

/* M5: fires on SSG_MEMBER_JOINED/LEFT/DIED for any group this process
 * belongs to. Both ssg_group_create() and ssg_group_join() require one.
 *
 * vs_tr_writer_broadcast_step_ready() does not need to be told about a
 * departure, since it queries live group membership at send time, so a
 * departed member is simply absent from the next broadcast. The
 * *subscription* table is different: it is built from subscribe RPCs and
 * keyed by member, and nothing else prunes it. Left alone, a subscriber
 * that dies keeps its entries forever, and the push loop pays a full
 * margo_forward_timed() timeout against the dead peer on every subsequent
 * step -- vs_tr_member_failed()'s guard is deliberately reset at the top
 * of each push call, so it suppresses repeat attempts *within* one write,
 * never across steps. That is the difference between a bounded per-step
 * cost and an unbounded recurring one, so the purge belongs here, where
 * SWIM's own failure detector is the trigger. */
static void
vs_membership_cb(void *group_data, ssg_member_id_t member_id, ssg_member_update_type_t update_type)
{
    vs_tr_t *tr = (vs_tr_t *)group_data;
    size_t    i;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM SSG membership update: member %lu, type %d\n", (unsigned long)member_id,
           (int)update_type);
#endif

    if (!tr)
        return;
    if (update_type != SSG_MEMBER_DIED && update_type != SSG_MEMBER_LEFT)
        return;

    /* Same lock the subscribe handler takes, and from the same kind of
     * context (an SSG/Margo ULT rather than the application thread). */
    pthread_mutex_lock(&tr->sub_lock);
    i = 0;
    while (i < tr->n_sub) {
        if (tr->sub_table[i].member_id == member_id) {
            vs_tr_sub_entry_clear(&tr->sub_table[i]);
            /* Shift rather than swap-remove: the push loop walks this table
             * in order, and preserving it keeps a departure from reordering
             * the pushes surviving subscribers see. */
            if (i + 1 < tr->n_sub)
                memmove(&tr->sub_table[i], &tr->sub_table[i + 1],
                        (tr->n_sub - i - 1) * sizeof(tr->sub_table[0]));
            tr->n_sub--;
        }
        else
            i++;
    }
    pthread_mutex_unlock(&tr->sub_lock);
} /* end vs_membership_cb() */

/* Distinct subscriber count -- sub_table holds one entry per (member, path),
 * so a single subscriber with three paths is three entries and one
 * subscriber. Caller holds sub_lock. O(n^2), on a table that is one entry
 * per subscribed path. */
static size_t
vs_tr_distinct_subscribers(const vs_tr_t *tr)
{
    size_t i, j, n = 0;

    for (i = 0; i < tr->n_sub; i++) {
        for (j = 0; j < i; j++)
            if (tr->sub_table[j].member_id == tr->sub_table[i].member_id)
                break;
        if (j == i)
            n++;
    }
    return n;
} /* end vs_tr_distinct_subscribers() */

int
vs_tr_writer_wait_subscribers(vs_tr_t *tr, uint64_t n_expected, uint64_t timeout_ms)
{
    uint64_t deadline_ns;

    if (!tr || tr->group_id == SSG_GROUP_ID_INVALID)
        return -1;
    if (n_expected == 0)
        return 0;

    deadline_ns = vs_now_ns() + timeout_ms * 1000000ull;

    for (;;) {
        size_t have;

        pthread_mutex_lock(&tr->sub_lock);
        have = vs_tr_distinct_subscribers(tr);
        pthread_mutex_unlock(&tr->sub_lock);

        if (have >= (size_t)n_expected)
            return 0;
        if (vs_now_ns() >= deadline_ns)
            return -1;

        /* margo_thread_sleep(), not sleep()/nanosleep(): this runs on the
         * application thread, and the subscribe RPCs being waited for are
         * served by Margo ULTs that need the progress engine to keep
         * running. Blocking the OS thread outright would deadlock against
         * exactly the arrivals this loop is waiting for. */
        margo_thread_sleep(tr->mid, 5.0);
    }
} /* end vs_tr_writer_wait_subscribers() */

/*-------------------------------------------------------------------------
 * RPC handlers
 *-------------------------------------------------------------------------
 */
static void
vs_step_ready_ult(hg_handle_t handle)
{
    margo_instance_id   mid = margo_hg_handle_get_instance(handle);
    hg_id_t              id = margo_get_info(handle)->id;
    vs_tr_t             *tr = (vs_tr_t *)margo_registered_data(mid, id);
    vs_step_ready_in_t   in;
    vs_step_ready_out_t  out;

    out.status = -1;

    if (tr && HG_SUCCESS == margo_get_input(handle, &in)) {
        vs_push_pending(tr, in.physical_step, in.wall_time_ns);
        out.status = 0;
        margo_free_input(handle, &in);
    }

    margo_respond(handle, &out);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(vs_step_ready_ult)

static void
vs_get_current_step_ult(hg_handle_t handle)
{
    margo_instance_id          mid = margo_hg_handle_get_instance(handle);
    hg_id_t                     id = margo_get_info(handle)->id;
    vs_tr_t                    *tr = (vs_tr_t *)margo_registered_data(mid, id);
    vs_get_current_step_in_t    in;
    vs_get_current_step_out_t   out;

    memset(&out, 0, sizeof(out));
    out.status = -1;

    if (tr && HG_SUCCESS == margo_get_input(handle, &in)) {
        pthread_mutex_lock(&tr->last_step_lock);
        if (tr->has_last_step) {
            out.status         = 0;
            out.physical_step  = tr->last_physical_step;
            out.wall_time_ns   = tr->last_wall_time_ns;
        }
        pthread_mutex_unlock(&tr->last_step_lock);
        margo_free_input(handle, &in);
    }

    margo_respond(handle, &out);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(vs_get_current_step_ult)

/* M10: writer-side handler for vs_get_schema_in_t. Copies the published blob
 * out from under schema_lock and answers with the copy rather than holding
 * the lock across margo_respond(): the encode happens inside that call, and
 * blocking a concurrent H5Fend_step()'s publish for the length of a network
 * respond would put this module's own bookkeeping in the writer's critical
 * path. Touches nothing but schema_lock -- no HDF5, the same rule every
 * handler here follows (see vs_reader_ack_ult()). */
static void
vs_get_schema_ult(hg_handle_t handle)
{
    margo_instance_id      mid = margo_hg_handle_get_instance(handle);
    hg_id_t                 id  = margo_get_info(handle)->id;
    vs_tr_t                *tr  = (vs_tr_t *)margo_registered_data(mid, id);
    vs_get_schema_in_t      in;
    vs_get_schema_out_t     out;
    uint8_t                *copy = NULL;

    memset(&out, 0, sizeof(out));
    out.status = -1;

    if (tr && HG_SUCCESS == margo_get_input(handle, &in)) {
        pthread_mutex_lock(&tr->schema_lock);
        /* is_writer is set once, by vs_tr_writer_start_group(), and Margo is
         * already listening by then -- so a query can arrive just before it
         * is set and be answered "not the writer". That costs the reader one
         * more attempt of vs_tr_reader_get_schema()'s retry loop and nothing
         * else, which is why this reads it without pretending the write side
         * is synchronized. */
        if (tr->is_writer) {
            out.status     = 0;
            out.is_writer  = 1;
            if (tr->schema_blob && tr->schema_len > 0 &&
                NULL != (copy = (uint8_t *)malloc((size_t)tr->schema_len))) {
                memcpy(copy, tr->schema_blob, (size_t)tr->schema_len);
                out.physical_step = tr->schema_step;
                out.schema.buf     = copy;
                out.schema.size    = tr->schema_len;
            }
        }
        pthread_mutex_unlock(&tr->schema_lock);
        margo_free_input(handle, &in);
    }

    margo_respond(handle, &out);
    /* Ours to free: Mercury frees a *decoded* blob (the far side's, via
     * margo_free_output()), never one the responder encoded from its own
     * memory. */
    free(copy);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(vs_get_schema_ult)

/* M7: writer-side handler for vs_reader_ack_in_t. Runs on a Margo/Argobots
 * RPC-handler thread, not the application's own -- touches only lag_table
 * under lag_lock, never HDF5 (v1 is not H5VL_CAP_FLAG_THREADSAFE; see
 * dev-plan.md's "Threading" section), matching the same discipline
 * vs_step_ready_ult/vs_get_current_step_ult already follow. */
static void
vs_reader_ack_ult(hg_handle_t handle)
{
    margo_instance_id  mid = margo_hg_handle_get_instance(handle);
    hg_id_t              id = margo_get_info(handle)->id;
    vs_tr_t             *tr = (vs_tr_t *)margo_registered_data(mid, id);
    vs_reader_ack_in_t   in;
    vs_reader_ack_out_t  out;

    out.status = -1;

    /* Writer-only, for exactly the reason spelled out in vs_subscribe_ult()
     * below: vs_tr_reader_ack_step() uses the same probe-until-status-0 idiom
     * to locate the writer, so a reader answering 0 here would swallow
     * another reader's ack into its own (never-consulted) lag_table. The
     * writer's min_acked_step would then never see that reader, silently
     * under-reporting how far behind the group is -- M7 queue-policy
     * backpressure applied against an incomplete view. */
    if (tr && !tr->is_writer) {
        margo_respond(handle, &out);
        margo_destroy(handle);
        return;
    }

    if (tr && HG_SUCCESS == margo_get_input(handle, &in)) {
        size_t i;

        pthread_mutex_lock(&tr->lag_lock);

        for (i = 0; i < tr->n_lag; i++)
            if (tr->lag_table[i].member_id == (ssg_member_id_t)in.member_id) {
                if (in.physical_step > tr->lag_table[i].acked_step)
                    tr->lag_table[i].acked_step = in.physical_step;
                break;
            }
        if (i == tr->n_lag) {
            if (tr->n_lag == tr->cap_lag) {
                size_t               new_cap = tr->cap_lag ? tr->cap_lag * 2 : 8;
                vs_tr_lag_entry_t *grown   = (vs_tr_lag_entry_t *)realloc(tr->lag_table,
                                                                            new_cap * sizeof(*tr->lag_table));

                if (grown) {
                    tr->lag_table = grown;
                    tr->cap_lag   = new_cap;
                }
            }
            if (tr->n_lag < tr->cap_lag) {
                tr->lag_table[tr->n_lag].member_id  = (ssg_member_id_t)in.member_id;
                tr->lag_table[tr->n_lag].acked_step = in.physical_step;
                tr->n_lag++;
            }
        }

        pthread_mutex_unlock(&tr->lag_lock);

        out.status = 0;
        margo_free_input(handle, &in);
    }

    margo_respond(handle, &out);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(vs_reader_ack_ult)

/* A copy, not an ownership transfer: used where the source (an `in` struct
 * margo_get_input() decoded) is about to be freed by margo_free_input(), but
 * the destination (a long-lived table entry) needs its own buffer. Returns
 * 0 on success (even for a genuinely empty blob -- *out_buf is then NULL,
 * *out_len 0), -1 only on allocation failure. */
static int
vs_blob_dup(const vs_blob_t *b, uint8_t **out_buf, uint64_t *out_len)
{
    if (b->size == 0) {
        *out_buf = NULL;
        *out_len = 0;
        return 0;
    }
    if (NULL == (*out_buf = (uint8_t *)malloc((size_t)b->size)))
        return -1;
    memcpy(*out_buf, b->buf, (size_t)b->size);
    *out_len = b->size;
    return 0;
} /* end vs_blob_dup() */

/* M8: writer-side handler for vs_subscribe_in_t. Same thread discipline as
 * vs_reader_ack_ult -- touches only sub_table under sub_lock. A repeat
 * subscription from the same member for the same path replaces the stored
 * range rather than adding a second entry -- one (member, path) always
 * means exactly one requested range in this increment (M8.5 does not
 * support a single reader subscribing to several disjoint subranges of the
 * same object).
 *
 * M9: with VS_SUB_FLAG_PRED_ONLY the request amends only the predicate of
 * an existing entry, and creates nothing if there is none -- out.matched
 * reports which happened. Without the flag it is an ordinary subscribe,
 * which *clears* any predicate: re-subscribing describes a subscription
 * afresh, and silently keeping a stale predicate would under-send, the one
 * failure mode this protocol treats as unacceptable. */
static void
vs_subscribe_ult(hg_handle_t handle)
{
    margo_instance_id    mid = margo_hg_handle_get_instance(handle);
    hg_id_t                id = margo_get_info(handle)->id;
    vs_tr_t               *tr = (vs_tr_t *)margo_registered_data(mid, id);
    vs_subscribe_in_t    in;
    vs_subscribe_out_t   out;

    out.status  = -1;
    out.matched = 0;

    /* Only the writer may accept a subscription. Every process in the group
     * registers this handler (Margo registers the full RPC set on both
     * roles -- see vs_tr_start()), and vs_tr_reader_subscribe() finds "the
     * writer" by probing members until one answers status 0. Answering 0
     * from a reader therefore makes a *second* reader mistake the *first*
     * reader for the writer and file its subscription in that reader's
     * sub_table, where nothing ever consults it -- the subscriber then
     * silently receives no data at all. Invisible with a single reader
     * (its only non-self member is the real writer); ~50/50 with two, since
     * it depends on group member ordering. vs_get_current_step_ult() is
     * immune for a different reason: a reader has no has_last_step and so
     * naturally answers -1. */
    if (tr && !tr->is_writer) {
        margo_respond(handle, &out);
        margo_destroy(handle);
        return;
    }

    if (tr && HG_SUCCESS == margo_get_input(handle, &in) && in.path) {
        size_t i;
        int    found     = 0;
        int    pred_only = (in.flags & VS_SUB_FLAG_PRED_ONLY) != 0;
        int    type_only = (in.flags & VS_SUB_FLAG_TYPE_ONLY) != 0;

        pthread_mutex_lock(&tr->sub_lock);

        for (i = 0; i < tr->n_sub; i++)
            if (tr->sub_table[i].member_id == (ssg_member_id_t)in.member_id &&
                strcmp(tr->sub_table[i].path, in.path) == 0) {
                uint8_t *blob_copy;
                uint64_t blob_copy_len;

                if (pred_only) {
                    /* A 0-length blob clears the predicate -- vs_blob_dup()
                     * yields NULL/0 for it, which is exactly "none". */
                    if (vs_blob_dup(&in.pred_enc, &blob_copy, &blob_copy_len) == 0) {
                        free(tr->sub_table[i].pred_enc);
                        tr->sub_table[i].pred_enc     = blob_copy;
                        tr->sub_table[i].pred_enc_len = blob_copy_len;
                        found                          = 1;
                    }
                    break;
                }

                if (type_only) {
                    /* Same shape as PRED_ONLY: a 0-length blob clears the
                     * narrowing and restores the dataset's own type. */
                    if (vs_blob_dup(&in.type_enc, &blob_copy, &blob_copy_len) == 0) {
                        free(tr->sub_table[i].want_type_enc);
                        tr->sub_table[i].want_type_enc     = blob_copy;
                        tr->sub_table[i].want_type_enc_len = blob_copy_len;
                        found                               = 1;
                    }
                    break;
                }

                if (vs_blob_dup(&in.dcpl_enc, &blob_copy, &blob_copy_len) == 0) {
                    free(tr->sub_table[i].dcpl_enc);
                    tr->sub_table[i].dcpl_enc     = blob_copy;
                    tr->sub_table[i].dcpl_enc_len = blob_copy_len;
                }
                tr->sub_table[i].sel_start = in.sel_start;
                tr->sub_table[i].sel_count = in.sel_count;
                {
                    uint8_t *space_copy;
                    uint64_t space_copy_len;

                    if (vs_blob_dup(&in.space_enc, &space_copy, &space_copy_len) == 0) {
                        free(tr->sub_table[i].space_enc);
                        tr->sub_table[i].space_enc     = space_copy;
                        tr->sub_table[i].space_enc_len = space_copy_len;
                    }
                }
                /* See this function's header comment: a plain re-subscribe
                 * starts from no predicate rather than inheriting one. The
                 * wanted datatype resets on the same rule, for the same
                 * reason -- one call, one predictable starting state. */
                free(tr->sub_table[i].pred_enc);
                tr->sub_table[i].pred_enc     = NULL;
                tr->sub_table[i].pred_enc_len = 0;
                free(tr->sub_table[i].want_type_enc);
                tr->sub_table[i].want_type_enc     = NULL;
                tr->sub_table[i].want_type_enc_len = 0;
                found = 1;
                break;
            }

        /* PRED_ONLY never creates a subscription: a predicate narrows one
         * that already exists. Reporting !matched lets the caller fail
         * loudly instead of quietly over-sending forever. */
        if (!found && !pred_only && !type_only) {
            if (tr->n_sub == tr->cap_sub) {
                size_t               new_cap = tr->cap_sub ? tr->cap_sub * 2 : 8;
                vs_tr_sub_entry_t *grown   = (vs_tr_sub_entry_t *)realloc(tr->sub_table,
                                                                            new_cap * sizeof(*tr->sub_table));

                if (grown) {
                    tr->sub_table = grown;
                    tr->cap_sub   = new_cap;
                }
            }
            if (tr->n_sub < tr->cap_sub) {
                char    *path_copy = strdup(in.path);
                uint8_t *dcpl_copy = NULL;
                uint64_t dcpl_copy_len = 0;
                uint8_t *space_copy = NULL;
                uint64_t space_copy_len = 0;

                if (path_copy && vs_blob_dup(&in.dcpl_enc, &dcpl_copy, &dcpl_copy_len) == 0 &&
                    vs_blob_dup(&in.space_enc, &space_copy, &space_copy_len) == 0) {
                    tr->sub_table[tr->n_sub].member_id     = (ssg_member_id_t)in.member_id;
                    tr->sub_table[tr->n_sub].path          = path_copy;
                    tr->sub_table[tr->n_sub].sel_start     = in.sel_start;
                    tr->sub_table[tr->n_sub].sel_count     = in.sel_count;
                    tr->sub_table[tr->n_sub].dcpl_enc      = dcpl_copy;
                    tr->sub_table[tr->n_sub].dcpl_enc_len  = dcpl_copy_len;
                    tr->sub_table[tr->n_sub].pred_enc      = NULL;
                    tr->sub_table[tr->n_sub].pred_enc_len  = 0;
                    tr->sub_table[tr->n_sub].space_enc     = space_copy;
                    tr->sub_table[tr->n_sub].space_enc_len = space_copy_len;
                    tr->n_sub++;
                    found = 1;
                }
                else {
                    free(path_copy);
                    free(dcpl_copy);
                    free(space_copy);
                }
            }
        }

        pthread_mutex_unlock(&tr->sub_lock);

        out.status  = 0;
        out.matched = found;
        margo_free_input(handle, &in);
    }

    margo_respond(handle, &out);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(vs_subscribe_ult)

/* M8: reader-side handler for vs_data_push_in_t -- just queues the item;
 * see vs_push_data_item(). Ownership of the decoded path/payload/dcpl_enc/
 * type_enc buffers moves into the queue (or is freed by vs_push_data_item()
 * itself if the queue could not grow); each moved blob's .buf is set to
 * NULL first so the subsequent margo_free_input() -- which still runs, to
 * free everything NOT handed off (in.path itself; in's own top-level
 * struct) -- sees a harmless no-op free() for those fields instead of a
 * double free (see hg_proc_vs_blob_t's HG_FREE case). */
static void
vs_data_push_ult(hg_handle_t handle)
{
    margo_instance_id     mid = margo_hg_handle_get_instance(handle);
    hg_id_t                 id = margo_get_info(handle)->id;
    vs_tr_t                *tr = (vs_tr_t *)margo_registered_data(mid, id);
    vs_data_push_in_t     in;
    vs_data_push_out_t    out;

    out.status = -1;

    if (tr && HG_SUCCESS == margo_get_input(handle, &in)) {
        char *path_copy = in.path ? strdup(in.path) : NULL;

        if (path_copy) {
            vs_push_data_item(tr, in.physical_step, path_copy, in.payload.buf, in.payload.size, in.elem_start,
                                in.elem_count, (uint8_t *)in.dcpl_enc.buf, in.dcpl_enc.size,
                                (uint8_t *)in.type_enc.buf, in.type_enc.size, in.filter_mask);
            /* ownership moved -- do not let margo_free_input() free these too */
            in.payload.buf  = NULL;
            in.dcpl_enc.buf = NULL;
            in.type_enc.buf = NULL;
            out.status      = 0;
        }
        margo_free_input(handle, &in);
    }

    margo_respond(handle, &out);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(vs_data_push_ult)

/*-------------------------------------------------------------------------
 * Public API
 *-------------------------------------------------------------------------
 */
vs_tr_t *
vs_tr_start(const char *na_str)
{
    vs_tr_t *tr;

    if (NULL == (tr = (vs_tr_t *)calloc(1, sizeof(*tr))))
        return NULL;

    /* Both roles receive RPCs, so both need MARGO_SERVER_MODE and a real
     * listening address, not the lighter client-only path. */
    tr->mid = margo_init(na_str, MARGO_SERVER_MODE, 1, 2);
    if (tr->mid == MARGO_INSTANCE_NULL) {
        free(tr);
        return NULL;
    }

    if (SSG_SUCCESS != ssg_init()) {
        margo_finalize(tr->mid);
        free(tr);
        return NULL;
    }

    tr->group_id = SSG_GROUP_ID_INVALID;

    pthread_mutex_init(&tr->last_step_lock, NULL);
    pthread_mutex_init(&tr->pending_lock, NULL);
    pthread_cond_init(&tr->pending_cond, NULL);
    pthread_mutex_init(&tr->lag_lock, NULL);
    pthread_mutex_init(&tr->sub_lock, NULL);
    pthread_mutex_init(&tr->data_lock, NULL);
    pthread_cond_init(&tr->data_cond, NULL);
    pthread_mutex_init(&tr->schema_lock, NULL);

    tr->step_ready_rpc_id =
        MARGO_REGISTER(tr->mid, "vol-stream:step_ready", vs_step_ready_in_t, vs_step_ready_out_t,
                        vs_step_ready_ult);
    tr->get_current_step_rpc_id =
        MARGO_REGISTER(tr->mid, "vol-stream:get_current_step", vs_get_current_step_in_t,
                        vs_get_current_step_out_t, vs_get_current_step_ult);
    tr->reader_ack_rpc_id =
        MARGO_REGISTER(tr->mid, "vol-stream:reader_ack", vs_reader_ack_in_t, vs_reader_ack_out_t,
                        vs_reader_ack_ult);
    tr->subscribe_rpc_id =
        MARGO_REGISTER(tr->mid, "vol-stream:subscribe", vs_subscribe_in_t, vs_subscribe_out_t,
                        vs_subscribe_ult);
    tr->data_push_rpc_id =
        MARGO_REGISTER(tr->mid, "vol-stream:data_push", vs_data_push_in_t, vs_data_push_out_t,
                        vs_data_push_ult);
    tr->get_schema_rpc_id =
        MARGO_REGISTER(tr->mid, "vol-stream:get_schema", vs_get_schema_in_t, vs_get_schema_out_t,
                        vs_get_schema_ult);

    margo_register_data(tr->mid, tr->step_ready_rpc_id, tr, NULL);
    margo_register_data(tr->mid, tr->get_current_step_rpc_id, tr, NULL);
    margo_register_data(tr->mid, tr->reader_ack_rpc_id, tr, NULL);
    margo_register_data(tr->mid, tr->subscribe_rpc_id, tr, NULL);
    margo_register_data(tr->mid, tr->data_push_rpc_id, tr, NULL);
    margo_register_data(tr->mid, tr->get_schema_rpc_id, tr, NULL);

    return tr;
}

void
vs_tr_set_refilter_cb(vs_tr_t *tr, vs_tr_refilter_fn fn)
{
    if (tr)
        tr->refilter_fn = fn;
}

void
vs_tr_set_convert_cb(vs_tr_t *tr, vs_tr_convert_fn fn)
{
    if (tr)
        tr->convert_fn = fn;
} /* end vs_tr_set_convert_cb() */

void
vs_tr_set_refilter_shape_cb(vs_tr_t *tr, vs_tr_refilter_shape_fn fn)
{
    if (tr)
        tr->refilter_shape_fn = fn;
}

void
vs_tr_set_predicate_cb(vs_tr_t *tr, vs_tr_predicate_fn fn)
{
    if (tr)
        tr->predicate_fn = fn;
}

void
vs_tr_set_selection_cb(vs_tr_t *tr, vs_tr_selection_fn fn)
{
    if (tr)
        tr->selection_fn = fn;
}

void
vs_tr_stop(vs_tr_t *tr)
{
    if (!tr)
        return;

    /* Anything still in flight -- a writer that pushed and then closed
     * without ending the step -- must complete before margo_finalize()
     * below, and its payload freed rather than leaked. */
    vs_tr_drain_pushes(tr);

    if (vs_push_stats_on > 0 && vs_push_stats_n > 0)
        fprintf(stderr,
                "[vol-stream push stats] %llu pushes, %.3f ms total in the forward "
                "(%.1f us/push), %.2f MiB pushed; %.3f ms of writer work follows the last "
                "push of a step before that step is announced (the window an async push "
                "could overlap into)\n",
                (unsigned long long)vs_push_stats_n, (double)vs_push_stats_ns / 1e6,
                (double)vs_push_stats_ns / 1e3 / (double)vs_push_stats_n,
                (double)vs_push_stats_bytes / (1024.0 * 1024.0),
                (double)vs_push_stats_tail_ns / 1e6);

    pthread_mutex_lock(&tr->pending_lock);
    tr->stopped = 1;
    pthread_cond_broadcast(&tr->pending_cond);
    pthread_mutex_unlock(&tr->pending_lock);

    pthread_mutex_lock(&tr->data_lock);
    pthread_cond_broadcast(&tr->data_cond);
    pthread_mutex_unlock(&tr->data_lock);

    if (tr->group_id != SSG_GROUP_ID_INVALID) {
        if (tr->is_writer)
            ssg_group_destroy(tr->group_id);
        else
            ssg_group_leave(tr->group_id);
        tr->group_id = SSG_GROUP_ID_INVALID;

        /* Settle window before tearing down SSG's own runtime state below.
         * This mirrors mochi-ssg's own tests/ssg-join-leave-group.c, which
         * deliberately sleeps between leave/destroy and finalize -- not
         * cosmetic. A peer with a SWIM ping already in flight *to* this
         * member (sent just before our leave/destroy gossiped out) still
         * lands here and gets dispatched to swim_dping_req_recv_ult(),
         * which asserts SSG's global runtime pointer is non-NULL --
         * exactly what ssg_finalize() below sets to NULL. Without giving
         * peers time to actually process our departure first, that
         * dispatch can race ssg_finalize() and crash the process with
         * "swim_dping_req_recv_ult: Assertion `ssg_rt' failed" -- root
         * caused (not guessed) via a real stress-test crash and reading
         * mochi-ssg's swim-fd-ping.c. This was a real usage gap on our
         * side, not purely a third-party bug: one swim-suspect-timeout
         * window's worth of margin (matches the writer's own
         * swim_period_length_ms=200 / swim_suspect_timeout_periods=3
         * group config below) gives that gossip time to land. */
        usleep(750000);
    }

    ssg_finalize();
    margo_finalize(tr->mid);

    pthread_mutex_destroy(&tr->last_step_lock);
    pthread_mutex_destroy(&tr->pending_lock);
    pthread_cond_destroy(&tr->pending_cond);
    pthread_mutex_destroy(&tr->lag_lock);
    pthread_mutex_destroy(&tr->sub_lock);
    pthread_mutex_destroy(&tr->data_lock);
    pthread_cond_destroy(&tr->data_cond);
    pthread_mutex_destroy(&tr->schema_lock);
    free(tr->schema_blob);
    free(tr->pending);
    free(tr->lag_table);
    {
        size_t i;

        for (i = 0; i < tr->n_sub; i++)
            vs_tr_sub_entry_clear(&tr->sub_table[i]);
        free(tr->sub_table);
        for (i = 0; i < tr->n_data; i++) {
            free(tr->data_queue[i].path);
            free(tr->data_queue[i].buf);
            free(tr->data_queue[i].dcpl_enc);
            free(tr->data_queue[i].type_enc);
        }
        free(tr->data_queue);
    }
    free(tr->inflight);
    free(tr->failed);
    free(tr);
}

int
vs_tr_writer_start_group(vs_tr_t *tr, const char *group_file)
{
    char               *self_addr;
    ssg_group_config_t   conf = SSG_GROUP_CONFIG_INITIALIZER;
    const char          *addrs[1];
    int                  ret = -1;

    if (!tr || !group_file)
        return -1;
    if (NULL == (self_addr = vs_tr_self_address(tr)))
        return -1;

    /* Faster than SSG's ~2s-period/10s-suspect HPC-scale defaults: this is
     * a step-notification stream, where sub-second failure detection is
     * worth more than saving a handful of pings a minute. */
    conf.swim_period_length_ms        = 200;
    conf.swim_suspect_timeout_periods = 3;

    addrs[0] = self_addr;
    tr->is_writer = 1;

    if (SSG_SUCCESS ==
        ssg_group_create(tr->mid, "vol-stream", addrs, 1, &conf, vs_membership_cb, tr, &tr->group_id)) {
        if (SSG_SUCCESS == ssg_group_id_store(group_file, tr->group_id, SSG_ALL_MEMBERS))
            ret = 0;
        else {
            ssg_group_destroy(tr->group_id);
            tr->group_id = SSG_GROUP_ID_INVALID;
        }
    }

    free(self_addr);
    return ret;
}

int
vs_tr_reader_join_group(vs_tr_t *tr, const char *group_file)
{
    int      num_addrs = SSG_ALL_MEMBERS;
    uint64_t phys, wall_ns;

    if (!tr || !group_file)
        return -1;

    if (SSG_SUCCESS != ssg_group_id_load(group_file, &num_addrs, &tr->group_id)) {
        tr->group_id = SSG_GROUP_ID_INVALID;
        return -1;
    }
    if (SSG_SUCCESS != ssg_group_join(tr->mid, tr->group_id, vs_membership_cb, tr)) {
        tr->group_id = SSG_GROUP_ID_INVALID;
        return -1;
    }
    tr->is_writer = 0;

    /* Late-joiner "coherent view": seed whatever the writer already
     * committed so the first vs_tr_reader_wait_step_ready() returns
     * immediately instead of blocking for the next write. Retried briefly:
     * right after ssg_group_join() returns, this process's own local group
     * view is not always populated yet (observed directly -- the very next
     * ssg_get_group_member_addr() call for the writer's rank can transiently
     * fail), so the first attempt or two can fail even though the writer
     * has, in fact, already committed something. Not an error either way if
     * every attempt fails or the writer has not committed anything yet --
     * nothing to seed, matching an ordinary non-late join. */
    {
        int i;

        for (i = 0; i < 10; i++) {
            if (0 == vs_tr_reader_get_current_step(tr, &phys, &wall_ns)) {
                vs_push_pending(tr, phys, wall_ns);
                break;
            }
            usleep(50000); /* 50ms */
        }
    }

    return 0;
}

int
vs_tr_reader_leave_group(vs_tr_t *tr)
{
    if (!tr || tr->group_id == SSG_GROUP_ID_INVALID)
        return -1;
    if (SSG_SUCCESS != ssg_group_leave(tr->group_id))
        return -1;
    tr->group_id = SSG_GROUP_ID_INVALID;
    return 0;
}

int
vs_tr_reader_get_current_step(vs_tr_t *tr, uint64_t *physical_step, uint64_t *wall_time_ns)
{
    ssg_member_id_t self_id;
    ssg_member_id_t *members;
    int              n_members, i;
    vs_get_current_step_in_t in;
    int              ret = -1;

    if (!tr || tr->group_id == SSG_GROUP_ID_INVALID)
        return -1;
    if (SSG_SUCCESS != ssg_get_self_id(tr->mid, &self_id))
        return -1;
    if ((n_members = vs_tr_snapshot_members(tr, self_id, &members)) <= 0)
        return -1;

    memset(&in, 0, sizeof(in));

    /* There is no way to identify "the writer" by rank: SSG assigns rank by
     * sorting on member_id, which is a hash of each member's own address,
     * not join order -- a reader can easily end up ranked before the
     * writer. Query every other member instead and use whichever one
     * actually knows a committed step: only the writer ever will (a
     * fellow reader answers status < 0, the same "nothing committed yet"
     * response a genuinely fresh writer gives), and there is exactly one
     * writer per group in this connector's model. */
    for (i = 0; i < n_members && ret != 0; i++) {
        ssg_member_id_t member_id = members[i];
        hg_addr_t        addr;
        hg_handle_t       handle;

        if (SSG_SUCCESS != ssg_get_group_member_addr(tr->group_id, member_id, &addr))
            continue;

        if (HG_SUCCESS == margo_create(tr->mid, addr, tr->get_current_step_rpc_id, &handle)) {
            /* Bounded, like every other forward in this file (1000 ms), and
             * for a sharper reason here: this loop queries members a reader
             * has never spoken to, one of which may be an SSG entry for a
             * peer that has already died. An untimed forward against a dead
             * peer retries until the job's wall clock ends it. A timeout
             * just means this member does not answer, which the loop already
             * treats as "not the writer" and skips. */
            if (HG_SUCCESS == margo_forward_timed(handle, &in, 1000.0)) {
                vs_get_current_step_out_t out;

                if (HG_SUCCESS == margo_get_output(handle, &out)) {
                    if (out.status == 0) {
                        if (physical_step)
                            *physical_step = out.physical_step;
                        if (wall_time_ns)
                            *wall_time_ns = out.wall_time_ns;
                        /* M7: this member just proved it is the writer (only
                         * the writer ever answers status == 0) -- cache it so
                         * vs_tr_reader_ack_step() can target it directly. */
                        tr->writer_member_id     = member_id;
                        tr->has_writer_member_id = 1;
                        ret = 0;
                    }
                    margo_free_output(handle, &out);
                }
            }
            margo_destroy(handle);
        }
        margo_addr_free(tr->mid, addr);
    }

    free(members);
    return ret;
}

int
vs_tr_writer_publish_schema(vs_tr_t *tr, uint64_t physical_step, const uint8_t *blob, uint64_t len)
{
    uint8_t *copy = NULL;

    if (!tr)
        return -1;

    if (blob && len > 0) {
        if (NULL == (copy = (uint8_t *)malloc((size_t)len)))
            return -1;
        memcpy(copy, blob, (size_t)len);
    }

    pthread_mutex_lock(&tr->schema_lock);
    free(tr->schema_blob);
    tr->schema_blob = copy;
    tr->schema_len   = copy ? len : 0;
    tr->schema_step  = physical_step;
    pthread_mutex_unlock(&tr->schema_lock);

    return 0;
}

/* One get_schema forward to one member. Returns 0 only when that member both
 * identified itself as the writer and had a schema published; *out_is_writer
 * separates the two, so a probe can stop hunting once the writer has answered
 * even though it had nothing yet. */
static int
vs_forward_get_schema(vs_tr_t *tr, ssg_member_id_t member_id, int *out_is_writer, uint64_t *out_step,
                       uint8_t **out_blob, uint64_t *out_len)
{
    hg_addr_t           addr;
    hg_handle_t          handle;
    vs_get_schema_in_t  in;
    int                  ret = -1;

    memset(&in, 0, sizeof(in));

    if (SSG_SUCCESS != ssg_get_group_member_addr(tr->group_id, member_id, &addr))
        return -1;

    if (HG_SUCCESS == margo_create(tr->mid, addr, tr->get_schema_rpc_id, &handle)) {
        if (HG_SUCCESS == margo_forward_timed(handle, &in, 1000.0)) {
            vs_get_schema_out_t out;

            if (HG_SUCCESS == margo_get_output(handle, &out)) {
                if (out.status == 0 && out.is_writer) {
                    if (out_is_writer)
                        *out_is_writer = 1;
                    /* Only the writer ever answers status == 0, so cache it
                     * the same way vs_tr_reader_get_current_step() does --
                     * a second query then targets it directly. */
                    tr->writer_member_id     = member_id;
                    tr->has_writer_member_id = 1;

                    if (out.schema.size > 0 && out.schema.buf) {
                        uint8_t *copy = (uint8_t *)malloc((size_t)out.schema.size);

                        /* Copied rather than stolen: margo_free_output()
                         * below runs hg_proc_vs_blob_t()'s HG_FREE over the
                         * decoded buffer, and a schema is small metadata --
                         * not worth teaching the proc about ownership
                         * transfer for. */
                        if (copy) {
                            memcpy(copy, out.schema.buf, (size_t)out.schema.size);
                            *out_blob = copy;
                            *out_len  = out.schema.size;
                            if (out_step)
                                *out_step = out.physical_step;
                            ret = 0;
                        }
                    }
                }
                margo_free_output(handle, &out);
            }
        }
        margo_destroy(handle);
    }
    margo_addr_free(tr->mid, addr);

    return ret;
}

int
vs_tr_reader_get_schema(vs_tr_t *tr, uint64_t timeout_ms, uint64_t *out_physical_step, uint8_t **out_blob,
                         uint64_t *out_len)
{
    ssg_member_id_t self_id;
    uint64_t         deadline_ns;

    if (!tr || !out_blob || !out_len || tr->group_id == SSG_GROUP_ID_INVALID)
        return -1;
    if (SSG_SUCCESS != ssg_get_self_id(tr->mid, &self_id))
        return -1;

    *out_blob    = NULL;
    *out_len     = 0;
    deadline_ns  = vs_now_ns() + timeout_ms * 1000000ull;

    for (;;) {
        int is_writer = 0;

        if (tr->has_writer_member_id &&
            0 == vs_forward_get_schema(tr, tr->writer_member_id, &is_writer, out_physical_step, out_blob,
                                        out_len))
            return 0;

        if (!is_writer) {
            /* Same stable-member-id snapshot every other probe here uses --
             * see vs_tr_snapshot_members(). A member leaving mid-probe must
             * not skip the tail of the list, which can hold the writer. */
            ssg_member_id_t *members;
            int               n_members, i;

            if ((n_members = vs_tr_snapshot_members(tr, self_id, &members)) > 0) {
                for (i = 0; i < n_members; i++) {
                    if (0 == vs_forward_get_schema(tr, members[i], &is_writer, out_physical_step, out_blob,
                                                    out_len)) {
                        free(members);
                        return 0;
                    }
                }
                free(members);
            }
        }

        /* Either no writer was reachable yet (a join whose local group view
         * is still filling in -- vs_tr_reader_join_group() retries for the
         * same reason) or the writer has not committed a step yet. Both are
         * ordinary for a reader that attached first, and both are answered
         * by waiting. */
        if (vs_now_ns() >= deadline_ns)
            break;
        usleep(50000); /* 50ms */
    }

    return -1;
}

int
vs_tr_writer_broadcast_step_ready(vs_tr_t *tr, uint64_t physical_step, uint64_t wall_time_ns)
{
    ssg_member_id_t     self_id;
    ssg_member_id_t    *members;
    int                  n_members, i;
    vs_step_ready_in_t  in;

    if (!tr)
        return -1;

    if (vs_push_stats_on > 0 && vs_push_stats_last_end_ns) {
        vs_push_stats_tail_ns += vs_now_ns() - vs_push_stats_last_end_ns;
        vs_push_stats_last_end_ns = 0;
    }

    /* Settle this step's data pushes before telling anyone the step exists.
     * This is what keeps the async push a pure latency overlap rather than a
     * change in what a subscriber observes: a reader woken by step_ready
     * still finds the step's pushed data already queued, exactly as when
     * every push blocked inline, and pushes never reorder across a step
     * boundary because step N's are all complete before step N+1's are
     * issued. The writer's own work between its last push and this point --
     * the rest of replay, discarding the pending buffer, resolving the
     * step's deferred-request completion -- is what the pushes overlapped
     * with in the meantime. */
    vs_tr_drain_pushes(tr);

    pthread_mutex_lock(&tr->last_step_lock);
    tr->has_last_step     = 1;
    tr->last_physical_step = physical_step;
    tr->last_wall_time_ns  = wall_time_ns;
    pthread_mutex_unlock(&tr->last_step_lock);

    /* "A writer starts with no readers and proceeds": no group yet is not
     * an error, just nothing to push to. */
    if (tr->group_id == SSG_GROUP_ID_INVALID)
        return 0;
    if (SSG_SUCCESS != ssg_get_self_id(tr->mid, &self_id))
        return 0;
    /* Stable member IDs, not ranks -- a reader closing mid-broadcast would
     * otherwise cost *other* readers their step_ready notification, leaving
     * them blocked in H5Fwait_step_ready(). Same defect the data push had;
     * see vs_tr_snapshot_members(). */
    if ((n_members = vs_tr_snapshot_members(tr, self_id, &members)) <= 0)
        return 0;

    in.physical_step = physical_step;
    in.wall_time_ns   = wall_time_ns;

    for (i = 0; i < n_members; i++) {
        ssg_member_id_t member_id = members[i];
        hg_addr_t        addr;
        hg_handle_t       handle;

        if (SSG_SUCCESS != ssg_get_group_member_addr(tr->group_id, member_id, &addr))
            continue;

        /* A stalled or gone reader must not stall the writer: a bounded
         * forward timeout, best-effort, no retry. SSG's SWIM failure
         * detector drops a dead member from the group view on its own
         * schedule (see vs_tr_writer_start_group()'s config); the next
         * broadcast simply will not see it. */
        if (HG_SUCCESS == margo_create(tr->mid, addr, tr->step_ready_rpc_id, &handle)) {
            if (HG_SUCCESS == margo_forward_timed(handle, &in, 1000.0)) {
                vs_step_ready_out_t out;

                if (HG_SUCCESS == margo_get_output(handle, &out))
                    margo_free_output(handle, &out);
            }
            margo_destroy(handle);
        }
        margo_addr_free(tr->mid, addr);
    }

    free(members);
    return 0;
}

/* Shared by both reader-wait functions below. Clamps timeout_ms first so a
 * caller-supplied sentinel meant as "wait indefinitely" (e.g. UINT64_MAX)
 * cannot carry deadline.tv_sec out of struct timespec's range -- a year is
 * already far past anything this protocol's own timeouts (RPC forwards are
 * bounded at 1s each) would ever need to actually wait for. */
#define VS_TR_MAX_WAIT_TIMEOUT_MS (86400000ULL * 365ULL)

static void
vs_tr_compute_deadline(uint64_t timeout_ms, struct timespec *deadline)
{
    if (timeout_ms > VS_TR_MAX_WAIT_TIMEOUT_MS)
        timeout_ms = VS_TR_MAX_WAIT_TIMEOUT_MS;

    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += (time_t)(timeout_ms / 1000);
    deadline->tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

int
vs_tr_reader_wait_step_ready(vs_tr_t *tr, uint64_t timeout_ms, uint64_t *physical_step,
                              uint64_t *wall_time_ns)
{
    struct timespec deadline;
    int             ret = -1;

    if (!tr)
        return -1;

    vs_tr_compute_deadline(timeout_ms, &deadline);

    pthread_mutex_lock(&tr->pending_lock);
    while (tr->n_pending == 0 && !tr->stopped) {
        if (ETIMEDOUT == pthread_cond_timedwait(&tr->pending_cond, &tr->pending_lock, &deadline))
            break;
    }

    if (tr->n_pending > 0) {
        if (physical_step)
            *physical_step = tr->pending[0].physical_step;
        if (wall_time_ns)
            *wall_time_ns = tr->pending[0].wall_time_ns;
        memmove(&tr->pending[0], &tr->pending[1], (tr->n_pending - 1) * sizeof(*tr->pending));
        tr->n_pending--;
        ret = 0;
    }
    pthread_mutex_unlock(&tr->pending_lock);

    return ret;
}

margo_instance_id
vs_tr_get_mid(vs_tr_t *tr)
{
    return tr ? tr->mid : MARGO_INSTANCE_NULL;
}

int
vs_tr_reader_ack_step(vs_tr_t *tr, uint64_t physical_step)
{
    ssg_member_id_t    self_id;
    vs_reader_ack_in_t in;
    int                 ret = -1;

    if (!tr || tr->group_id == SSG_GROUP_ID_INVALID)
        return -1;
    if (SSG_SUCCESS != ssg_get_self_id(tr->mid, &self_id))
        return -1;

    in.member_id     = (uint64_t)self_id;
    in.physical_step = physical_step;

    /* Common case: the writer was already identified by an earlier
     * get_current_step call (always true after vs_tr_reader_join_group()) --
     * target it directly instead of probing every member on every ack. */
    if (tr->has_writer_member_id) {
        hg_addr_t  addr;
        hg_handle_t handle;

        if (SSG_SUCCESS == ssg_get_group_member_addr(tr->group_id, tr->writer_member_id, &addr)) {
            if (HG_SUCCESS == margo_create(tr->mid, addr, tr->reader_ack_rpc_id, &handle)) {
                if (HG_SUCCESS == margo_forward_timed(handle, &in, 1000.0)) {
                    vs_reader_ack_out_t out;

                    if (HG_SUCCESS == margo_get_output(handle, &out)) {
                        if (out.status == 0)
                            ret = 0;
                        margo_free_output(handle, &out);
                    }
                }
                margo_destroy(handle);
            }
            margo_addr_free(tr->mid, addr);
        }
    }

    /* Fallback: the writer has not been identified yet (e.g. the very first
     * ack, racing a join whose seed query has not resolved). Same
     * probe-every-member approach vs_tr_reader_get_current_step() uses. */
    if (ret != 0) {
        /* Stable member IDs, not ranks -- see vs_tr_snapshot_members(). A
         * member leaving while this probe is forwarding would otherwise
         * skip the tail of the list, which can include the writer this is
         * hunting for. */
        ssg_member_id_t *members;
        int               n_members, i;

        if ((n_members = vs_tr_snapshot_members(tr, self_id, &members)) <= 0)
            return ret;

        for (i = 0; i < n_members && ret != 0; i++) {
            ssg_member_id_t member_id = members[i];
            hg_addr_t         addr;
            hg_handle_t        handle;

            if (SSG_SUCCESS != ssg_get_group_member_addr(tr->group_id, member_id, &addr))
                continue;

            if (HG_SUCCESS == margo_create(tr->mid, addr, tr->reader_ack_rpc_id, &handle)) {
                if (HG_SUCCESS == margo_forward_timed(handle, &in, 1000.0)) {
                    vs_reader_ack_out_t out;

                    if (HG_SUCCESS == margo_get_output(handle, &out)) {
                        if (out.status == 0) {
                            tr->writer_member_id     = member_id;
                            tr->has_writer_member_id = 1;
                            ret                       = 0;
                        }
                        margo_free_output(handle, &out);
                    }
                }
                margo_destroy(handle);
            }
            margo_addr_free(tr->mid, addr);
        }
        free(members);
    }

    return ret;
}

int
vs_tr_writer_min_acked_step(vs_tr_t *tr, uint64_t *min_acked_step)
{
    size_t i;
    int    found = 0;
    uint64_t min = 0;

    if (!tr)
        return 0;

    /* Deliberately NOT in the rank-snapshot class vs_tr_snapshot_members()
     * exists for: this walks lag_table, which is already keyed by
     * ssg_member_id_t, and does no blocking work per entry --
     * ssg_get_group_member_rank() below is only a liveness predicate whose
     * rank value is discarded. Correct by construction rather than by
     * snapshotting. (It previously also read ssg_get_group_size() into a
     * variable it never used, which read like rank logic that was not
     * there.) */

    pthread_mutex_lock(&tr->lag_lock);
    for (i = 0; i < tr->n_lag; i++) {
        int      rank;
        int      still_member = (SSG_SUCCESS == ssg_get_group_member_rank(tr->group_id, tr->lag_table[i].member_id, &rank));

        if (!still_member)
            continue; /* departed -- not pruned, just skipped (see struct comment) */

        if (!found || tr->lag_table[i].acked_step < min)
            min = tr->lag_table[i].acked_step;
        found = 1;
    }
    pthread_mutex_unlock(&tr->lag_lock);

    if (found && min_acked_step)
        *min_acked_step = min;

    return found;
}

/* Forwards a fully-filled subscribe request to the writer, finding it the
 * same target-the-cached-writer-then-fall-back-to-probing way
 * vs_tr_reader_ack_step() does -- see that function's comment. Returns 0 if
 * a writer answered at all, setting *out_matched to whether it acted on the
 * request (M9: a PRED_ONLY request naming a path this member never
 * subscribed to is answered, but matches nothing). Split out of
 * vs_tr_reader_subscribe() so the predicate path inherits the probing
 * rather than duplicating it. */
static int
vs_send_subscribe(vs_tr_t *tr, ssg_member_id_t self_id, vs_subscribe_in_t *in, int *out_matched)
{
    int ret = -1;

    if (out_matched)
        *out_matched = 0;

    if (tr->has_writer_member_id) {
        hg_addr_t  addr;
        hg_handle_t handle;

        if (SSG_SUCCESS == ssg_get_group_member_addr(tr->group_id, tr->writer_member_id, &addr)) {
            if (HG_SUCCESS == margo_create(tr->mid, addr, tr->subscribe_rpc_id, &handle)) {
                if (HG_SUCCESS == margo_forward_timed(handle, in, 1000.0)) {
                    vs_subscribe_out_t out;

                    if (HG_SUCCESS == margo_get_output(handle, &out)) {
                        if (out.status == 0) {
                            ret = 0;
                            if (out_matched)
                                *out_matched = out.matched;
                        }
                        margo_free_output(handle, &out);
                    }
                }
                margo_destroy(handle);
            }
            margo_addr_free(tr->mid, addr);
        }
    }

    if (ret != 0) {
        /* Stable member IDs, not ranks -- see vs_tr_snapshot_members(). A
         * member leaving while this probe is forwarding would otherwise
         * skip the tail of the list, which can include the writer this is
         * hunting for. */
        ssg_member_id_t *members;
        int               n_members, i;

        if ((n_members = vs_tr_snapshot_members(tr, self_id, &members)) <= 0)
            return ret;

        for (i = 0; i < n_members && ret != 0; i++) {
            ssg_member_id_t member_id = members[i];
            hg_addr_t         addr;
            hg_handle_t        handle;

            if (SSG_SUCCESS != ssg_get_group_member_addr(tr->group_id, member_id, &addr))
                continue;

            if (HG_SUCCESS == margo_create(tr->mid, addr, tr->subscribe_rpc_id, &handle)) {
                if (HG_SUCCESS == margo_forward_timed(handle, in, 1000.0)) {
                    vs_subscribe_out_t out;

                    if (HG_SUCCESS == margo_get_output(handle, &out)) {
                        if (out.status == 0) {
                            tr->writer_member_id     = member_id;
                            tr->has_writer_member_id = 1;
                            ret                       = 0;
                            if (out_matched)
                                *out_matched = out.matched;
                        }
                        margo_free_output(handle, &out);
                    }
                }
                margo_destroy(handle);
            }
            margo_addr_free(tr->mid, addr);
        }
        free(members);
    }

    return ret;
}

int
vs_tr_reader_subscribe(vs_tr_t *tr, const char *path, uint64_t sel_start, uint64_t sel_count,
                        const uint8_t *dcpl_enc, uint64_t dcpl_enc_len, const uint8_t *space_enc,
                        uint64_t space_enc_len)
{
    ssg_member_id_t     self_id;
    vs_subscribe_in_t in;

    if (!tr || !path || tr->group_id == SSG_GROUP_ID_INVALID)
        return -1;
    if (SSG_SUCCESS != ssg_get_self_id(tr->mid, &self_id))
        return -1;

    in.member_id = (uint64_t)self_id;
    in.path       = (hg_string_t)path;
    in.sel_start  = sel_start;
    in.sel_count  = sel_count;
    /* HG_ENCODE only reads this -- see hg_proc_vs_blob_t() -- so pointing
     * directly at the caller's own buffer needs no copy here. */
    in.dcpl_enc.buf  = (void *)(uintptr_t)dcpl_enc;
    in.dcpl_enc.size = dcpl_enc_len;
    in.flags         = 0;
    /* No narrowing on a plain subscribe. Must be set explicitly: the wire
     * struct is a stack local, so leaving it alone ships garbage. */
    in.type_enc.buf  = NULL;
    in.type_enc.size = 0;
    in.pred_enc.buf  = NULL;
    in.pred_enc.size = 0;
    in.space_enc.buf  = (void *)(uintptr_t)space_enc;
    in.space_enc.size = space_enc_len;

    return vs_send_subscribe(tr, self_id, &in, NULL);
}

int
vs_tr_reader_subscribe_predicate(vs_tr_t *tr, const char *path, const uint8_t *pred_enc,
                                   uint64_t pred_enc_len)
{
    ssg_member_id_t     self_id;
    vs_subscribe_in_t in;
    int                  matched = 0;

    if (!tr || !path || tr->group_id == SSG_GROUP_ID_INVALID)
        return -1;
    if (SSG_SUCCESS != ssg_get_self_id(tr->mid, &self_id))
        return -1;

    in.member_id = (uint64_t)self_id;
    in.path       = (hg_string_t)path;
    /* Ignored under PRED_ONLY -- the stored range is left exactly as the
     * original subscribe set it. */
    in.sel_start     = 0;
    in.sel_count     = UINT64_MAX;
    in.dcpl_enc.buf  = NULL;
    in.dcpl_enc.size = 0;
    in.flags         = VS_SUB_FLAG_PRED_ONLY;
    in.pred_enc.buf  = (void *)(uintptr_t)pred_enc;
    in.pred_enc.size = pred_enc_len;
    /* Ignored under PRED_ONLY, like sel_start/sel_count above -- the stored
     * selection is left exactly as the original subscribe set it. */
    in.space_enc.buf  = NULL;
    in.space_enc.size = 0;
    /* Ignored under PRED_ONLY; set so the wire struct is fully initialized. */
    in.type_enc.buf  = NULL;
    in.type_enc.size = 0;

    if (vs_send_subscribe(tr, self_id, &in, &matched) != 0)
        return -1;
    return matched ? 0 : -1;
}

int
vs_tr_reader_subscribe_type(vs_tr_t *tr, const char *path, const uint8_t *type_enc, uint64_t type_enc_len)
{
    ssg_member_id_t     self_id;
    vs_subscribe_in_t in;
    int                  matched = 0;

    if (!tr || !path || tr->group_id == SSG_GROUP_ID_INVALID)
        return -1;
    if (SSG_SUCCESS != ssg_get_self_id(tr->mid, &self_id))
        return -1;

    in.member_id = (uint64_t)self_id;
    in.path       = (hg_string_t)path;
    /* All ignored under TYPE_ONLY -- only the wanted datatype is amended. */
    in.sel_start      = 0;
    in.sel_count      = UINT64_MAX;
    in.dcpl_enc.buf   = NULL;
    in.dcpl_enc.size  = 0;
    in.pred_enc.buf   = NULL;
    in.pred_enc.size  = 0;
    in.space_enc.buf  = NULL;
    in.space_enc.size = 0;
    in.flags          = VS_SUB_FLAG_TYPE_ONLY;
    in.type_enc.buf   = (void *)(uintptr_t)type_enc;
    in.type_enc.size  = type_enc_len;

    if (vs_send_subscribe(tr, self_id, &in, &matched) != 0)
        return -1;
    return matched ? 0 : -1;
} /* end vs_tr_reader_subscribe_type() */

/*-------------------------------------------------------------------------
 * Writer-side in-flight push bookkeeping (see vs_tr_inflight_t)
 *-------------------------------------------------------------------------
 */

/* Record one issued push. Returns 0 on success, -1 if the list could not be
 * grown -- the caller then completes the request itself rather than losing
 * it. */
static int
vs_tr_inflight_add(vs_tr_t *tr, hg_handle_t handle, margo_request req, void *owned,
                    ssg_member_id_t member_id)
{
    if (tr->n_inflight == tr->cap_inflight) {
        size_t            new_cap = tr->cap_inflight ? tr->cap_inflight * 2 : 8;
        vs_tr_inflight_t *grown;

        if (NULL == (grown = (vs_tr_inflight_t *)realloc(tr->inflight, new_cap * sizeof(*grown))))
            return -1;
        tr->inflight     = grown;
        tr->cap_inflight = new_cap;
    }

    tr->inflight[tr->n_inflight].handle    = handle;
    tr->inflight[tr->n_inflight].req       = req;
    tr->inflight[tr->n_inflight].owned     = owned;
    tr->inflight[tr->n_inflight].member_id = member_id;
    tr->n_inflight++;
    return 0;
} /* end vs_tr_inflight_add() */

/* Has a push to this member already failed during the current write? */
static int
vs_tr_member_failed(vs_tr_t *tr, ssg_member_id_t member_id)
{
    size_t i;

    for (i = 0; i < tr->n_failed; i++)
        if (tr->failed[i] == member_id)
            return 1;
    return 0;
} /* end vs_tr_member_failed() */

static void
vs_tr_member_failed_add(vs_tr_t *tr, ssg_member_id_t member_id)
{
    if (vs_tr_member_failed(tr, member_id))
        return;

    if (tr->n_failed == tr->cap_failed) {
        size_t           new_cap = tr->cap_failed ? tr->cap_failed * 2 : 4;
        ssg_member_id_t *grown;

        if (NULL == (grown = (ssg_member_id_t *)realloc(tr->failed, new_cap * sizeof(*grown))))
            return; /* best-effort: without the note this member just gets
                     * the rest of the write's pushes attempted anyway. */
        tr->failed     = grown;
        tr->cap_failed = new_cap;
    }
    tr->failed[tr->n_failed++] = member_id;
} /* end vs_tr_member_failed_add() */

/* Complete the oldest in-flight push, freeing everything it owns. A failed
 * request (the 1s timeout, or a dead peer) notes its member so the rest of
 * this write stops forwarding there -- the async spelling of the
 * member_unreachable guard vs_tr_writer_push_data() has always applied. */
static void
vs_tr_drain_one(vs_tr_t *tr)
{
    vs_tr_inflight_t *e;

    if (tr->n_inflight == 0)
        return;

    e = &tr->inflight[0];

    if (HG_SUCCESS == margo_wait(e->req)) {
        vs_data_push_out_t out;

        if (HG_SUCCESS == margo_get_output(e->handle, &out))
            margo_free_output(e->handle, &out);
    }
    else
        vs_tr_member_failed_add(tr, e->member_id);

    margo_destroy(e->handle);
    free(e->owned);

    memmove(&tr->inflight[0], &tr->inflight[1], (tr->n_inflight - 1) * sizeof(*tr->inflight));
    tr->n_inflight--;
} /* end vs_tr_drain_one() */

/* Complete every outstanding push. Called before a step is announced (so a
 * subscriber still has the step's data in hand no later than it did when
 * every push blocked inline) and again at teardown. */
static void
vs_tr_drain_pushes(vs_tr_t *tr)
{
    while (tr->n_inflight > 0)
        vs_tr_drain_one(tr);
} /* end vs_tr_drain_pushes() */

/* One vs_data_push_in_t RPC to one already-resolved member address.
 * Factored out of vs_tr_writer_push_data()'s per-run loop so that loop can
 * call this once per run (the original M8/M8.5 behavior) or, when a
 * subscriber's own DCPL asks for a real chunk shape smaller than the run
 * (vs_tr_refilter_shape_fn), once per chunk-sized slice of it -- see
 * vs_tr_refilter_shape_fn's comment in tr_mercury.h for why the far side
 * needs no change to handle either case: each call here is already a
 * complete, independently-decodable push, exactly like today's existing
 * one-push-per-selection-run and one-push-per-predicate-run splits.
 *
 * payload_owned, if non-NULL, is freed once the forward completes --
 * callers pass the refilter callback's malloc'd output there, or NULL when
 * payload_buf points into the caller's own live buffer (the raw-bytes,
 * not-refiltered case). Either way the *source* bytes may die as soon as
 * this returns; see vs_tr_inflight_t's comment.
 *
 * Returns 1 if the member should be treated as unreachable for the rest of
 * this write (matching the existing member_unreachable convention), 0
 * otherwise. Since the forward no longer completes here, that answer now
 * covers a synchronous failure to issue plus anything vs_tr_drain_one()
 * has since reported for this member -- see tr->failed. */
static int
vs_tr_push_one_item(vs_tr_t *tr, hg_addr_t addr, ssg_member_id_t member_id, uint64_t physical_step,
                     const char *path, uint64_t elem_start, uint64_t elem_count, const void *payload_buf,
                     uint64_t payload_len, const uint8_t *dcpl_enc, uint64_t dcpl_enc_len,
                     const uint8_t *type_enc, uint64_t type_enc_len, uint32_t filter_mask,
                     void *payload_owned)
{
    vs_data_push_in_t in;
    hg_handle_t         handle;
    int                  unreachable = 0;
    uint64_t             t0          = 0;

    in.physical_step  = physical_step;
    in.path            = (hg_string_t)(uintptr_t)path;
    in.elem_start      = elem_start;
    in.elem_count      = elem_count;
    in.filter_mask     = filter_mask;
    in.payload.buf     = (void *)(uintptr_t)payload_buf;
    in.payload.size    = payload_len;
    in.dcpl_enc.buf    = (void *)(uintptr_t)dcpl_enc;
    in.dcpl_enc.size   = dcpl_enc_len;
    in.type_enc.buf    = (void *)(uintptr_t)type_enc;
    in.type_enc.size   = type_enc_len;

    /* Same bounded, best-effort forward vs_tr_writer_push_data() always
     * used here -- see its own comment (now on this function) on why one
     * timeout per unreachable member, not one per run/chunk, is the right
     * bound. The 1s timeout still bounds one push; it is now measured from
     * here to whenever vs_tr_drain_pushes() completes the request, and
     * every push in a step is timing out concurrently rather than one after
     * another, so an unreachable member costs the writer *at most* the same
     * single timeout it used to and no longer needs the whole run loop to
     * finish before the next one starts. */
    if (vs_push_stats_on < 0) {
        const char *e = getenv("VOL_STREAM_PUSH_STATS");

        vs_push_stats_on = (e && *e && *e != '0') ? 1 : 0;
    }
    if (vs_push_stats_on)
        t0 = vs_now_ns();

    /* Bound outstanding RPCs before adding one more. */
    if (tr->n_inflight >= VS_TR_MAX_INFLIGHT_PUSHES)
        vs_tr_drain_one(tr);

    if (HG_SUCCESS == margo_create(tr->mid, addr, tr->data_push_rpc_id, &handle)) {
        margo_request req;

        if (HG_SUCCESS == margo_iforward_timed(handle, &in, 1000.0, &req)) {
            if (0 == vs_tr_inflight_add(tr, handle, req, payload_owned, member_id)) {
                /* Owned by the in-flight list now -- completed, and its
                 * payload freed, by vs_tr_drain_one(). */
                payload_owned = NULL;
                handle        = HG_HANDLE_NULL;
            }
            else {
                /* Could not record it: complete it here rather than leak the
                 * handle or lose track of the request. */
                margo_wait(req);
                margo_destroy(handle);
                handle = HG_HANDLE_NULL;
            }
        }
        else {
            margo_destroy(handle);
            handle      = HG_HANDLE_NULL;
            unreachable = 1;
        }
    }
    else
        unreachable = 1;

    if (vs_push_stats_on) {
        uint64_t t1 = vs_now_ns();

        vs_push_stats_n++;
        vs_push_stats_ns += t1 - t0;
        vs_push_stats_bytes += payload_len;
        vs_push_stats_last_end_ns = t1;
    }

    /* A member the drain above reported dead is not worth issuing more to,
     * even though this particular forward was accepted. */
    if (!unreachable && vs_tr_member_failed(tr, member_id))
        unreachable = 1;

    free(payload_owned);
    return unreachable;
} /* end vs_tr_push_one_item() */

int
vs_tr_writer_push_data(vs_tr_t *tr, uint64_t physical_step, const char *path, const void *buf,
                        uint64_t elem_size, uint64_t write_start, uint64_t write_count,
                        const uint8_t *type_enc, uint64_t type_enc_len,
                        const uint8_t *native_dcpl_enc, uint64_t native_dcpl_enc_len, void *native_ctx,
                        const uint8_t *space_enc, uint64_t space_enc_len)
{
    ssg_member_id_t   self_id;
    uint64_t           write_end;

    if (!tr || !path || elem_size == 0)
        return -1;
    if (tr->group_id == SSG_GROUP_ID_INVALID)
        return 0; /* no group -- no subscribers possible, same "proceed" tolerance as broadcast */
    if (SSG_SUCCESS != ssg_get_self_id(tr->mid, &self_id))
        return 0;

    write_end = write_start + write_count;

    /* Fresh unreachable-member set per write, matching the synchronous
     * version's scope exactly: a member given up on here is retried from
     * scratch by the next write. */
    tr->n_failed = 0;

    /* Snapshot every sub_table entry matching this path in one sub_lock
     * acquisition, instead of locking/unlocking once per group member below.
     * The member loop below does real work per iteration -- RPC forwards
     * with a 1s bound, refiltering -- so holding sub_lock across the whole
     * loop (a single lock/unlock bracketing it) would serialize any
     * concurrent subscribe/unsubscribe against that entire duration instead
     * of against a brief table scan. This keeps the same "copy out, release
     * before real work" shape the per-member version already used, just
     * with one lock acquisition instead of up to group_size. */
    {
        typedef struct {
            ssg_member_id_t member_id;
            uint64_t        sel_start;
            uint64_t        sel_count;
            uint8_t        *dcpl_enc;
            uint64_t        dcpl_enc_len;
            uint8_t        *pred_enc;
            uint64_t        pred_enc_len;
            uint8_t        *space_enc;
            uint64_t        space_enc_len;
            uint8_t        *want_type_enc;
            uint64_t        want_type_enc_len;
        } vs_tr_push_sub_snapshot_t;

        vs_tr_push_sub_snapshot_t *snapshot     = NULL;
        size_t                      n_snapshot   = 0, cap_snapshot = 0;
        size_t                      si;

        pthread_mutex_lock(&tr->sub_lock);
        for (si = 0; si < tr->n_sub; si++) {
            vs_tr_push_sub_snapshot_t *grown;

            if (strcmp(tr->sub_table[si].path, path) != 0)
                continue;

            if (n_snapshot == cap_snapshot) {
                size_t new_cap = cap_snapshot ? cap_snapshot * 2 : 8;

                if (NULL == (grown = (vs_tr_push_sub_snapshot_t *)realloc(
                                 snapshot, new_cap * sizeof(*snapshot))))
                    break; /* best-effort, same as the rest of this function --
                            * proceed with whatever was captured so far. */
                snapshot     = grown;
                cap_snapshot = new_cap;
            }

            memset(&snapshot[n_snapshot], 0, sizeof(snapshot[n_snapshot]));
            snapshot[n_snapshot].member_id = tr->sub_table[si].member_id;
            snapshot[n_snapshot].sel_start  = tr->sub_table[si].sel_start;
            snapshot[n_snapshot].sel_count  = tr->sub_table[si].sel_count;
            if (tr->sub_table[si].dcpl_enc_len > 0 &&
                NULL != (snapshot[n_snapshot].dcpl_enc =
                             (uint8_t *)malloc(tr->sub_table[si].dcpl_enc_len))) {
                memcpy(snapshot[n_snapshot].dcpl_enc, tr->sub_table[si].dcpl_enc,
                       tr->sub_table[si].dcpl_enc_len);
                snapshot[n_snapshot].dcpl_enc_len = tr->sub_table[si].dcpl_enc_len;
            }
            if (tr->sub_table[si].pred_enc_len > 0 &&
                NULL != (snapshot[n_snapshot].pred_enc =
                             (uint8_t *)malloc(tr->sub_table[si].pred_enc_len))) {
                memcpy(snapshot[n_snapshot].pred_enc, tr->sub_table[si].pred_enc,
                       tr->sub_table[si].pred_enc_len);
                snapshot[n_snapshot].pred_enc_len = tr->sub_table[si].pred_enc_len;
            }
            if (tr->sub_table[si].space_enc_len > 0 &&
                NULL != (snapshot[n_snapshot].space_enc =
                             (uint8_t *)malloc(tr->sub_table[si].space_enc_len))) {
                memcpy(snapshot[n_snapshot].space_enc, tr->sub_table[si].space_enc,
                       tr->sub_table[si].space_enc_len);
                snapshot[n_snapshot].space_enc_len = tr->sub_table[si].space_enc_len;
            }
            if (tr->sub_table[si].want_type_enc_len > 0 &&
                NULL != (snapshot[n_snapshot].want_type_enc =
                             (uint8_t *)malloc(tr->sub_table[si].want_type_enc_len))) {
                memcpy(snapshot[n_snapshot].want_type_enc, tr->sub_table[si].want_type_enc,
                       tr->sub_table[si].want_type_enc_len);
                snapshot[n_snapshot].want_type_enc_len = tr->sub_table[si].want_type_enc_len;
            }
            n_snapshot++;
        }
        pthread_mutex_unlock(&tr->sub_lock);

        /* Iterate the subscribers themselves, NOT the group's rank space.
         *
         * This used to walk ranks 0..ssg_get_group_size()-1 and look each
         * rank's member up, which is a time-of-check/time-of-use bug: the
         * loop body does blocking network I/O, and a subscriber that
         * receives its last push and closes leaves the SSG group while the
         * loop is still running. The group then shrinks, the top ranks stop
         * existing, and ssg_get_group_member_id_from_rank() correctly fails
         * for them -- silently dropping the pushes of subscribers that had
         * nothing to do with the departure. Reproduced by
         * test/b_push_fanout.c at 8 subscribers: ranks 7 and 8 skipped at
         * the final step, two readers left waiting for data that was never
         * sent, every time. SSG was answering honestly; indexing a mutable
         * rank space across blocking calls was the mistake.
         *
         * The snapshot already holds exactly the members subscribed to this
         * path, so a member_id is all that is needed -- it stays valid
         * whether or not that member is still present, and
         * ssg_get_group_member_addr() below is what reports a departure, per
         * member, without touching anyone else's push. This also drops the
         * O(group_size * n_snapshot) match loop, since subscribers to one
         * path are typically far fewer than group members. */
        for (si = 0; si < n_snapshot; si++) {
        ssg_member_id_t member_id;
        hg_addr_t         addr;
        hg_handle_t        handle;
        uint64_t           sub_start = 0, sub_end = 0, overlap_start, overlap_end, overlap_count;
        uint8_t           *sub_dcpl_enc = NULL;
        uint64_t           sub_dcpl_enc_len = 0;
        uint8_t           *sub_pred_enc = NULL;
        uint64_t           sub_pred_enc_len = 0;
        uint8_t           *sub_space_enc = NULL;
        uint64_t           sub_space_enc_len = 0;
        uint8_t           *sub_want_type_enc = NULL;
        uint64_t           sub_want_type_enc_len = 0;
        /* This subscriber's view of the data: the dataset's own buffer/type by
         * default, or a narrowed conversion of the overlap. s_base is the
         * element index s_buf's first element corresponds to -- write_start
         * normally, overlap_start once converted. s_conv_buf is the owned
         * allocation, NULL when no conversion happened. */
        const void        *s_buf = buf;
        uint64_t           s_base = write_start;
        uint64_t           s_elem_size = elem_size;
        const uint8_t     *s_type_enc = type_enc;
        uint64_t           s_type_enc_len = type_enc_len;
        void              *s_conv_buf = NULL;
        vs_tr_run_t        sel_runs[VS_TR_MAX_PRED_RUNS];
        vs_tr_run_t        runs[VS_TR_MAX_PRED_RUNS];
        int                n_sel = 1, n_runs = 1, sr, r;
        int                member_unreachable = 0;

        member_id = snapshot[si].member_id;

        /* A writer does not subscribe to itself, but the table is populated
         * from the wire, so keep the guard. */
        if (member_id == self_id)
            continue;

        sub_start = snapshot[si].sel_start;
        /* UINT64_MAX sel_count means "whole object" -- keep sub_end at the
         * UINT64_MAX sentinel too rather than computing sel_start +
         * sel_count, which would overflow. */
        sub_end = (snapshot[si].sel_count == UINT64_MAX) ? UINT64_MAX : sub_start + snapshot[si].sel_count;
        /* Take ownership out of the snapshot rather than copying again --
         * each slot is visited exactly once now, so nothing else can claim
         * these. */
        sub_dcpl_enc      = snapshot[si].dcpl_enc;
        sub_dcpl_enc_len  = snapshot[si].dcpl_enc_len;
        sub_pred_enc      = snapshot[si].pred_enc;
        sub_pred_enc_len  = snapshot[si].pred_enc_len;
        sub_space_enc     = snapshot[si].space_enc;
        sub_space_enc_len = snapshot[si].space_enc_len;
        sub_want_type_enc     = snapshot[si].want_type_enc;
        sub_want_type_enc_len = snapshot[si].want_type_enc_len;
        snapshot[si].dcpl_enc = snapshot[si].pred_enc = snapshot[si].space_enc = NULL;
        snapshot[si].want_type_enc = NULL;

        /* M8.5: only the overlap between what this member asked for and
         * what was just written -- never the whole write, and never
         * anything the member did not ask for. A subscriber whose range
         * does not touch this write at all gets nothing this call, not an
         * empty push (see this function's header comment). */
        overlap_start = (sub_start > write_start) ? sub_start : write_start;
        overlap_end   = (sub_end < write_end) ? sub_end : write_end;
        if (overlap_start >= overlap_end) {
            free(sub_dcpl_enc);
            free(sub_pred_enc);
            free(sub_space_enc);
            free(sub_want_type_enc);
            continue;
        }
        overlap_count = overlap_end - overlap_start;

        /* Two refinements of the overlap, applied in order, each optional
         * and each falling back to "send it all" when it cannot be applied.
         *
         * First the subscriber's actual selection: overlap_start/count is
         * only the flat span *containing* what was asked for, so a
         * non-contiguous selection (a column of a 2-D dataset) would
         * otherwise be served its whole bounding span.
         *
         * Then, within each of those runs, the subscriber's predicate.
         * Nesting rather than combining keeps each callback's contract
         * simple -- "given this flat range, which parts do you want" -- and
         * means either can decline independently. */
        sel_runs[0].start = 0;
        sel_runs[0].count = overlap_count;
        if (sub_space_enc_len > 0 && tr->selection_fn) {
            int n = tr->selection_fn(sub_space_enc, sub_space_enc_len, space_enc, space_enc_len,
                                       overlap_start, overlap_count, sel_runs, VS_TR_MAX_PRED_RUNS);

            if (n >= 0)
                n_sel = n;
            else {
                /* Declined -- restore, since the callback may have written
                 * into sel_runs before giving up. */
                sel_runs[0].start = 0;
                sel_runs[0].count = overlap_count;
            }
        }
        free(sub_space_enc);
        sub_space_enc = NULL;

        if (n_sel == 0) {
            /* The selection does not touch this write at all. */
            free(sub_dcpl_enc);
            free(sub_pred_enc);
            free(sub_want_type_enc);
            continue;
        }

        if (SSG_SUCCESS != ssg_get_group_member_addr(tr->group_id, member_id, &addr)) {
            free(sub_dcpl_enc);
            free(sub_pred_enc);
            free(sub_want_type_enc);
            continue;
        }

        /* Datatype narrowing: deliver this subscriber the overlap AS the type
         * it asked for, rather than the dataset's own -- a viz client taking
         * 4-byte float from a double field halves the payload before any
         * filter runs.
         *
         * Converted once per subscriber over the whole overlap, not per run,
         * and the converted buffer is re-based: its first element is
         * overlap_start rather than write_start, which is why every indexing
         * expression below goes through s_base rather than write_start.
         *
         * Declining leaves every s_* local pointing at the dataset's own
         * representation, i.e. exactly the pre-narrowing behavior -- the same
         * decline-is-safe rule the refilter and predicate paths follow. */
        if (sub_want_type_enc_len > 0 && tr->convert_fn) {
            void    *conv     = NULL;
            uint64_t conv_esz = 0;

            if (0 == tr->convert_fn((const uint8_t *)buf + (overlap_start - write_start) * elem_size,
                                      overlap_count, type_enc, type_enc_len, sub_want_type_enc,
                                      sub_want_type_enc_len, &conv, &conv_esz) &&
                conv_esz > 0) {
                s_conv_buf     = conv;
                s_buf          = conv;
                s_base         = overlap_start;
                s_elem_size    = conv_esz;
                s_type_enc     = sub_want_type_enc;
                s_type_enc_len = sub_want_type_enc_len;
            }
        }

        for (sr = 0; sr < n_sel; sr++) {
            uint64_t sel_start_abs = overlap_start + sel_runs[sr].start;
            uint64_t sel_count_abs = sel_runs[sr].count;

        /* M9 predicate pushdown: narrow this run to the elements the
         * subscriber's predicate actually matches. With no predicate (or
         * none the callback can evaluate) that is the whole run, i.e.
         * exactly M8.5's behavior. */
        runs[0].start = 0;
        runs[0].count = sel_count_abs;
        n_runs        = 1;
        if (sub_pred_enc_len > 0 && tr->predicate_fn) {
            int n = tr->predicate_fn((const uint8_t *)s_buf + (sel_start_abs - s_base) * s_elem_size,
                                       s_elem_size, sel_count_abs, sub_pred_enc, sub_pred_enc_len,
                                       s_type_enc, s_type_enc_len, runs, VS_TR_MAX_PRED_RUNS);

            if (n >= 0)
                n_runs = n;
            else {
                runs[0].start = 0;
                runs[0].count = sel_count_abs;
            }
        }

        /* Nothing matched: this subscriber gets no RPC at all for this run.
         * The whole point of the predicate -- every other reduction in this
         * protocol still sends something. */
        /* Same bounded, best-effort forward vs_tr_push_one_item() uses for
         * every call below -- a stalled or gone subscriber must not stall
         * the writer.
         *
         * The per-RPC timeout bounds one push, not one write: a write goes
         * out as one RPC per contiguous run (up to VS_TR_MAX_PRED_RUNS, and
         * again per selection run, and now again per chunk when a
         * subscriber's DCPL asks for one smaller than the run), so nothing
         * here structurally stopped an unreachable member from being
         * charged the full timeout once per run. Giving up on that member
         * after its first timeout makes the per-write bound explicit -- one
         * timeout, not one per run/chunk.
         *
         * Measured, and the measurement is worth recording because it
         * contradicts the obvious assumption: it did NOT cost a timeout per
         * run beforehand. A 128-run write to an unreachable member took
         * ~1.00s both with and without this guard, because Mercury itself
         * stops retrying an address that has already timed out, so every
         * later forward fails immediately. This guard therefore changes no
         * number measured here.
         *
         * Kept anyway, as a bound this module owns rather than one it
         * borrows. That fast-fail is Mercury's internal behavior observed on
         * one provider (na+sm) in one failure mode, not a documented
         * guarantee across providers or versions, and the cost if it ever
         * differs is a multi-minute stall inside H5Fend_step() -- a poor
         * thing to discover in production. For scale: a healthy push costs
         * ~20us, so one timeout already costs ~50,000x the work it guards.
         *
         * The next write retries the member from scratch, and SSG's failure
         * detector is what eventually removes it from the group, so nothing
         * a later push would have delivered is lost. */
        for (r = 0; r < n_runs; r++) {
            uint64_t run_start = sel_start_abs + runs[r].start;
            uint64_t run_count = runs[r].count;
            uint64_t chunk_elems = 0;

            /* HG_ENCODE only reads this -- see hg_proc_vs_blob_t(). Slicing
             * by byte offset into buf: buf's own first element is
             * write_start, so this run begins (run_start - write_start)
             * elements in. */
            const void *run_ptr = (const uint8_t *)s_buf + (run_start - s_base) * s_elem_size;

            if (sub_dcpl_enc_len > 0 && tr->refilter_shape_fn)
                chunk_elems = tr->refilter_shape_fn(sub_dcpl_enc, sub_dcpl_enc_len, run_count, s_elem_size);

            if (chunk_elems > 0 && chunk_elems < run_count) {
                /* M8.5 follow-up: this subscriber's DCPL asked for a real
                 * chunk shape smaller than the whole run -- honor it by
                 * pushing each chunk as its own RPC rather than forcing one
                 * chunk spanning the run (see vs_tr_refilter_shape_fn's
                 * comment in tr_mercury.h for why the receiving side needs
                 * no change for this). */
                uint64_t off = 0;

                while (off < run_count) {
                    uint64_t    this_count = (run_count - off < chunk_elems) ? (run_count - off) : chunk_elems;
                    const void *sub_ptr    = (const uint8_t *)run_ptr + off * s_elem_size;
                    void       *cb_buf     = NULL;
                    uint64_t    cb_len     = 0;
                    uint32_t    cb_mask    = 0;

                    if (tr->refilter_fn &&
                        0 == tr->refilter_fn(sub_ptr, s_elem_size, this_count, sub_dcpl_enc,
                                               sub_dcpl_enc_len, s_type_enc, s_type_enc_len,
                                               s_conv_buf ? NULL : native_dcpl_enc,
                                               s_conv_buf ? 0 : native_dcpl_enc_len, native_ctx, &cb_buf,
                                               &cb_len, &cb_mask))
                        member_unreachable = vs_tr_push_one_item(
                            tr, addr, member_id, physical_step, path, run_start + off, this_count, cb_buf,
                            cb_len, sub_dcpl_enc, sub_dcpl_enc_len, s_type_enc, s_type_enc_len, cb_mask,
                            cb_buf);
                    else
                        /* Refilter declined/failed for this slice: fall back
                         * to raw bytes for just this slice, the same
                         * "decline, don't fail" rule the whole-run path
                         * already follows. */
                        member_unreachable = vs_tr_push_one_item(
                            tr, addr, member_id, physical_step, path, run_start + off, this_count, sub_ptr,
                            this_count * s_elem_size, NULL, 0, s_type_enc, s_type_enc_len, 0, NULL);

                    if (member_unreachable)
                        break;
                    off += this_count;
                }
            }
            else {
                /* Unchanged: one push spanning the whole run. */
                void    *filtered_buf = NULL;
                uint64_t filtered_len = 0;
                uint32_t filter_mask  = 0;
                int      did_refilter = 0;

                /* M8.5 precision: this subscriber asked for a specific
                 * filter pipeline -- run the slice through it via the
                 * registered callback (see vs_tr_refilter_fn's comment)
                 * instead of sending raw bytes. A declined/failed refilter
                 * (no callback registered, or the callback itself fails) is
                 * not an error -- fall back to the exact raw-bytes behavior
                 * this subscriber would have gotten without a dcpl_enc at
                 * all.
                 *
                 * M9: native_ctx describes the whole write, so a predicate
                 * that split it declines the zero-copy fast path
                 * automatically -- that path requires the push to cover the
                 * dataset exactly (ctx->total_elems != count), which a
                 * partial run never does. Correct by construction rather
                 * than by a check here. */
                if (sub_dcpl_enc_len > 0 && tr->refilter_fn &&
                    0 == tr->refilter_fn(run_ptr, s_elem_size, run_count, sub_dcpl_enc, sub_dcpl_enc_len,
                                           s_type_enc, s_type_enc_len,
                                           s_conv_buf ? NULL : native_dcpl_enc,
                                           s_conv_buf ? 0 : native_dcpl_enc_len, native_ctx, &filtered_buf,
                                           &filtered_len, &filter_mask))
                    did_refilter = 1;

                if (did_refilter)
                    member_unreachable = vs_tr_push_one_item(
                        tr, addr, member_id, physical_step, path, run_start, run_count, filtered_buf,
                        filtered_len, sub_dcpl_enc, sub_dcpl_enc_len, s_type_enc, s_type_enc_len,
                        filter_mask, filtered_buf);
                else
                    member_unreachable =
                        vs_tr_push_one_item(tr, addr, member_id, physical_step, path, run_start, run_count,
                                             run_ptr, run_count * s_elem_size, NULL, 0, s_type_enc,
                                             s_type_enc_len, 0, NULL);
            }

            if (member_unreachable)
                break;
        }
        if (member_unreachable)
            break;
        } /* end per-selection-run loop */
        margo_addr_free(tr->mid, addr);
        free(sub_dcpl_enc);
        free(sub_pred_enc);
        free(sub_want_type_enc);
        free(s_conv_buf);
        }

        /* Whatever wasn't matched to a member above (e.g. a subscriber SSG
         * has since dropped from the group) still owns its own copies. */
        {
            size_t k;

            for (k = 0; k < n_snapshot; k++) {
                free(snapshot[k].dcpl_enc);
                free(snapshot[k].pred_enc);
                free(snapshot[k].space_enc);
                free(snapshot[k].want_type_enc);
            }
            free(snapshot);
        }
    }

    return 0;
}

int
vs_tr_reader_wait_data(vs_tr_t *tr, uint64_t timeout_ms, uint64_t *physical_step, char **out_path,
                         void **out_buf, uint64_t *out_size, uint64_t *out_elem_start,
                         uint64_t *out_elem_count, uint8_t **out_dcpl_enc, uint64_t *out_dcpl_enc_len,
                         uint8_t **out_type_enc, uint64_t *out_type_enc_len, uint32_t *out_filter_mask)
{
    struct timespec deadline;
    int             ret = -1;

    if (!tr)
        return -1;

    vs_tr_compute_deadline(timeout_ms, &deadline);

    pthread_mutex_lock(&tr->data_lock);
    while (tr->n_data == 0 && !tr->stopped) {
        if (ETIMEDOUT == pthread_cond_timedwait(&tr->data_cond, &tr->data_lock, &deadline))
            break;
    }

    if (tr->n_data > 0) {
        if (physical_step)
            *physical_step = tr->data_queue[0].physical_step;
        if (out_path)
            *out_path = tr->data_queue[0].path;
        else
            free(tr->data_queue[0].path);
        if (out_buf)
            *out_buf = tr->data_queue[0].buf;
        else
            free(tr->data_queue[0].buf);
        if (out_size)
            *out_size = tr->data_queue[0].size;
        if (out_elem_start)
            *out_elem_start = tr->data_queue[0].elem_start;
        if (out_elem_count)
            *out_elem_count = tr->data_queue[0].elem_count;
        if (out_dcpl_enc)
            *out_dcpl_enc = tr->data_queue[0].dcpl_enc;
        else
            free(tr->data_queue[0].dcpl_enc);
        if (out_dcpl_enc_len)
            *out_dcpl_enc_len = tr->data_queue[0].dcpl_enc_len;
        if (out_type_enc)
            *out_type_enc = tr->data_queue[0].type_enc;
        else
            free(tr->data_queue[0].type_enc);
        if (out_type_enc_len)
            *out_type_enc_len = tr->data_queue[0].type_enc_len;
        if (out_filter_mask)
            *out_filter_mask = tr->data_queue[0].filter_mask;
        memmove(&tr->data_queue[0], &tr->data_queue[1], (tr->n_data - 1) * sizeof(*tr->data_queue));
        tr->n_data--;
        ret = 0;
    }
    pthread_mutex_unlock(&tr->data_lock);

    return ret;
}
