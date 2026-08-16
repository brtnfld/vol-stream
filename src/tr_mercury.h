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
 *
 *          M9, predicate pushdown: a subscription may additionally carry an
 *          opaque predicate blob (vs_tr_reader_subscribe_predicate()). The
 *          writer evaluates it against the bytes it is about to send --
 *          through another registered callback, vs_tr_predicate_fn, for the
 *          same HDF5-free reason -- and sends only the runs of elements that
 *          match, or nothing at all when none do. This is the one reduction
 *          in the protocol that can take a push to zero bytes: subvolume
 *          routing and precision both still send *something* for every write
 *          that overlaps a subscription.
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
 * this subscriber raw, unfiltered bytes instead of failing the push.
 *
 * native_dcpl_enc/native_dcpl_enc_len (M8.5.1, chunk-level fast path) are
 * the H5Pencode2() bytes of the DCPL the write actually landed under --
 * NULL/0 if the caller has none available. native_ctx is likewise opaque to
 * this module (and to the caller in between, vs_tr_writer_push_data(), which
 * only forwards it): H5VLstream.c is both the sole producer and sole
 * consumer, using it to reach the just-written, already-filtered dataset
 * object directly (H5Dget_chunk_info_by_coord()/H5Dread_chunk2() against the
 * real chunk, not a throwaway one) when a subscriber's own dcpl_enc turns
 * out to match native_dcpl_enc's pipeline and chunking exactly -- true
 * zero-copy, skipping the temporary-dataset round trip entirely. Safe to
 * pass NULL for both on any call site that has nothing to offer; the
 * callback then always falls back to the temporary-dataset path. */
typedef int (*vs_tr_refilter_fn)(const void *raw_buf, uint64_t elem_size, uint64_t count,
                                   const uint8_t *dcpl_enc, uint64_t dcpl_enc_len, const uint8_t *type_enc,
                                   uint64_t type_enc_len, const uint8_t *native_dcpl_enc,
                                   uint64_t native_dcpl_enc_len, void *native_ctx, void **out_buf,
                                   uint64_t *out_len, uint32_t *out_filter_mask);

/* Registers the callback used to re-filter a subscriber's overlap slice
 * through their own requested DCPL (M8.5). NULL (the default) means every
 * push goes out as raw bytes regardless of what a subscriber requested,
 * exactly M8/M8.5's original whole-object/subrange-only behavior. Not
 * thread-safe against a concurrent vs_tr_writer_push_data() call -- call
 * once, right after vs_tr_start(), before any step replay can run. */
void vs_tr_set_refilter_cb(vs_tr_t *tr, vs_tr_refilter_fn fn);

/* M8.5 follow-up: implemented by H5VLstream.c, registered via
 * vs_tr_set_refilter_shape_cb(). Query-only, deliberately kept separate
 * from vs_tr_refilter_fn rather than changing that callback's contract:
 * given a subscriber's dcpl_enc and the element count of one run about to
 * be pushed to them, reports how many elements per chunk that DCPL
 * actually asked for via H5Pget_chunk() -- 0 means "no real preference
 * (unchunked, or a chunk shape >= count)," in which case the caller pushes
 * the whole run as one RPC exactly as before, calling vs_tr_refilter_fn
 * once with the full run.
 *
 * A return > 0 and < count means the caller instead splits the run into
 * that many (or fewer, for a final remainder) elements per slice and calls
 * vs_tr_refilter_fn once per slice, each becoming its own RPC with its own
 * elem_start/elem_count -- the same "one push per contiguous run"
 * mechanism M8.5's selection-run splitting and M9's predicate-run
 * splitting already use (see the reader-side note on
 * vs_tr_reader_wait_data() below), just splitting on chunk boundaries
 * instead of selection/predicate-match boundaries. This is why the far
 * side (vs_tr_reader_wait_data()) needs no change at all: each slice's own
 * refiltered bytes already are a single real chunk of exactly the
 * subscriber's requested size, the same shape vs_tr_refilter_fn always
 * produces -- only what counts as "the whole run" being refiltered in one
 * call changes, at the caller, not the callback's own behavior.
 *
 * NULL (the default) means every push still goes out as one RPC per run,
 * exactly the original M8.5 behavior -- the residual this closes is
 * opt-in via registering this callback, not a behavior change on its own.
 * Same threading rule as vs_tr_set_refilter_cb(): call once, right after
 * vs_tr_start(). */
typedef uint64_t (*vs_tr_refilter_shape_fn)(const uint8_t *dcpl_enc, uint64_t dcpl_enc_len, uint64_t count,
                                              uint64_t elem_size);
void vs_tr_set_refilter_shape_cb(vs_tr_t *tr, vs_tr_refilter_shape_fn fn);

/* M9 predicate pushdown: one maximal contiguous run of matching elements,
 * counted in elements relative to the overlap slice handed to
 * vs_tr_predicate_fn -- start == 0 is that slice's first element, not the
 * object's. vs_tr_writer_push_data() adds the slice's own global offset
 * back before a run goes on the wire. */
typedef struct vs_tr_run_t {
    uint64_t start;
    uint64_t count;
} vs_tr_run_t;

/* Cap on how many runs one overlap slice may be decomposed into. Each run
 * becomes its own RPC, so a predicate matching a finely alternating pattern
 * would otherwise turn one push into thousands of tiny ones -- slower and
 * bulkier than just sending everything. Past the cap the callback coalesces
 * rather than truncating (see vs_tr_predicate_fn); truncating would be data
 * loss. */
#define VS_TR_MAX_PRED_RUNS 64

/* M9: implemented by H5VLstream.c, registered via vs_tr_set_predicate_cb().
 * Same division of labour as vs_tr_refilter_fn -- this module carries
 * pred_enc and type_enc without ever decoding either.
 *
 * Given raw_buf (count elements of elem_size bytes, unfiltered) and this
 * subscriber's own pred_enc, the callback fills runs[] with the maximal
 * contiguous runs of elements satisfying the predicate and returns how many
 * it wrote (0 .. max_runs). A return of 0 -- "nothing here matches" -- is
 * the point of the whole exercise: the caller then sends this subscriber
 * nothing at all for this write, the only reduction in the protocol that
 * reaches zero bytes.
 *
 * Returning -1 means "cannot evaluate this": a datatype the predicate does
 * not apply to, a malformed blob, a failed HDF5 call. The caller then
 * pushes the whole overlap exactly as if no predicate had been given --
 * this project's standing rule that over-sending is inefficiency while
 * under-sending is data loss. A subscriber consequently may receive
 * elements that do not match, and must not read delivery as proof of a
 * match. */
typedef int (*vs_tr_predicate_fn)(const void *raw_buf, uint64_t elem_size, uint64_t count,
                                    const uint8_t *pred_enc, uint64_t pred_enc_len,
                                    const uint8_t *type_enc, uint64_t type_enc_len, vs_tr_run_t *runs,
                                    int max_runs);

/* Registers the callback used to evaluate a subscriber's predicate (M9).
 * NULL (the default) means a predicate is carried but never acted on, so
 * every push goes out whole -- exactly M8.5's behavior. Same threading rule
 * as vs_tr_set_refilter_cb(): call once, right after vs_tr_start(). */
void vs_tr_set_predicate_cb(vs_tr_t *tr, vs_tr_predicate_fn fn);

/* M8.5 follow-up: implemented by H5VLstream.c, registered via
 * vs_tr_set_selection_cb(). Same HDF5-free division of labour as the two
 * callbacks above -- this module carries H5Sencode2 blobs without decoding
 * them.
 *
 * A subscription's requested range reaches this module as a flat element
 * span, which is the smallest span *containing* the selection. For a
 * selection that is not contiguous in flat order -- a column of a 2-D
 * dataset, say, whose bounding span is nearly the whole dataset -- routing
 * on that span alone sends elements nobody asked for. Given the
 * subscriber's own encoded selection, this callback reports which parts of
 * the flat range [range_start, range_start + range_count) the selection
 * actually covers, as runs relative to range_start.
 *
 * write_space_enc is the encoded dataspace of the write being pushed, and
 * exists so the callback can confirm that both describe the same extent
 * before treating a flat index from one as meaningful in the other.
 * Mismatched extents are the shape of bug that has twice cost this project
 * silent data loss, so the callback declines rather than assumes.
 *
 * Returns the number of runs written (0 .. max_runs); 0 means the selection
 * does not touch this range at all and nothing is sent for it. Returns -1 to
 * decline -- an undecodable blob, extents that disagree, or a selection too
 * fragmented to describe in max_runs -- and the caller then sends the whole
 * range, exactly the bounding-span behavior this refines. Over-sending is
 * inefficiency; under-sending is data loss. */
typedef int (*vs_tr_selection_fn)(const uint8_t *sub_space_enc, uint64_t sub_space_enc_len,
                                    const uint8_t *write_space_enc, uint64_t write_space_enc_len,
                                    uint64_t range_start, uint64_t range_count, vs_tr_run_t *runs,
                                    int max_runs);

/* Registers the callback used to narrow a push to a subscriber's actual
 * selection. NULL (the default) leaves routing on the flat bounding span.
 * Same threading rule as vs_tr_set_refilter_cb(). */
void vs_tr_set_selection_cb(vs_tr_t *tr, vs_tr_selection_fn fn);

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
                            const uint8_t *dcpl_enc, uint64_t dcpl_enc_len, const uint8_t *space_enc,
                            uint64_t space_enc_len);

/* Reader side (M9): attach a predicate to an *existing* subscription for
 * path -- vs_tr_reader_subscribe() must have succeeded for the same path
 * first, since the predicate narrows a subscription rather than being one.
 * pred_enc is opaque here and meaningful only to the registered
 * vs_tr_predicate_fn; pass NULL/0 to clear a predicate previously set.
 * Returns 0 only if the writer both answered and found that subscription,
 * -1 otherwise (no writer reachable, or no such subscription) -- unlike
 * vs_tr_reader_subscribe() this distinction matters, because silently
 * dropping a predicate would over-send rather than fail visibly. */
int vs_tr_reader_subscribe_predicate(vs_tr_t *tr, const char *path, const uint8_t *pred_enc,
                                       uint64_t pred_enc_len);

/* Writer side: push the subset of buf that overlaps each subscriber's own
 * requested range to that subscriber (M8.5) -- buf holds write_count
 * elements of elem_size bytes each, starting at global element offset
 * write_start; a subscriber whose [sel_start, sel_start+sel_count) does not
 * overlap [write_start, write_start+write_count) at all receives nothing
 * for this call, not an empty push. type_enc/type_enc_len (M8.5 precision)
 * is buf's own H5Tencode() bytes, handed to the registered refilter
 * callback (vs_tr_set_refilter_cb()) for any subscriber that provided a
 * dcpl_enc at subscribe time; pass NULL/0 if the caller never registers a
 * callback (the type is then simply unused). space_enc/space_enc_len is
 * this write's own H5Sencode2() bytes, handed to the registered selection
 * callback (vs_tr_set_selection_cb()) so a subscriber's non-contiguous
 * selection can be honored exactly rather than as its bounding span; NULL/0
 * simply leaves that refinement off. Not an error for there to be
 * no subscribers at all (the ordinary case for most paths on most steps) --
 * mirrors vs_tr_writer_broadcast_step_ready()'s "no readers, proceed"
 * tolerance. Returns 0 on success (even if an individual member's RPC
 * failed), -1 only on a local error.
 *
 * M9: a subscriber carrying a predicate receives one RPC per matching run
 * of its overlap rather than one covering the whole overlap -- each run
 * truthfully labelled with its own elem_start/elem_count, so the wire
 * format is unchanged -- or no RPC at all if nothing matched. */
int vs_tr_writer_push_data(vs_tr_t *tr, uint64_t physical_step, const char *path, const void *buf,
                            uint64_t elem_size, uint64_t write_start, uint64_t write_count,
                            const uint8_t *type_enc, uint64_t type_enc_len,
                            const uint8_t *native_dcpl_enc, uint64_t native_dcpl_enc_len, void *native_ctx,
                            const uint8_t *space_enc, uint64_t space_enc_len);

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
