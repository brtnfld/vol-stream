/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Purpose: M4's transport module (Mercury for RPC, Margo for the progress
 *          engine, Argobots underneath both) plus M5's group membership on
 *          top of it (SSG). Per dev-plan.md's "Argobots and Margo own the
 *          progress engine from M4" decision.
 *
 *          This is the control plane only: a writer that just committed a
 *          step notifies every group member with a "step_ready" RPC. Step
 *          data itself still moves through the replicated /step/<n>/ file
 *          (M2/M3's mechanism, untouched) -- Mercury does not carry payload
 *          bytes until M8's subscription protocol makes that the
 *          differentiator worth the wire-format work.
 *
 *          M5: what used to be a hand-rolled "attach" RPC and a manually
 *          tracked reader-address array is now an SSG group. The writer
 *          creates a group containing just itself and stores the group id
 *          to a sidecar file (vs_tr_writer_create_group()); a reader loads
 *          it and joins (vs_tr_reader_join_group()) -- SSG's own join
 *          protocol replaces the old attach RPC. SSG's SWIM failure
 *          detector means a reader that dies is dropped from the group
 *          without vol-stream having to implement its own liveness
 *          tracking: vs_tr_writer_broadcast_step_ready() queries the live
 *          group view at send time, so a dead or departed member is simply
 *          absent from it. A late joiner's "coherent view" comes from
 *          vs_tr_reader_get_current_step(), which vs_tr_reader_join_group()
 *          calls automatically and seeds into the same pending-notification
 *          queue vs_tr_reader_wait_step_ready() already drains, so the
 *          first wait after joining returns immediately with the current
 *          step rather than blocking for the next write.
 *
 *          Deliberately independent of HDF5 headers/types (plain int
 *          returns, not herr_t) so this module has no HDF5 dependency of
 *          its own.
 */

#ifndef VOL_STREAM_TR_MERCURY_H
#define VOL_STREAM_TR_MERCURY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vs_tr_t vs_tr_t;

/* Starts Margo and SSG on na_str (e.g. "na+sm", "ofi+tcp"). Both writer and
 * reader run in server mode: a writer must receive RPCs (get_current_step,
 * plus SSG's own group protocol), and a reader must receive step_ready
 * pushes (and SSG group protocol too), so there is no lighter client-only
 * role here. Returns NULL on failure. */
vs_tr_t *vs_tr_start(const char *na_str);

/* Stops SSG and Margo and releases every resource vs_tr_start() allocated.
 * If this process still belongs to a group (writer or reader, whichever is
 * still set from create/join below), leaves or destroys it first. Wakes any
 * thread blocked in vs_tr_reader_wait_step_ready(). */
void vs_tr_stop(vs_tr_t *tr);

/* Writer side: create a new single-member SSG group (this process is the
 * sole initial member) and store its id to group_file, which a reader loads
 * via vs_tr_reader_join_group(). Returns 0 on success, -1 on failure --
 * not fatal to the caller: a writer with no working group simply has no
 * transport-level readers, which vs_tr_writer_broadcast_step_ready()
 * tolerates as a no-op. */
int vs_tr_writer_start_group(vs_tr_t *tr, const char *group_file);

/* Writer side: push a step_ready notification to every current member of
 * the group (querying live membership, so a departed or failure-detected
 * member is simply skipped -- "a reader leaving mid-stream does not stall
 * the writer"). Also caches physical_step/wall_time_ns as the answer to a
 * later vs_tr_reader_get_current_step() query. Not an error for there to be
 * no other members -- "a writer starts with no readers and proceeds" is
 * exercised by never having called vs_tr_reader_join_group() anywhere.
 * Returns 0 on success (even if an individual member's RPC failed), -1 only
 * on a local error. */
int vs_tr_writer_broadcast_step_ready(vs_tr_t *tr, uint64_t physical_step, uint64_t wall_time_ns);

/* Reader side: load the group id from group_file (bootstrap -- the caller
 * retries if the writer has not published it yet, same as M4) and join the
 * group. On success, also queries the writer for its current committed step
 * and seeds it into the pending-notification queue if there is one -- a
 * late joiner's "coherent view". Returns 0 on success, -1 on failure. */
int vs_tr_reader_join_group(vs_tr_t *tr, const char *group_file);

/* Reader side: leave the group cleanly, so the writer's membership view
 * updates immediately (SSG_MEMBER_LEFT) rather than waiting for SWIM to
 * time the member out (SSG_MEMBER_DIED). Call before vs_tr_stop() when the
 * reader is closing normally; not required (vs_tr_stop() itself leaves any
 * group still joined) but faster and cleaner for the writer's view when it
 * happens. Returns 0 on success, -1 on failure. */
int vs_tr_reader_leave_group(vs_tr_t *tr);

/* Reader side: ask the writer (group rank 0) for the latest step it has
 * committed. Returns 0 with *physical_step / *wall_time_ns filled in, or -1
 * if the writer has not committed any step yet or is unreachable. Normally
 * called automatically by vs_tr_reader_join_group(); exposed separately in
 * case a caller wants to re-query later. */
int vs_tr_reader_get_current_step(vs_tr_t *tr, uint64_t *physical_step, uint64_t *wall_time_ns);

/* Reader side: block up to timeout_ms for a step_ready notification (one
 * may already be queued -- from before this call, or seeded by
 * vs_tr_reader_join_group()'s late-joiner query), filling *physical_step
 * and *wall_time_ns. timeout_ms == 0 polls without blocking. Returns 0 on
 * success, -1 on timeout or if vs_tr_stop() was called while waiting and no
 * notification remains queued. */
int vs_tr_reader_wait_step_ready(vs_tr_t *tr, uint64_t timeout_ms, uint64_t *physical_step,
                                  uint64_t *wall_time_ns);

#ifdef __cplusplus
}
#endif

#endif /* VOL_STREAM_TR_MERCURY_H */
