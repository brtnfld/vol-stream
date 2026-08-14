/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Purpose: Implementation of tr_mercury.h. See that header for the design
 *          note on scope and the M5 SSG integration.
 *
 *          Both RPCs are answered on a plain Argobots handler pool that
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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
 * raw-bytes behavior. */
MERCURY_GEN_PROC(vs_subscribe_in_t, ((uint64_t)(member_id))((hg_string_t)(path))((uint64_t)(sel_start))(
                                          (uint64_t)(sel_count))((vs_blob_t)(dcpl_enc)))
MERCURY_GEN_PROC(vs_subscribe_out_t, ((int32_t)(status)))
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

typedef struct vs_tr_pending_t {
    uint64_t physical_step;
    uint64_t wall_time_ns;
} vs_tr_pending_t;

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
 * none. */
typedef struct vs_tr_sub_entry_t {
    ssg_member_id_t member_id;
    char            *path;
    uint64_t          sel_start;
    uint64_t          sel_count;
    uint8_t          *dcpl_enc;
    uint64_t          dcpl_enc_len;
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

    /* M8, writer side: subscription table (see vs_tr_sub_entry_t). Same
     * "grown lazily, entries for departed members simply ignored rather than
     * pruned" approach as lag_table above. */
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
};

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

/* M5: fires on SSG_MEMBER_JOINED/LEFT/DIED for any group this process
 * belongs to. Both ssg_group_create() and ssg_group_join() require one;
 * vs_tr_writer_broadcast_step_ready() does not need to be told about a
 * departure itself, since it queries live group membership at send time,
 * so a departed or failure-detected member is simply absent from the next
 * broadcast without this callback's help. */
static void
vs_membership_cb(void *group_data, ssg_member_id_t member_id, ssg_member_update_type_t update_type)
{
    (void)group_data;
    (void)member_id;
#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM SSG membership update: member %lu, type %d\n", (unsigned long)member_id,
           (int)update_type);
#endif
} /* end vs_membership_cb() */

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
 * same object). */
static void
vs_subscribe_ult(hg_handle_t handle)
{
    margo_instance_id    mid = margo_hg_handle_get_instance(handle);
    hg_id_t                id = margo_get_info(handle)->id;
    vs_tr_t               *tr = (vs_tr_t *)margo_registered_data(mid, id);
    vs_subscribe_in_t    in;
    vs_subscribe_out_t   out;

    out.status = -1;

    if (tr && HG_SUCCESS == margo_get_input(handle, &in) && in.path) {
        size_t i;
        int    found = 0;

        pthread_mutex_lock(&tr->sub_lock);

        for (i = 0; i < tr->n_sub; i++)
            if (tr->sub_table[i].member_id == (ssg_member_id_t)in.member_id &&
                strcmp(tr->sub_table[i].path, in.path) == 0) {
                uint8_t *dcpl_copy;
                uint64_t dcpl_copy_len;

                if (vs_blob_dup(&in.dcpl_enc, &dcpl_copy, &dcpl_copy_len) == 0) {
                    free(tr->sub_table[i].dcpl_enc);
                    tr->sub_table[i].dcpl_enc     = dcpl_copy;
                    tr->sub_table[i].dcpl_enc_len = dcpl_copy_len;
                }
                tr->sub_table[i].sel_start = in.sel_start;
                tr->sub_table[i].sel_count = in.sel_count;
                found = 1;
                break;
            }

        if (!found) {
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

                if (path_copy && vs_blob_dup(&in.dcpl_enc, &dcpl_copy, &dcpl_copy_len) == 0) {
                    tr->sub_table[tr->n_sub].member_id     = (ssg_member_id_t)in.member_id;
                    tr->sub_table[tr->n_sub].path          = path_copy;
                    tr->sub_table[tr->n_sub].sel_start     = in.sel_start;
                    tr->sub_table[tr->n_sub].sel_count     = in.sel_count;
                    tr->sub_table[tr->n_sub].dcpl_enc      = dcpl_copy;
                    tr->sub_table[tr->n_sub].dcpl_enc_len  = dcpl_copy_len;
                    tr->n_sub++;
                }
                else
                    free(path_copy);
            }
        }

        pthread_mutex_unlock(&tr->sub_lock);

        out.status = 0;
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

    margo_register_data(tr->mid, tr->step_ready_rpc_id, tr, NULL);
    margo_register_data(tr->mid, tr->get_current_step_rpc_id, tr, NULL);
    margo_register_data(tr->mid, tr->reader_ack_rpc_id, tr, NULL);
    margo_register_data(tr->mid, tr->subscribe_rpc_id, tr, NULL);
    margo_register_data(tr->mid, tr->data_push_rpc_id, tr, NULL);

    return tr;
}

void
vs_tr_set_refilter_cb(vs_tr_t *tr, vs_tr_refilter_fn fn)
{
    if (tr)
        tr->refilter_fn = fn;
}

void
vs_tr_stop(vs_tr_t *tr)
{
    if (!tr)
        return;

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
    free(tr->pending);
    free(tr->lag_table);
    {
        size_t i;

        for (i = 0; i < tr->n_sub; i++) {
            free(tr->sub_table[i].path);
            free(tr->sub_table[i].dcpl_enc);
        }
        free(tr->sub_table);
        for (i = 0; i < tr->n_data; i++) {
            free(tr->data_queue[i].path);
            free(tr->data_queue[i].buf);
            free(tr->data_queue[i].dcpl_enc);
            free(tr->data_queue[i].type_enc);
        }
        free(tr->data_queue);
    }
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
    int              group_size, i;
    vs_get_current_step_in_t in;
    int              ret = -1;

    if (!tr || tr->group_id == SSG_GROUP_ID_INVALID)
        return -1;
    if (SSG_SUCCESS != ssg_get_self_id(tr->mid, &self_id))
        return -1;
    if (SSG_SUCCESS != ssg_get_group_size(tr->group_id, &group_size))
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
    for (i = 0; i < group_size && ret != 0; i++) {
        ssg_member_id_t member_id;
        hg_addr_t        addr;
        hg_handle_t       handle;

        if (SSG_SUCCESS != ssg_get_group_member_id_from_rank(tr->group_id, i, &member_id))
            continue;
        if (member_id == self_id)
            continue;
        if (SSG_SUCCESS != ssg_get_group_member_addr(tr->group_id, member_id, &addr))
            continue;

        if (HG_SUCCESS == margo_create(tr->mid, addr, tr->get_current_step_rpc_id, &handle)) {
            if (HG_SUCCESS == margo_forward(handle, &in)) {
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

    return ret;
}

int
vs_tr_writer_broadcast_step_ready(vs_tr_t *tr, uint64_t physical_step, uint64_t wall_time_ns)
{
    ssg_member_id_t     self_id;
    int                 group_size, i;
    vs_step_ready_in_t  in;

    if (!tr)
        return -1;

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
    if (SSG_SUCCESS != ssg_get_group_size(tr->group_id, &group_size))
        return 0;

    in.physical_step = physical_step;
    in.wall_time_ns   = wall_time_ns;

    for (i = 0; i < group_size; i++) {
        ssg_member_id_t member_id;
        hg_addr_t        addr;
        hg_handle_t       handle;

        if (SSG_SUCCESS != ssg_get_group_member_id_from_rank(tr->group_id, i, &member_id))
            continue;
        if (member_id == self_id)
            continue;
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

    return 0;
}

int
vs_tr_reader_wait_step_ready(vs_tr_t *tr, uint64_t timeout_ms, uint64_t *physical_step,
                              uint64_t *wall_time_ns)
{
    struct timespec deadline;
    int             ret = -1;

    if (!tr)
        return -1;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

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
        int group_size, i;

        if (SSG_SUCCESS != ssg_get_group_size(tr->group_id, &group_size))
            return ret;

        for (i = 0; i < group_size && ret != 0; i++) {
            ssg_member_id_t member_id;
            hg_addr_t         addr;
            hg_handle_t        handle;

            if (SSG_SUCCESS != ssg_get_group_member_id_from_rank(tr->group_id, i, &member_id))
                continue;
            if (member_id == self_id)
                continue;
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
    }

    return ret;
}

int
vs_tr_writer_min_acked_step(vs_tr_t *tr, uint64_t *min_acked_step)
{
    int    group_size = 0;
    size_t i;
    int    found = 0;
    uint64_t min = 0;

    if (!tr)
        return 0;

    /* No group at all -- nothing to be behind. */
    if (tr->group_id != SSG_GROUP_ID_INVALID)
        ssg_get_group_size(tr->group_id, &group_size);

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

    (void)group_size;

    if (found && min_acked_step)
        *min_acked_step = min;

    return found;
}

int
vs_tr_reader_subscribe(vs_tr_t *tr, const char *path, uint64_t sel_start, uint64_t sel_count,
                        const uint8_t *dcpl_enc, uint64_t dcpl_enc_len)
{
    ssg_member_id_t     self_id;
    vs_subscribe_in_t in;
    int                  ret = -1;

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

    /* Same target-the-cached-writer-then-fall-back-to-probing approach as
     * vs_tr_reader_ack_step() -- see that function's comment. */
    if (tr->has_writer_member_id) {
        hg_addr_t  addr;
        hg_handle_t handle;

        if (SSG_SUCCESS == ssg_get_group_member_addr(tr->group_id, tr->writer_member_id, &addr)) {
            if (HG_SUCCESS == margo_create(tr->mid, addr, tr->subscribe_rpc_id, &handle)) {
                if (HG_SUCCESS == margo_forward_timed(handle, &in, 1000.0)) {
                    vs_subscribe_out_t out;

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

    if (ret != 0) {
        int group_size, i;

        if (SSG_SUCCESS != ssg_get_group_size(tr->group_id, &group_size))
            return ret;

        for (i = 0; i < group_size && ret != 0; i++) {
            ssg_member_id_t member_id;
            hg_addr_t         addr;
            hg_handle_t        handle;

            if (SSG_SUCCESS != ssg_get_group_member_id_from_rank(tr->group_id, i, &member_id))
                continue;
            if (member_id == self_id)
                continue;
            if (SSG_SUCCESS != ssg_get_group_member_addr(tr->group_id, member_id, &addr))
                continue;

            if (HG_SUCCESS == margo_create(tr->mid, addr, tr->subscribe_rpc_id, &handle)) {
                if (HG_SUCCESS == margo_forward_timed(handle, &in, 1000.0)) {
                    vs_subscribe_out_t out;

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
    }

    return ret;
}

int
vs_tr_writer_push_data(vs_tr_t *tr, uint64_t physical_step, const char *path, const void *buf,
                        uint64_t elem_size, uint64_t write_start, uint64_t write_count,
                        const uint8_t *type_enc, uint64_t type_enc_len)
{
    ssg_member_id_t   self_id;
    int                 group_size, i;
    uint64_t           write_end;

    if (!tr || !path || elem_size == 0)
        return -1;
    if (tr->group_id == SSG_GROUP_ID_INVALID)
        return 0; /* no group -- no subscribers possible, same "proceed" tolerance as broadcast */
    if (SSG_SUCCESS != ssg_get_self_id(tr->mid, &self_id))
        return 0;
    if (SSG_SUCCESS != ssg_get_group_size(tr->group_id, &group_size))
        return 0;

    write_end = write_start + write_count;

    for (i = 0; i < group_size; i++) {
        ssg_member_id_t member_id;
        hg_addr_t         addr;
        hg_handle_t        handle;
        size_t             j;
        int                subscribed = 0;
        uint64_t           sub_start = 0, sub_end = 0, overlap_start, overlap_end, overlap_count;
        uint8_t           *sub_dcpl_enc = NULL;
        uint64_t           sub_dcpl_enc_len = 0;
        void              *filtered_buf = NULL;
        uint64_t           filtered_len = 0;
        uint32_t           filter_mask = 0;
        int                did_refilter = 0;
        vs_data_push_in_t in;

        if (SSG_SUCCESS != ssg_get_group_member_id_from_rank(tr->group_id, i, &member_id))
            continue;
        if (member_id == self_id)
            continue;

        pthread_mutex_lock(&tr->sub_lock);
        for (j = 0; j < tr->n_sub; j++)
            if (tr->sub_table[j].member_id == member_id && strcmp(tr->sub_table[j].path, path) == 0) {
                sub_start = tr->sub_table[j].sel_start;
                /* UINT64_MAX sel_count means "whole object" -- keep sub_end
                 * at the UINT64_MAX sentinel too rather than computing
                 * sel_start + sel_count, which would overflow. */
                sub_end   = (tr->sub_table[j].sel_count == UINT64_MAX) ? UINT64_MAX
                                                                        : sub_start + tr->sub_table[j].sel_count;
                /* Copy, not a borrowed pointer: a concurrent re-subscribe
                 * could free/replace tr->sub_table[j].dcpl_enc after this
                 * function releases sub_lock below, while the refilter call
                 * further down (real work -- building a temp dataset,
                 * writing, reading back) is still using it. */
                if (tr->sub_table[j].dcpl_enc_len > 0 &&
                    NULL != (sub_dcpl_enc = (uint8_t *)malloc(tr->sub_table[j].dcpl_enc_len))) {
                    memcpy(sub_dcpl_enc, tr->sub_table[j].dcpl_enc, tr->sub_table[j].dcpl_enc_len);
                    sub_dcpl_enc_len = tr->sub_table[j].dcpl_enc_len;
                }
                subscribed = 1;
                break;
            }
        pthread_mutex_unlock(&tr->sub_lock);

        if (!subscribed)
            continue;

        /* M8.5: only the overlap between what this member asked for and
         * what was just written -- never the whole write, and never
         * anything the member did not ask for. A subscriber whose range
         * does not touch this write at all gets nothing this call, not an
         * empty push (see this function's header comment). */
        overlap_start = (sub_start > write_start) ? sub_start : write_start;
        overlap_end   = (sub_end < write_end) ? sub_end : write_end;
        if (overlap_start >= overlap_end) {
            free(sub_dcpl_enc);
            continue;
        }
        overlap_count = overlap_end - overlap_start;

        {
            /* HG_ENCODE only reads this -- see hg_proc_vs_blob_t(). Slicing
             * by byte offset into buf: buf's own first element is
             * write_start, so the overlap begins (overlap_start -
             * write_start) elements in. */
            const void *overlap_ptr =
                (const uint8_t *)buf + (overlap_start - write_start) * elem_size;

            /* M8.5 precision: this subscriber asked for a specific filter
             * pipeline -- run the overlap slice through it via the
             * registered callback (see vs_tr_refilter_fn's comment) instead
             * of sending raw bytes. A declined/failed refilter (no callback
             * registered, or the callback itself fails) is not an error --
             * fall back to the exact raw-bytes behavior this subscriber
             * would have gotten without a dcpl_enc at all. */
            if (sub_dcpl_enc_len > 0 && tr->refilter_fn &&
                0 == tr->refilter_fn(overlap_ptr, elem_size, overlap_count, sub_dcpl_enc, sub_dcpl_enc_len,
                                       type_enc, type_enc_len, &filtered_buf, &filtered_len, &filter_mask))
                did_refilter = 1;

            in.physical_step = physical_step;
            in.path             = (hg_string_t)path;
            in.elem_start       = overlap_start;
            in.elem_count       = overlap_count;
            in.filter_mask      = filter_mask;

            if (did_refilter) {
                in.payload.buf     = filtered_buf;
                in.payload.size    = filtered_len;
                in.dcpl_enc.buf    = sub_dcpl_enc;
                in.dcpl_enc.size   = sub_dcpl_enc_len;
                in.type_enc.buf    = (void *)(uintptr_t)type_enc;
                in.type_enc.size   = type_enc_len;
            }
            else {
                in.payload.buf   = (void *)(uintptr_t)overlap_ptr;
                in.payload.size  = overlap_count * elem_size;
                in.dcpl_enc.buf  = NULL;
                in.dcpl_enc.size = 0;
                in.type_enc.buf  = NULL;
                in.type_enc.size = 0;
            }
        }

        if (SSG_SUCCESS != ssg_get_group_member_addr(tr->group_id, member_id, &addr)) {
            free(sub_dcpl_enc);
            free(filtered_buf);
            continue;
        }

        /* Same bounded, best-effort forward as vs_tr_writer_broadcast_step_ready() --
         * a stalled or gone subscriber must not stall the writer. */
        if (HG_SUCCESS == margo_create(tr->mid, addr, tr->data_push_rpc_id, &handle)) {
            if (HG_SUCCESS == margo_forward_timed(handle, &in, 1000.0)) {
                vs_data_push_out_t out;

                if (HG_SUCCESS == margo_get_output(handle, &out))
                    margo_free_output(handle, &out);
            }
            margo_destroy(handle);
        }
        margo_addr_free(tr->mid, addr);
        free(sub_dcpl_enc);
        free(filtered_buf);
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

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

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
