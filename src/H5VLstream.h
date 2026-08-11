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
 *              (7 writers/3 readers, 64/5). Heterogeneous per-rank object
 *              sets and the Subfiling-style I/O-concentrator aggregation
 *              topology dev-plan.md's M6 section calls for are not
 *              implemented yet -- both need real cross-rank manifest
 *              aggregation (H5Sselect_project_intersection), which this
 *              increment's uniform-topology assumption avoids needing.
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
 * \param file_id  File opened through the vol-stream connector for reading
 * \param count    Number of entries
 * \param paths    Object paths
 * \param spaces   Dataspace IDs carrying the wanted selection
 * \param plists   DCPL IDs requesting a filter pipeline, or NULL
 * \return \herr_t
 *
 * \note M0/M1 accept and record subscriptions without acting on them; the
 *       protocol lands in M8.
 */
H5VL_STREAM_API herr_t H5Fsubscribe(hid_t file_id, size_t count, const char *const *paths, const hid_t *spaces,
                    const hid_t *plists);

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
