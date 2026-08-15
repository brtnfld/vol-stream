/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of vol-stream, and is derived from the HDF5 pass-through *
 * VOL connector (src/H5VLpassthru.h).  The full HDF5 copyright notice,       *
 * including terms governing use, modification, and redistribution, is        *
 * contained in the LICENSE file, which can be found at the root of the       *
 * source code distribution tree, or in https://www.hdfgroup.org/licenses.    *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Purpose:     Public header for the vol-stream VOL connector.
 *
 *              M2 status: writes bracketed in a step are captured into a
 *              manifest (H5Tencode/H5Sencode2/H5Pencode2) and, at end_step,
 *              decoded and replayed group-based under /step/<n>/ in the
 *              underlying file.  Unbracketed writes remain a pure pass-through,
 *              identical to native HDF5.
 *
 *              M3 status: a file opened read-only is a reader. H5Fbegin_step()
 *              advances a read cursor instead of starting write capture, and
 *              subsequent object opens transparently resolve a bare app-given
 *              path to the correct /step/<k>/<path> replica -- the largest
 *              physical step at or before the reader's current one that
 *              actually has an entry for that path. H5Fbegin_logical_step()/
 *              H5Fget_logical_steps() do the same by logical id instead of
 *              physical step, skipping restart-superseded occurrences.
 *
 *              M4 status: two additions, both opt-in and both no-ops unless
 *              exercised, so M0-M3 behavior (byte-identity, the replay
 *              invariant, reader resolution) is untouched by default.
 *
 *              (1) Deferred write/attr-write requests. A dataset/attribute
 *              write captured into an open step now returns a real request
 *              object from H5Dwrite_async()/H5Awrite_async() when the
 *              caller passes an event set. Its completion tracks
 *              durability -- whether end_step()'s replay landed the entry
 *              in the underlying file -- not buffer safety, which was
 *              already guaranteed the moment the call returned (see
 *              H5VL__stream_make_deferred_request()'s comment in
 *              src/H5VLstream.c). All deferred requests from one step share
 *              a single completion cell and resolve together when that step
 *              commits, matching the step's own atomicity.
 *
 *              (2) The Mercury/Margo transport (src/tr_mercury.c), built
 *              only when VOL_STREAM_ENABLE_MERCURY finds mercury, argobots,
 *              mochi-margo and mochi-ssg (see CMakeLists.txt). Set the
 *              VOL_STREAM_NA environment variable (e.g. "na+sm", "ofi+tcp")
 *              to opt a file_create()/file_open() into it: a writer pushes
 *              a step_ready notification to every current group member
 *              (see M5, next) after each successful end_step(); a reader
 *              can call the new H5Fwait_step_ready() to block for the next
 *              one. This is the control-plane only -- step data still
 *              moves through the replicated /step/<n>/ file, not over
 *              Mercury, and a reader's own step index does not yet grow
 *              live in response to a notification (that needs reopening
 *              the file).
 *
 *              M5 status: rendezvous is an SSG group, not a hand-rolled
 *              attach RPC. A writer creates a single-member group at
 *              file_create()/file_open() time and stores its id to a
 *              "<filename>.vsgroup" sidecar file; a reader loads it and
 *              joins. SSG's SWIM failure detector means a reader that dies
 *              is simply absent from the group view the next time the
 *              writer broadcasts -- no liveness tracking of vol-stream's
 *              own, and no risk of a dead reader stalling the writer. A
 *              reader that joins mid-stream gets a coherent view
 *              automatically: joining triggers a query for whatever the
 *              writer has already committed, seeded into the same queue
 *              H5Fwait_step_ready() drains, so the first call after joining
 *              returns the current step immediately rather than blocking
 *              for a write the reader already missed.
 *
 *              M6 status (first increment -- see src/H5VLstream.c's
 *              H5VL__stream_replay_step() for the full scope note): a file
 *              opened with H5Pset_fapl_mpio() makes H5Fbegin_step()/
 *              H5Fend_step() collective over that communicator. Every
 *              writer rank creates the same set of objects (the ordinary
 *              parallel-HDF5 pattern -- all ranks call H5Dcreate2() etc.
 *              with matching arguments) and independently writes its own
 *              non-overlapping hyperslab; HDF5's own collective-metadata
 *              handling does the rest, so no connector-level cross-rank
 *              manifest aggregation is needed for this case. Verified
 *              byte-exact with the M6 exit gate's own coprime rank counts
 *              (7 writers/3 readers, 64/5).
 *
 *              M6.5 status: the two things M6's first increment named but
 *              didn't need are both done. Heterogeneous per-rank object sets
 *              (rank 0 creating a dataset rank 1 never touches) work via
 *              cross-rank manifest aggregation -- see
 *              H5VL__stream_replay_step_parallel()'s comment in
 *              src/H5VLstream.c. The Subfiling-style I/O-concentrator
 *              topology is opt-in via VOL_STREAM_CONCENTRATION -- see
 *              H5VL__stream_replay_concentrated_writes()'s comment. Both
 *              verified byte-exact at 7/3 and 64/5 (aggregation) and 3/2,
 *              7/3 (concentration). H5Sselect_project_intersection-based
 *              reader resolution -- combining a concentrator's contributors
 *              into fewer/larger reads -- remains the one open M6.5 item;
 *              neither of the above needed it, since the file's bytes are
 *              identical regardless of which rank wrote them.
 *
 *              M7 status: queue policy for a lagging reader
 *              (H5Fset_stream_queue_policy(), src/tr_mercury.c's reader-ack
 *              RPC, src/tr_bake.c's embedded BAKE provider). Opt-in and a
 *              no-op unless set -- with no policy, H5Fend_step() behaves
 *              exactly as through M6. Block waits for the reader; Discard
 *              drops the step's data entirely (the physical step still
 *              exists, so later steps stay reachable, but carries nothing);
 *              Spill writes the step's manifest+payload bytes to node-local
 *              storage instead of replaying them into the (possibly
 *              congested) shared file immediately, and a later
 *              H5Fend_step() drains the spill queue -- completes the real
 *              replay -- once the reader is back within the window. Spill
 *              needs VOL_STREAM_ENABLE_BAKE (falls back to Discard's
 *              behavior if the connector was built without it). See
 *              H5VL_stream_queue_policy_t's doc comment for the exact
 *              per-policy contract.
 *
 *              M8 status (first increment -- see src/tr_mercury.c's header
 *              comment for the full scope note): H5Fsubscribe() now does
 *              something -- it sends the writer a real RPC per subscribed
 *              path, and the writer pushes that path's actual payload bytes
 *              (Mercury's first RPC ever to carry more than small fixed
 *              scalars) to every subscriber at replay time. Retrieved via
 *              the new H5Fget_subscribed_data(). An unsubscribed sibling
 *              object in the same step is never sent, the increment's core
 *              thesis.
 *
 *              M8.5 status: subscription routing now honors a 1-D element
 *              subrange, not just the whole object -- H5Fsubscribe()'s
 *              dataspace selection (H5Sget_select_bounds(), dimension 0)
 *              travels with the subscription, and the writer computes the
 *              integer overlap between what it just wrote and each
 *              subscriber's requested range, sending only that slice.
 *              H5Fget_subscribed_data() reports the covered subrange via its
 *              elem_start/elem_count OUT params. Attribute paths ("@"-joined,
 *              H5VL__stream_attr_path()) are subscribable too.
 *
 *              Per-subscriber precision also landed: a subscription's DCPL
 *              (H5Fsubscribe()'s plists, previously accepted and ignored) now
 *              makes the writer re-filter that subscriber's slice through the
 *              requested pipeline before sending -- two subscribers to one
 *              object can get it at different precisions, and the object
 *              itself need not be filtered at all. Reversal is transparent
 *              (H5Fget_subscribed_data() always returns decoded values). See
 *              H5VL__stream_refilter_for_subscriber() in src/H5VLstream.c and
 *              vs_tr_refilter_fn in src/tr_mercury.h.
 *
 *              Update: routing now honors a subscription's *selection*, not
 *              just its bounding span. The H5Sencode2 blob travels to the
 *              writer, which intersects it with each write and sends one
 *              push per contiguous run of the intersection -- so a
 *              non-contiguous subscription (a column of a 2-D dataset)
 *              receives exactly its own elements rather than a superset.
 *
 *              Still open, and narrower than it once looked: re-filtering
 *              builds a single chunk spanning the pushed run rather than
 *              honoring a chunk *shape* a subscriber's DCPL asks for. That
 *              is about how a re-filtered push is stored in transit, not
 *              about which elements are chosen.
 *
 *              M9 status: predicate pushdown. H5Fsubscribe_predicate()
 *              attaches a value test to an existing subscription, and the
 *              writer evaluates it against the bytes it is about to send,
 *              transmitting only the runs of elements that satisfy it --
 *              or nothing at all for a write where none do. That last case
 *              is what distinguishes a predicate from every other narrowing
 *              in this protocol: subvolume routing and precision both still
 *              put something on the wire for each overlapping write.
 */

#ifndef H5VLstream_H
#define H5VLstream_H

#include "H5VLpublic.h" /* Virtual Object Layer */

/* Identifier for the vol-stream connector */
#define H5VL_STREAM (H5OPEN H5VL_STREAM_g)

/* Characteristics of the vol-stream connector.
 *
 * NOTE: H5VL_STREAM_VALUE is PROVISIONAL.  Values below H5_VOL_RESERVED (256)
 * are reserved for HDF5 library use; anything at or above it is available, but
 * there is no automatic collision check between third-party connectors.  An
 * official value should be requested from The HDF Group before any release.
 */
#define H5VL_STREAM_NAME    "vol-stream"
#define H5VL_STREAM_VALUE   1091 /* provisional; see note above */
#define H5VL_STREAM_VERSION 0

/* vol-stream connector info */
typedef struct H5VL_stream_info_t {
    hid_t under_vol_id;   /* VOL ID for underlying connector   */
    void *under_vol_info; /* VOL info for underlying connector */
} H5VL_stream_info_t;

/* The connector is built with hidden visibility so its ~140 internal callbacks
 * stay private; the public step API below is exported deliberately.
 */
#if defined(_MSC_VER)
#define H5VL_STREAM_API __declspec(dllexport)
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#define H5VL_STREAM_API __attribute__((visibility("default")))
#else
#define H5VL_STREAM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Global holding the connector ID (set when the connector registers) */
H5VL_STREAM_API extern hid_t H5VL_STREAM_g;

/*-------------------------------------------------------------------------
 * Step API
 *
 * These are thin wrappers over H5VLfile_optional_op(), invoking operations
 * the connector registers with H5VLregister_opt_operation() at init time.
 * No HDF5 library change is required for any of this.
 *
 * The H5F* naming follows the convention set by the HDF5 Async VOL, which
 * also ships H5F-namespaced calls from an out-of-tree connector.  A step is a
 * file-scoped transaction, so the file namespace is where it belongs.
 *
 * Registered and queryable via H5VLquery_optional(); begin_step/end_step
 * drive real capture and replay from M2 onward.
 *-------------------------------------------------------------------------*/

/* Operation names as registered in the optional-op registry.  Namespaced so
 * two connectors cannot collide, and so a future connector wanting source
 * compatibility can register the same strings.
 */
#define H5VL_STREAM_OP_BEGIN_STEP         "vol-stream:begin_step"
#define H5VL_STREAM_OP_END_STEP           "vol-stream:end_step"
#define H5VL_STREAM_OP_STEP_STATUS        "vol-stream:step_status"
#define H5VL_STREAM_OP_SUBSCRIBE          "vol-stream:subscribe"
#define H5VL_STREAM_OP_BEGIN_LOGICAL_STEP "vol-stream:begin_logical_step"
#define H5VL_STREAM_OP_GET_LOGICAL_STEPS  "vol-stream:get_logical_steps"
#define H5VL_STREAM_OP_WAIT_STEP_READY    "vol-stream:wait_step_ready"
#define H5VL_STREAM_OP_SET_QUEUE_POLICY   "vol-stream:set_queue_policy"
#define H5VL_STREAM_OP_GET_SUBSCRIBED_DATA "vol-stream:get_subscribed_data"
#define H5VL_STREAM_OP_SUBSCRIBE_PREDICATE "vol-stream:subscribe_predicate"

/** Status of the current step */
typedef enum H5F_step_status_t {
    H5F_STEP_NOT_IN_STEP = 0, /**< No step is open                     */
    H5F_STEP_IN_STEP     = 1, /**< A step is open and accepting writes */
    H5F_STEP_COMMITTING  = 2, /**< end_step in progress                */
    H5F_STEP_EOS         = 3, /**< Writer closed; no further steps     */
    H5F_STEP_READING     = 4  /**< M3: reader positioned at a step; begin_step
                                *   advances it                             */
} H5F_step_status_t;

/**
 * \brief M7: writer-only. How H5Fend_step() behaves when the furthest-behind
 *        tracked reader (see H5Fset_stream_queue_policy()) is more than
 *        \c reserve_slots steps behind the one about to commit.
 *
 * A reader is "tracked" once it has sent at least one ack, which happens
 * automatically after a sequential H5Fbegin_step() advance -- a reader that
 * only ever jumps via H5Fbegin_logical_step() (a monitoring/latest-only
 * reader) never acks and is therefore never counted as behind, regardless of
 * policy. With no policy set (the default), or with no tracked reader ever
 * behind the window, H5Fend_step() behaves exactly as it did through M6:
 * unconditional, synchronous, durable replay.
 */
typedef enum H5VL_stream_queue_policy_t {
    H5VL_STREAM_QUEUE_BLOCK   = 0, /**< Wait for the reader to catch up. Nothing
                                      *   is ever lost; the replay invariant holds
                                      *   for every step, same as with no policy
                                      *   set -- the only change is when
                                      *   H5Fend_step() returns.               */
    H5VL_STREAM_QUEUE_DISCARD = 1, /**< Do not wait. The step's data is dropped
                                      *   entirely -- the physical step still
                                      *   exists (so later steps stay reachable)
                                      *   but carries nothing.                 */
    H5VL_STREAM_QUEUE_SPILL   = 2  /**< Do not wait, and do not drop: write the
                                      *   step's bytes to node-local storage
                                      *   (BAKE) instead of replaying them into
                                      *   the (possibly congested) shared file
                                      *   now. A later H5Fend_step() drains it --
                                      *   completes the real replay -- once the
                                      *   tracked reader is back within the
                                      *   window. Falls back to Discard if this
                                      *   connector was built without BAKE.     */
} H5VL_stream_queue_policy_t;

/**
 * \brief Open a step on \p file_id.
 *
 * Collective over the file's communicator when the file was opened with MPI.
 *
 * \param file_id      File opened through the vol-stream connector
 * \param n_logical    Number of logical step ids (may be 0)
 * \param logical_ids  Logical step ids this step carries, or NULL
 * \param wall_time_ns Wall-clock time to record in the step manifest, in
 *                      nanoseconds since the Unix epoch. Caller-supplied,
 *                      never generated by the connector -- pass 0 if the
 *                      application does not track this itself.
 *
 * \return \herr_t
 *
 * \note The logical ids are deliberately separate from the connector's own
 *       monotone physical step counter.  A restart from a checkpoint replays
 *       logical ids that have already been seen, and a single monotone counter
 *       cannot represent that -- a problem openPMD hit in production against
 *       ADIOS2 and solved with an explicit annotation.
 *
 * \note M3: on a file opened H5F_ACC_RDONLY, this call is a *reader*
 *       operation instead -- it advances the read cursor to the next
 *       physical step (n_logical/logical_ids/wall_time_ns are write-only and
 *       ignored) and returns -1 once there is no next step. Object opens
 *       made afterward resolve bare paths against that step; see
 *       H5Fbegin_logical_step() to jump to a step by logical id instead.
 */
H5VL_STREAM_API herr_t H5Fbegin_step(hid_t file_id, size_t n_logical, const uint64_t *logical_ids,
                                      uint64_t wall_time_ns);

/**
 * \brief Commit the open step on \p file_id.
 *
 * Waits for every deferred operation issued since H5Fbegin_step(), validates
 * the step, and publishes it atomically.  Collective, as above.
 *
 * \param file_id  File opened through the vol-stream connector
 * \return \herr_t
 */
H5VL_STREAM_API herr_t H5Fend_step(hid_t file_id);

/**
 * \brief Query the step state of \p file_id.
 *
 * \param file_id  File opened through the vol-stream connector
 * \param status   Out: current step status
 * \return \herr_t
 */
H5VL_STREAM_API herr_t H5Fstep_status(hid_t file_id, H5F_step_status_t *status);

/**
 * \brief Position a reader's read cursor at the authoritative physical step
 *        for \p logical_id.
 *
 * "Authoritative" is the largest physical step whose manifest carries
 * \p logical_id -- if a restart rewrote it, this resolves to the later,
 * superseding occurrence, never an earlier one. \p file_id must be a reader
 * (opened H5F_ACC_RDONLY); see H5Fbegin_step() for the writer/reader
 * distinction.
 *
 * \param file_id     File opened through the vol-stream connector for reading
 * \param logical_id  Logical step id to position at
 * \return \herr_t, -1 if \p logical_id never appears in any step
 */
H5VL_STREAM_API herr_t H5Fbegin_logical_step(hid_t file_id, uint64_t logical_id);

/**
 * \brief Query the deduped, ascending, authoritative-only logical step ids
 *        present in \p file_id.
 *
 * A logical id superseded by a restart (see dev-plan.md decision #1) appears
 * once here, not once per physical occurrence -- this is the set a reader
 * would iterate to see the stream's logical history in order, each id
 * resolvable via H5Fbegin_logical_step().
 *
 * Two-call size-then-fill idiom, matching H5Tencode(): call once with
 * \p logical_ids NULL to get the count in \c *n_logical, then again with a
 * buffer of at least that size.
 *
 * \param file_id     File opened through the vol-stream connector for reading
 * \param n_logical   INOUT: buffer capacity in, id count out
 * \param logical_ids OUT: ascending logical ids, or NULL to size only
 * \return \herr_t
 */
H5VL_STREAM_API herr_t H5Fget_logical_steps(hid_t file_id, size_t *n_logical, uint64_t *logical_ids);

/**
 * \brief Declare reader interest in part of a stream.
 *
 * Each entry is an object path plus a dataspace whose selection bounds the
 * region wanted, and optionally a dataset creation property list requesting a
 * filter pipeline -- which is how per-subscriber precision is expressed
 * without a new codec.  The writer marshals only what is subscribed.
 *
 * \param file_id  File opened through the vol-stream connector for reading,
 *                 with the transport enabled (see VOL_STREAM_NA in
 *                 dev-plan.md's M4 section) -- a subscription cannot reach
 *                 the writer without it
 * \param count    Number of entries
 * \param paths    Object paths
 * \param spaces   Dataspace IDs carrying the wanted selection
 * \param plists   DCPL IDs requesting a filter pipeline, or NULL
 * \return \herr_t
 *
 * \note M8.5: both \p spaces and \p plists are now acted on. \p spaces bounds
 *       what is pushed to a 1-D element subrange (dimension 0 of
 *       H5Sget_select_bounds()); \p plists, when given, makes the writer
 *       re-filter that subrange through the requested pipeline before it goes
 *       on the wire, so two subscribers to the same object can receive it at
 *       different precisions. Reversal is transparent:
 *       H5Fget_subscribed_data() always hands back decoded values, never raw
 *       filtered bytes, so a caller never has to know whether a given push was
 *       re-filtered. A \p plists entry must be a real DCPL (H5P_DEFAULT means
 *       "no re-filtering", the original raw-bytes behavior).
 *
 * \note \p spaces is honored as a selection, not merely as a bounding
 *       span: the encoded selection travels to the writer, which sends one
 *       push per contiguous run of its intersection with each write. A
 *       selection that is contiguous in flat order (a whole object, a slab
 *       of leading dimensions, a 1-D subrange) arrives as a single push, as
 *       before; one that is not (a column) arrives as several, each
 *       truthfully labelled. A selection too fragmented to describe in a
 *       bounded number of runs falls back to the bounding span, which
 *       over-sends rather than under-sends. Call H5Fget_subscribed_data()
 *       after H5Fwait_step_ready() to retrieve what was pushed.
 *
 * \note Still follow-up scope: re-filtering (\p plists) always uses a
 *       single chunk spanning the pushed run rather than honoring a chunk
 *       shape requested there.
 */
H5VL_STREAM_API herr_t H5Fsubscribe(hid_t file_id, size_t count, const char *const *paths, const hid_t *spaces,
                    const hid_t *plists);

/**
 * \brief M9: the comparison a subscription predicate applies to each element.
 *
 * Deliberately a value test against one scalar constant and nothing more.
 * Anything richer (ranges, conjunctions, expressions over several objects)
 * would be a query language, and HDF5 has no H5Q/H5X in the tree to borrow
 * one from -- see dev-plan.md's M9 section for why that boundary is where
 * it is.
 */
typedef enum H5VL_stream_pred_op_t {
    H5VL_STREAM_PRED_LT = 0, /**< element <  value */
    H5VL_STREAM_PRED_LE = 1, /**< element <= value */
    H5VL_STREAM_PRED_GT = 2, /**< element >  value */
    H5VL_STREAM_PRED_GE = 3, /**< element >= value */
    H5VL_STREAM_PRED_EQ = 4, /**< element == value */
    H5VL_STREAM_PRED_NE = 5  /**< element != value */
} H5VL_stream_pred_op_t;

/**
 * \brief M9: reader only. Attach a value predicate to an existing
 *        subscription, so the writer sends only elements satisfying it.
 *
 * Predicate pushdown: the test is evaluated by the *writer*, against the
 * bytes it is about to marshal, so non-matching elements are never
 * serialized and never cross the wire. A write in which nothing matches
 * produces no RPC at all -- the only narrowing in this protocol that
 * reaches zero bytes, since subvolume routing and per-subscriber precision
 * both still send something for every overlapping write.
 *
 * H5Fsubscribe() must have succeeded for \p path first: a predicate narrows
 * a subscription rather than being one. A later H5Fsubscribe() naming the
 * same path clears the predicate, so re-subscribe first and re-apply the
 * predicate after, never the reverse.
 *
 * \param file_id  File opened through the vol-stream connector for reading,
 *                 with the transport enabled (see VOL_STREAM_NA)
 * \param path     A path passed to a previous H5Fsubscribe() on this file
 * \param op       The comparison to apply to every element
 * \param type_id  Datatype of \p value, as an ordinary in-memory type
 *                 (H5T_NATIVE_INT, say). Travels as H5Tencode() bytes, so a
 *                 writer of different endianness converts correctly
 * \param value    Address of the single scalar constant to compare against
 * \return \herr_t. -1 if the transport is unavailable, if \p path has no
 *         subscription on the writer, or if \p type_id cannot be encoded
 *
 * \note The predicate applies to atomic integer and floating-point data.
 *       Against anything else -- a compound, a string, an unsigned 64-bit
 *       integer whose values can exceed LLONG_MAX, a float wider than
 *       double -- the writer cannot evaluate it and sends the subscription's
 *       whole overlap instead, exactly as if no predicate had been set. This
 *       is deliberate and follows the same rule as the rest of the routing
 *       code: over-sending is inefficiency, under-sending is data loss. A
 *       reader that must see only matching elements should therefore re-test
 *       what arrives rather than treat delivery as proof of a match.
 *
 * \note Comparison happens in the data's own class, in \c long \c long for
 *       integers and \c double for floats, with \p value converted by HDF5's
 *       own conversion engine. A floating-point \p value against integer
 *       data is truncated toward zero by that conversion, so prefer a
 *       constant of the same class as the data.
 *
 * \note Matching elements are delivered as one push per maximal contiguous
 *       run, each carrying its own \c elem_start / \c elem_count through
 *       H5Fget_subscribed_data() -- no wire-format change, and a predicate
 *       matching one contiguous region stays a single push. A match set too
 *       fragmented to describe in a bounded number of runs is coalesced to
 *       the span containing it (a superset), never truncated.
 */
H5VL_STREAM_API herr_t H5Fsubscribe_predicate(hid_t file_id, const char *path, H5VL_stream_pred_op_t op,
                    hid_t type_id, const void *value);

/**
 * \brief M8/M8.5: reader only. Block until the writer pushes data for a
 *        subscribed path, or \p timeout_ms elapses.
 *
 * Only paths named in a prior H5Fsubscribe() call on this file ever produce
 * an item here. Several items may be queued (one per subscribed path per
 * step that writes it); each call drains exactly one, oldest first. A
 * subscription bounded to a subrange (M8.5) may receive several pushes per
 * step -- one per writer entry overlapping that range, each with its own
 * \p elem_start / \p elem_count -- rather than one push covering the whole
 * requested range at once.
 *
 * \param file_id       File opened through the vol-stream connector for
 *                      reading, with the transport enabled
 * \param timeout_ms    Milliseconds to wait, or 0 to poll without blocking
 * \param physical_step OUT: the step this data belongs to
 * \param path          OUT: the subscribed object path, newly malloc()'d --
 *                      caller frees with free()
 * \param buf           OUT: the pushed bytes, newly malloc()'d -- caller
 *                      frees with free()
 * \param size          OUT: length of \p buf in bytes
 * \param elem_start    OUT: first (1-D) element index \p buf covers
 * \param elem_count    OUT: number of elements \p buf covers
 * \return \herr_t, -1 on timeout or if the transport is unavailable for this
 *         file
 */
H5VL_STREAM_API herr_t H5Fget_subscribed_data(hid_t file_id, uint64_t timeout_ms, uint64_t *physical_step,
                                                char **path, void **buf, size_t *size, uint64_t *elem_start,
                                                uint64_t *elem_count);

/**
 * \brief M4/M5: reader only. Block until the writer's transport announces a
 *        newly committed step, or \p timeout_ms elapses.
 *
 * Unlike H5Fbegin_step(), this does not move the reader's cursor or touch
 * its step index -- it only reports that \p physical_step committed, with
 * the wall_time_ns the writer passed to its own H5Fbegin_step() call for
 * that step. Reading the step's data needs the reader to (re)open the file
 * so its index picks it up: the index is not yet maintained live against
 * incoming notifications.
 *
 * A reader that joins the writer's group mid-stream (M5: see
 * H5Fopen()/VOL_STREAM_NA) gets a coherent view automatically: the first
 * call to this function after opening returns the writer's current step
 * immediately, seeded by the join itself, rather than blocking for a write
 * the reader already missed.
 *
 * \param file_id       File opened through the vol-stream connector for
 *                      reading, with the transport enabled (see
 *                      VOL_STREAM_NA in dev-plan.md's M4 section)
 * \param timeout_ms    Milliseconds to wait, or 0 to poll without blocking
 * \param physical_step OUT: the physical step that committed
 * \param wall_time_ns  OUT: its wall_time_ns, or NULL if not wanted
 * \return \herr_t, -1 on timeout or if the transport is unavailable for
 *         this file (VOL_STREAM_NA was unset, the connector was built
 *         without Mercury, or \p file_id is not a reader)
 */
H5VL_STREAM_API herr_t H5Fwait_step_ready(hid_t file_id, uint64_t timeout_ms, uint64_t *physical_step,
                    uint64_t *wall_time_ns);

/**
 * \brief M7: writer only. Set the policy H5Fend_step() applies when a
 *        tracked reader (see H5VL_stream_queue_policy_t) falls more than
 *        \p reserve_slots steps behind the one about to commit.
 *
 * Takes effect starting with the next H5Fend_step() call; steps already
 * committed are unaffected. Not collective by itself in a parallel writer,
 * but every rank should set the same policy before the first H5Fend_step()
 * that could be affected, the same convention as other stream-wide settings.
 *
 * \param file_id       File opened through the vol-stream connector for
 *                      writing, with the transport enabled (see
 *                      VOL_STREAM_NA in dev-plan.md's M4 section) -- a
 *                      policy cannot do anything without the reader-ack
 *                      transport H5Fwait_step_ready() also needs
 * \param policy        Block, Discard, or Spill
 * \param reserve_slots How many steps a tracked reader may lag before
 *                      \p policy applies. 0 applies it the moment any
 *                      tracked reader is not caught up to the step just
 *                      about to commit.
 * \return \herr_t, -1 if \p file_id is not a writer
 */
H5VL_STREAM_API herr_t H5Fset_stream_queue_policy(hid_t file_id, H5VL_stream_queue_policy_t policy,
                                                    uint64_t reserve_slots);

/**
 * \brief Register the vol-stream connector and return its ID.
 *
 * Not needed when loading the connector as a plugin via HDF5_VOL_CONNECTOR;
 * provided for applications that link it directly.
 *
 * \return Connector ID on success, H5I_INVALID_HID on failure
 */
H5VL_STREAM_API hid_t H5VL_stream_register(void);

#ifdef __cplusplus
}
#endif

#endif /* H5VLstream_H */
