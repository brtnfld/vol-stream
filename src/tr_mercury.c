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

typedef struct vs_tr_pending_t {
    uint64_t physical_step;
    uint64_t wall_time_ns;
} vs_tr_pending_t;

struct vs_tr_t {
    margo_instance_id mid;
    hg_id_t            step_ready_rpc_id;
    hg_id_t            get_current_step_rpc_id;

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

    tr->step_ready_rpc_id =
        MARGO_REGISTER(tr->mid, "vol-stream:step_ready", vs_step_ready_in_t, vs_step_ready_out_t,
                        vs_step_ready_ult);
    tr->get_current_step_rpc_id =
        MARGO_REGISTER(tr->mid, "vol-stream:get_current_step", vs_get_current_step_in_t,
                        vs_get_current_step_out_t, vs_get_current_step_ult);

    margo_register_data(tr->mid, tr->step_ready_rpc_id, tr, NULL);
    margo_register_data(tr->mid, tr->get_current_step_rpc_id, tr, NULL);

    return tr;
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
    free(tr->pending);
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
