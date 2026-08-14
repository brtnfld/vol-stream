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
 *
 *          M7: a reader-> writer step_ack RPC, so the writer can, for the
 *          first time, learn how far behind an attached reader is (the
 *          missing half of the M4-M5 protocol -- readers previously only
 *          ever received pushes, never reported consumption back). Sent
 *          automatically by H5VLstream.c after a reader's sequential
 *          H5Fbegin_step() advance; a reader that only ever jumps via
 *          H5Fbegin_logical_step() (a monitoring/latest-only reader) never
 *          sends one and is therefore never counted as "behind" -- see
 *          vs_tr_writer_min_acked_step()'s comment. vs_tr_get_mid() exists
 *          so H5VLstream.c can hand the same margo instance to tr_bake.c's
 *          embedded BAKE provider (M7's Spill policy) without this module
 *          needing to know BAKE exists.
 *
 *          M8, first increment: Mercury finally carries payload bytes, not
 *          just control-plane scalars -- vs_tr_reader_subscribe() (reader
 *          -> writer, "send me path P's data") and vs_tr_writer_push_data()
 *          (writer -> reader, the actual bytes for one entry of one step),
 *          both new custom hg_proc routines for a length-prefixed blob (no
 *          builtin Mercury type covers "arbitrary raw bytes", only fixed
 *          scalars and NUL-terminated strings). Whole-object granularity
 *          only this increment: a subscription names a path, not yet a
 *          sub-selection or a requested precision -- H5Sselect_intersect_
 *          block-based chunk routing and per-subscriber re-filtering are the
 *          flagged follow-up (M8.5, mirroring M6.5's pattern), see
 *          docs/dev-plan.md.
 *
 *          M8.5, per-subscriber precision: a subscription may carry an
 *          optional DCPL (H5Pencode2 bytes, opaque to this module -- see the
 *          "deliberately independent of HDF5" note below) requesting a
 *          filter pipeline different from the dataset's own. This module
 *          stays HDF5-free by never decoding those bytes itself: it invokes
 *          a caller-registered vs_tr_refilter_fn (vs_tr_set_refilter_cb())
 *          to turn a subscriber's raw overlap slice into that subscriber's
 *          own filtered bytes, and simply carries the result (plus the DCPL/
 *          type bytes the far side needs to reverse it) as more opaque
 *          blobs, mirroring how payload bytes already move today. A
 *          subscription with no DCPL (or one the refilter callback declines)
 *          gets the exact M8/M8.5 raw-bytes behavior unchanged.
 */

#ifndef VOL_STREAM_TR_MERCURY_H
#define VOL_STREAM_TR_MERCURY_H

#include <stdint.h>

/* Only for the margo_instance_id typedef vs_tr_get_mid() returns (M7) --
 * still no Mercury/Argobots/SSG types in this header's own signatures. */
#include <margo.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vs_tr_t vs_tr_t;

/* M8.5: implemented by H5VLstream.c, registered via vs_tr_set_refilter_cb()
 * below -- this module stays HDF5-free (see the header comment above), so
 * it never decodes dcpl_enc/type_enc itself, only carries them. Given
 * raw_buf (count elements of elem_size bytes, uncompressed/unfiltered),
 * dcpl_enc (H5Pencode2 bytes, the subscriber's requested pipeline) and
 * type_enc (H5Tencode bytes, the data's own type), the callback must run
 * raw_buf through that pipeline (H5VLstream.c does this via a temporary
 * in-memory dataset and H5Dread_chunk2()) and fill *out_buf (malloc'd --
 * vs_tr_writer_push_data() frees it once sent), *out_len and
 * *out_filter_mask (H5Dget_chunk_info_by_coord()'s filter_mask, needed on
 * the far side to reconstruct correctly if any filter chose to skip
 * itself). Returns 0 on success; a non-zero return falls back to sending
 * this subscriber raw, unfiltered bytes instead of failing the push. */
typedef int (*vs_tr_refilter_fn)(const void *raw_buf, uint64_t elem_size, uint64_t count,
                                   const uint8_t *dcpl_enc, uint64_t dcpl_enc_len, const uint8_t *type_enc,
                                   uint64_t type_enc_len, void **out_buf, uint64_t *out_len,
                                   uint32_t *out_filter_mask);

/* Registers the callback used to re-filter a subscriber's overlap slice
 * through their own requested DCPL (M8.5). NULL (the default) means every
 * push goes out as raw bytes regardless of what a subscriber requested,
 * exactly M8/M8.5's original whole-object/subrange-only behavior. Not
 * thread-safe against a concurrent vs_tr_writer_push_data() call -- call
 * once, right after vs_tr_start(), before any step replay can run. */
void vs_tr_set_refilter_cb(vs_tr_t *tr, vs_tr_refilter_fn fn);

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

/* Reader side: ask the writer (found by probing group members -- there is no
 * "rank 0 is the writer" shortcut, see the .c file) for the latest step it
 * has committed. Returns 0 with *physical_step / *wall_time_ns filled in, or -1
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

/* M7: the margo instance this vs_tr_t drives, so H5VLstream.c can embed
 * tr_bake.c's BAKE provider on the same instance/address rather than
 * standing up a second one -- a reader reaching this process's step_ready
 * RPCs can reach its BAKE provider too, with no new address to discover. */
margo_instance_id vs_tr_get_mid(vs_tr_t *tr);

/* Reader side: report that this reader has consumed through physical_step
 * (called automatically by H5VLstream.c after a sequential H5Fbegin_step()
 * advance -- not after H5Fbegin_logical_step()'s jump, which is exactly what
 * keeps a "latest-step-only" monitoring reader exempt from the writer's
 * queue-policy pressure, see vs_tr_writer_min_acked_step()). Best-effort:
 * failure to reach the writer is not fatal to the reader, it just means this
 * ack is lost and the writer's view of this reader's progress goes stale
 * until the next one. Returns 0 if the RPC was delivered and acknowledged,
 * -1 otherwise. */
int vs_tr_reader_ack_step(vs_tr_t *tr, uint64_t physical_step);

/* Writer side: the furthest-behind physical_step among readers that have
 * ever sent an ack and are still current SSG group members -- i.e. among
 * readers actually being tracked for backpressure. Returns 1 with
 * *min_acked_step filled in if at least one such reader exists, 0 if none do
 * (no group, no reader has ever acked, or every acking reader has since left
 * -- "no pressure": the caller should not apply any queue policy). A reader
 * that only ever jumps via H5Fbegin_logical_step() never appears here. */
int vs_tr_writer_min_acked_step(vs_tr_t *tr, uint64_t *min_acked_step);

/* Reader side: declare interest in path, bounded to the 1-D element range
 * [sel_start, sel_start + sel_count) -- M8.5's subvolume routing (see this
 * file's M8/M8.5 comment); pass sel_start=0, sel_count=UINT64_MAX for the
 * whole object. dcpl_enc/dcpl_enc_len (M8.5 precision) is the caller's own
 * H5Pencode2() bytes for the filter pipeline this subscriber wants pushed
 * data re-filtered through; pass NULL/0 for none (the original raw-bytes
 * behavior). Best-effort like vs_tr_reader_ack_step(): a failed RPC just
 * means this subscription is not registered and no data for path will
 * arrive, not a fatal error to the caller. Returns 0 if the RPC was
 * delivered and acknowledged, -1 otherwise. */
int vs_tr_reader_subscribe(vs_tr_t *tr, const char *path, uint64_t sel_start, uint64_t sel_count,
                            const uint8_t *dcpl_enc, uint64_t dcpl_enc_len);

/* Writer side: push the subset of buf that overlaps each subscriber's own
 * requested range to that subscriber (M8.5) -- buf holds write_count
 * elements of elem_size bytes each, starting at global element offset
 * write_start; a subscriber whose [sel_start, sel_start+sel_count) does not
 * overlap [write_start, write_start+write_count) at all receives nothing
 * for this call, not an empty push. type_enc/type_enc_len (M8.5 precision)
 * is buf's own H5Tencode() bytes, handed to the registered refilter
 * callback (vs_tr_set_refilter_cb()) for any subscriber that provided a
 * dcpl_enc at subscribe time; pass NULL/0 if the caller never registers a
 * callback (the type is then simply unused). Not an error for there to be
 * no subscribers at all (the ordinary case for most paths on most steps) --
 * mirrors vs_tr_writer_broadcast_step_ready()'s "no readers, proceed"
 * tolerance. Returns 0 on success (even if an individual member's RPC
 * failed), -1 only on a local error. */
int vs_tr_writer_push_data(vs_tr_t *tr, uint64_t physical_step, const char *path, const void *buf,
                            uint64_t elem_size, uint64_t write_start, uint64_t write_count,
                            const uint8_t *type_enc, uint64_t type_enc_len);

/* Reader side: block up to timeout_ms for a pushed data item (one may
 * already be queued), filling *physical_step, *out_path (newly malloc'd,
 * caller frees), *out_buf, *out_size (newly malloc'd, caller frees), and
 * *out_elem_start, *out_elem_count -- the (element-granularity) subrange of
 * the subscribed object this push covers (M8.5): the whole object's extent
 * for a whole-object subscription, or the overlap with what the writer
 * happened to write for a bounded one. *out_dcpl_enc/*out_dcpl_enc_len and
 * *out_type_enc/*out_type_enc_len (M8.5 precision, both newly malloc'd,
 * caller frees, NULL/0 if this push was not re-filtered) and
 * *out_filter_mask are what a caller needs to reverse the filtering (see
 * vs_tr_refilter_fn's comment) -- *out_buf is the *filtered* bytes in that
 * case, not decoded values. timeout_ms == 0 polls without blocking. Returns
 * 0 on success, -1 on timeout or if vs_tr_stop() was called while waiting
 * and no item remains queued. */
int vs_tr_reader_wait_data(vs_tr_t *tr, uint64_t timeout_ms, uint64_t *physical_step, char **out_path,
                            void **out_buf, uint64_t *out_size, uint64_t *out_elem_start,
                            uint64_t *out_elem_count, uint8_t **out_dcpl_enc, uint64_t *out_dcpl_enc_len,
                            uint8_t **out_type_enc, uint64_t *out_type_enc_len, uint32_t *out_filter_mask);

#ifdef __cplusplus
}
#endif

#endif /* VOL_STREAM_TR_MERCURY_H */
