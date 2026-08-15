/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of vol-stream, and is derived from the HDF5 pass-through *
 * VOL connector (src/H5VLpassthru.c).  The full HDF5 copyright notice,       *
 * including terms governing use, modification, and redistribution, is        *
 * contained in the LICENSE file, which can be found at the root of the       *
 * source code distribution tree, or in https://www.hdfgroup.org/licenses.    *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Purpose:     This is a "pass through" VOL connector, which forwards each
 *              VOL callback to an underlying connector.
 *
 *              It is designed as an example VOL connector for developers to
 *              use when creating new connectors, especially connectors that
 *              are outside of the HDF5 library.  As such, it should _NOT_
 *              include _any_ private HDF5 header files.  This connector should
 *              therefore only make public HDF5 API calls and use standard C /
 *              POSIX calls.
 *
 *              Note that the HDF5 error stack must be preserved on code paths
 *              that could be invoked when the underlying VOL connector's
 *              callback can fail.
 *
 */

/* Header files needed */
/* Do NOT include private HDF5 files here! */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Public HDF5 file */
#include "hdf5.h"

/* M6: collective step boundaries for a file opened with the MPI-IO VFD.
 * H5_HAVE_PARALLEL is a fact about the HDF5 this connector is built
 * against (set in the public H5pubconf.h, pulled in by hdf5.h above), not
 * a vol-stream build option -- a serial HDF5 build simply never defines it,
 * and every M6 code path below compiles to nothing in that case. */
#ifdef H5_HAVE_PARALLEL
#include "H5FDmpio.h"
#endif

/* This connector's header */
#include "H5VLnative.h"
#include "H5PLextern.h"
#include "H5VLstream.h"

/* Generated from src/vol_stream.fbs by flatcc (see CMakeLists.txt). Pulls in
 * vol_stream_reader.h and flatcc/flatcc_builder.h transitively -- the step
 * manifest schema, and the builder/reader API used to serialize and decode it.
 * vol_stream_verifier.h (M3) adds vs_Step_verify_as_root(), used to validate
 * a manifest read back from durable storage before trusting it -- unlike
 * H5VL__stream_replay_step()'s just-built buffer, a reader's manifest could
 * in principle come from a corrupted or foreign file.
 */
#include "vol_stream_builder.h"
#include "vol_stream_verifier.h"

/* M4: the Mercury/Margo transport. Optional -- see CMakeLists.txt's
 * VOL_STREAM_ENABLE_MERCURY. Without it, begin_step/end_step/file_create/
 * file_open behave exactly as in M2/M3: no step_ready notification, no
 * address sidecar file. */
#ifdef VOL_STREAM_HAVE_MERCURY
#include "tr_mercury.h"
#include <unistd.h> /* usleep() -- bounded retry waiting for a writer's address sidecar file */

/* M7: BAKE-backed Spill queue policy. Needs VOL_STREAM_HAVE_MERCURY (BAKE's
 * provider rides the same margo instance tr_mercury.c already starts) --
 * see CMakeLists.txt's VOL_STREAM_ENABLE_BAKE. Without it, Spill behaves
 * like Discard (see H5VL__stream_apply_queue_policy()'s comment). */
#ifdef VOL_STREAM_HAVE_BAKE
#include "tr_bake.h"
#endif
#endif

/* Pin the VOL class struct version.
 *
 * The connector compiles H5VL_VERSION into its class struct, and a library
 * expecting a different one rejects it at load time -- the failure that broke
 * the ADIOS2 connector when H5VL_VERSION moved. Fail at compile time with a
 * readable message instead, since the load-time error is opaque.
 */
#ifndef H5VL_STREAM_SKIP_VERSION_CHECK
#if H5VL_VERSION < 2 || H5VL_VERSION > 3
#error "vol-stream is tested against H5VL_VERSION 2 and 3. Define H5VL_STREAM_SKIP_VERSION_CHECK to build anyway, and expect to audit the H5VL_class_t layout."
#endif
#endif

/**********/
/* Macros */
/**********/

/* Whether to display log message when callback is invoked */
/* (Uncomment to enable) */
/* #define ENABLE_STREAM_LOGGING */

/************/
/* Typedefs */
/************/

/* Forward declarations: H5VL_stream_pending_entry_t and H5VL_stream_t refer to
 * each other (a pending entry remembers the still-open placeholder wrapper
 * that owns it; a wrapper remembers the file_state it was captured under). */
typedef struct H5VL_stream_t             H5VL_stream_t;
typedef struct H5VL_stream_file_state_t  H5VL_stream_file_state_t;

/* One buffered create/write, captured while a step is open and not yet
 * replayed into the real file. See the M2 architecture note above
 * H5VL_stream_file_optional().
 *
 * kind is a vs_Kind_enum_t (DsetCreate, DsetWrite, or Attr -- M2 populates
 * only these three). type_id/space_id/dcpl_id/dapl_id are live copies held
 * only until end_step encodes and closes them; dcpl_id/dapl_id are
 * H5I_INVALID_HID on a DsetWrite entry, which never has its own DCPL/DAPL.
 */
/* M8.5.1: what the chunk-level zero-copy fast path needs to reach the real,
 * already-filtered dataset replay just wrote. Passed opaquely through
 * tr_mercury.c (which stays HDF5-free and only forwards the pointer) as
 * vs_tr_refilter_fn's native_ctx -- see H5VL__stream_refilter_zero_copy().
 *
 * under_dset/under_vol_id are borrowed: they belong to the replay frame that
 * is still mid-flight, valid only for the duration of the
 * vs_tr_writer_push_data() call they are handed to, and must be neither
 * closed nor retained. The under-VOL object is deliberately carried raw
 * rather than as an hid_t: H5VLwrap_register() would hand back an id that
 * *owns* the object, so releasing it would close the dataset replay itself
 * still owns and later closes (observed as a double-close crash -- the same
 * trap the H5R replay attempt hit). The chunk reads therefore go through the
 * native connector's own optional operations, which take the object
 * directly.
 *
 * write_start/total_elems describe the write being pushed, so the fast path
 * can confirm it covers the whole dataset before serving stored chunk bytes.
 */
typedef struct H5VL_stream_native_chunk_ctx_t {
    void    *under_dset;
    hid_t    under_vol_id;
    uint64_t write_start;
    uint64_t total_elems;
} H5VL_stream_native_chunk_ctx_t;

typedef struct H5VL_stream_pending_entry_t {
    int            kind;
    char          *path;
    hid_t          type_id;
    hid_t          space_id;
    hid_t          dcpl_id;
    hid_t          dapl_id;
    uint8_t       *payload;
    size_t         payload_len;
    H5VL_stream_t *owner_wrapper; /* still-open placeholder handle, or NULL */
} H5VL_stream_pending_entry_t;

/* M3: one logical id's authoritative (largest / most recent) physical step,
 * built by H5VL__stream_reader_build_index() scanning steps in ascending
 * order and overwriting on each occurrence -- a later restart write always
 * supersedes an earlier one, per dev-plan.md decision #1. */
typedef struct H5VL_stream_logical_map_entry_t {
    uint64_t logical_id;
    uint64_t physical_step;
} H5VL_stream_logical_map_entry_t;

/* M3: every physical step (DsetCreate/Attr entry) that ever existed for one
 * path, ascending. Built in physical-step scan order, so already sorted --
 * H5VL__stream_path_index_resolve() just scans from the end for the largest
 * entry <= the reader's current step. */
typedef struct H5VL_stream_path_steps_t {
    char     *path;      /* matches Entry.path's capture convention exactly,
                           * post H5VL__stream_child_path()'s normalization */
    uint64_t *steps;
    size_t    n_steps;
    size_t    cap_steps;
} H5VL_stream_path_steps_t;

/* M6.5/dictionary caching: the last type_enc (H5Tencode() bytes) sent for a
 * given DsetWrite path, on both the writer side (H5VL__stream_build_
 * manifest(), to decide whether this step's type_enc can be omitted) and
 * the replay side (H5VL__stream_replay_manifest(), to resolve an omitted
 * one back into real bytes). Two independent arrays -- one per role -- both
 * using this same element type; see H5VL__stream_type_cache_lookup()'s
 * comment for why no schema change was needed to carry the "omitted"
 * sentinel. */
typedef struct H5VL_stream_type_cache_entry_t {
    char    *path;
    uint8_t *type_enc;
    size_t   type_enc_len;
} H5VL_stream_type_cache_entry_t;

/* One H5VL_stream_request_notify() registration still waiting on a step's
 * completion cell to resolve -- a singly linked list since there is no
 * bound on how many deferred requests from the same step get a callback
 * registered before end_step() runs. */
typedef struct H5VL_stream_step_notify_t {
    H5VL_request_notify_t              cb;
    void                               *ctx;
    struct H5VL_stream_step_notify_t   *next;
} H5VL_stream_step_notify_t;

/* M4: the fate every deferred write/attr-write request issued during one
 * step shares. A step is the unit of atomicity (dev-plan.md's state-machine
 * table: "COMMITTING: Barrier over this step's requests"), so request
 * objects do not track completion per entry -- they share one refcounted
 * cell, created when a step opens and resolved by end_step() once replay
 * finishes (or fails). status: 0 = pending, 1 = succeeded, -1 = failed.
 * notify_list is drained (each callback invoked) by end_step() at the same
 * point status is resolved; any left over when the cell is freed (an
 * unclosed step at file_close(), see H5VL__stream_file_state_decref()) are
 * freed without being invoked -- correct, since that step never committed
 * and never will. */
typedef struct H5VL_stream_step_completion_t {
    unsigned                    refcount;
    int                         status;
    H5VL_stream_step_notify_t  *notify_list;
} H5VL_stream_step_completion_t;

#ifdef VOL_STREAM_HAVE_BAKE
/* M7: one step currently sitting in BAKE rather than fully replayed into the
 * shared file -- see H5VL__stream_spill_step()/H5VL__stream_drain_spill().
 * desc is the string tr_bake.c's vs_bake_spill_write() returned; owned by
 * this entry, freed when the entry is drained or the file closes. size is
 * what vs_bake_spill_write() was given -- vs_bake_spill_read() needs it
 * back verbatim, since the file backend's bake_get_size() is not usable
 * (confirmed against BAKE 0.6.4: returns BAKE_ERR_OP_UNSUPPORTED). */
typedef struct H5VL_stream_spill_entry_t {
    uint64_t physical_step;
    char    *desc;
    uint64_t size;
} H5VL_stream_spill_entry_t;
#endif

/* Step state and the pending-entry buffer for one open file. Refcounted: the
 * file's own wrapper and every dataset/attribute/group/datatype wrapper
 * opened under it borrow this pointer, so it outlives the file wrapper if a
 * child object is still open when the file closes.
 */
struct H5VL_stream_file_state_t {
    unsigned                     refcount;
    H5F_step_status_t            step_state;
    uint64_t                     physical_step; /* next step number to assign */
    uint64_t                    *logical_ids;   /* ids carried by the open step */
    size_t                       n_logical;
    uint64_t                     wall_time_ns;  /* caller-supplied, see H5Fbegin_step */
    H5VL_stream_pending_entry_t *pending;        /* growable array */
    size_t                       n_pending;
    size_t                       cap_pending;

    /* M4: the open step's shared completion cell (NULL outside IN_STEP/
     * COMMITTING), and the transport used to announce a committed step to
     * attached readers. transport is NULL unless the file was opened with
     * the connector's transport enabled (see H5VL__stream_transport_env());
     * a NULL transport makes end_step()'s notification a no-op, so this is
     * fully backward compatible with M0-M3 byte-identity/replay-invariant
     * behavior. */
    H5VL_stream_step_completion_t *current_completion;
#ifdef VOL_STREAM_HAVE_MERCURY
    vs_tr_t *transport;
#endif

    /* M7: writer-side queue policy, opt-in via H5Fset_stream_queue_policy()
     * (queue_policy_set stays 0 otherwise, so H5Fend_step() takes the exact
     * M0-M6 code path -- see H5VL__stream_apply_queue_policy()). spill_bake/
     * pending_spill are lazily created on this file's first Spill-policy
     * eviction; NULL/empty otherwise, even with the policy set, if it never
     * actually triggers. */
    int                          queue_policy_set;
    H5VL_stream_queue_policy_t   queue_policy;
    uint64_t                     reserve_slots;
#ifdef VOL_STREAM_HAVE_BAKE
    vs_bake_t                   *spill_bake;
    char                         *spill_dir;
    H5VL_stream_spill_entry_t   *pending_spill; /* growable array, oldest first */
    size_t                        n_pending_spill;
    size_t                        cap_pending_spill;
#endif

    /* M6: set when this file was opened with H5Pset_fapl_mpio(), i.e. a
     * parallel writer/reader over the given communicator. comm is this
     * connector's own duplicate (H5Pget_fapl_mpio() itself already hands
     * back a fresh duplicate -- see H5VL__stream_transport_na()-adjacent
     * comment in file_create()/file_open()), freed in
     * H5VL__stream_file_state_decref(). has_comm is checked instead of
     * comparing against MPI_COMM_NULL so this struct needs no MPI type at
     * all in a serial build (see H5_HAVE_PARALLEL note at the top of this
     * file). begin_step()/end_step() collective-barrier around the step
     * boundary; the replay itself needs no other change -- see
     * H5VL__stream_replay_step()'s M6 comment for why. */
#ifdef H5_HAVE_PARALLEL
    MPI_Comm comm;
    int      has_comm;
    int      mpi_rank;
    int      mpi_size;
#endif

    /* M3: reader state. under_object/under_vol_id (on H5VL_stream_t) belong
     * to whatever object a given wrapper wraps; file_under_object/
     * file_under_vol_id are always the FILE's own, so a wrapper several
     * levels deep can still route an absolute "/step/<k>/<path>" open
     * through the file root rather than through some other resolved
     * object's under_object -- see the M3 plan's "Critical finding #1". Set
     * once at file_create()/file_open() time, never reassigned. */
    void  *file_under_object;
    hid_t  file_under_vol_id;

    unsigned is_reader;      /* set at file_open() from flags */
    int      index_built;    /* built lazily, on first reader begin_step */
    uint64_t current_step;   /* valid once step_state == H5F_STEP_READING */

    size_t     n_steps_total;      /* physical steps are always 0..n-1, contiguous */
    uint64_t **step_logical_ids;   /* [p] -> malloc'd array, n_logical_per_step[p] entries */
    size_t    *n_logical_per_step;

    H5VL_stream_logical_map_entry_t *logical_map; /* sorted ascending by logical_id after build */
    size_t                           n_logical_map;
    size_t                           cap_logical_map;

    H5VL_stream_path_steps_t *path_index;
    size_t                     n_path_index;
    size_t                     cap_path_index;

    /* Dictionary caching for DsetWrite's type_enc, see H5VL__stream_type_
     * cache_entry_t's comment. write_type_cache is consulted/updated by
     * H5VL__stream_build_manifest() (the writer role); replay_type_cache by
     * H5VL__stream_replay_manifest() (immediately after, same process, same
     * step -- see that function's own comment on why "reader" here does not
     * mean a separate process). Both are empty/unused unless a DsetWrite
     * ever runs, so this has no cost for files that never write. */
    H5VL_stream_type_cache_entry_t *write_type_cache;
    size_t                           n_write_type_cache;
    size_t                           cap_write_type_cache;
    H5VL_stream_type_cache_entry_t *replay_type_cache;
    size_t                           n_replay_type_cache;
    size_t                           cap_replay_type_cache;
};

/* Whether a wrapped object is a real, opened/created underlying object, a
 * bookkeeping placeholder standing in for a create+write deferred into the
 * pending step manifest, or (M3) a reader-mode virtual group. H5VL_STREAM_OBJ_LIVE
 * is 0, the calloc() default, so every object not explicitly made something
 * else is correctly LIVE. */
typedef enum H5VL_stream_obj_state_t {
    H5VL_STREAM_OBJ_LIVE               = 0,
    H5VL_STREAM_OBJ_PLACEHOLDER        = 1,
    H5VL_STREAM_OBJ_READER_VIRTUAL     = 2, /* M3: reader-mode group wrapper -- path/
                                              * file_state bookkeeping only, no single
                                              * underlying object exists. See
                                              * H5VL_stream_group_open(). */
    H5VL_STREAM_OBJ_DEFERRED_REQUEST   = 3  /* M4: a request object returned by a
                                              * dataset/attribute write captured
                                              * into the current step. Carries no
                                              * under_object of its own -- see
                                              * deferred_completion below. */
} H5VL_stream_obj_state_t;

/* The pass through VOL info object */
struct H5VL_stream_t {
    hid_t under_vol_id; /* ID for underlying VOL connector */
    void *under_object; /* Info object for underlying VOL connector; NULL while
                          * obj_state == H5VL_STREAM_OBJ_PLACEHOLDER */

    /* M2 step-capture bookkeeping. file_state is borrowed and refcounted (see
     * H5VL__stream_file_state_incref/decref); path is this object's absolute
     * path from the file root ("" for the file itself), or NULL when it
     * cannot be resolved (BY_NAME/BY_IDX/BY_TOKEN entry points, object_open,
     * wrap_object) -- capture is simply skipped for those, never incorrect.
     */
    H5VL_stream_file_state_t *file_state;
    char                     *path;
    H5VL_stream_obj_state_t   obj_state;
    size_t                    pending_index; /* valid only when obj_state ==
                                               * H5VL_STREAM_OBJ_PLACEHOLDER */
    H5VL_stream_step_completion_t *deferred_completion; /* valid only when obj_state ==
                                                           * H5VL_STREAM_OBJ_DEFERRED_REQUEST;
                                                           * a reference this request object
                                                           * owns, dropped in request_free()/
                                                           * on resolution. */
} /* H5VL_stream_t -- see forward-declared typedef above */;

/* The pass through VOL wrapper context */
typedef struct H5VL_stream_wrap_ctx_t {
    hid_t under_vol_id;   /* VOL ID for under VOL */
    void *under_wrap_ctx; /* Object wrapping context for under VOL */
} H5VL_stream_wrap_ctx_t;

/********************* */
/* Function prototypes */
/********************* */

/* Helper routines */
static H5VL_stream_t *H5VL_stream_new_obj(void *under_obj, hid_t under_vol_id);
static herr_t               H5VL_stream_free_obj(H5VL_stream_t *obj);

/* M2: step-capture helpers */
static H5VL_stream_file_state_t *H5VL__stream_file_state_new(void);
static void  H5VL__stream_file_state_incref(H5VL_stream_file_state_t *fs);
static void  H5VL__stream_file_state_decref(H5VL_stream_file_state_t *fs);
static void  H5VL__stream_pending_entry_clear(H5VL_stream_pending_entry_t *e);
static void  H5VL__stream_pending_discard_all(H5VL_stream_file_state_t *fs);
static char *H5VL__stream_child_path(const char *parent_path, const char *name);
static char *H5VL__stream_attr_path(const char *parent_path, const char *name);
static H5VL_stream_t *H5VL__stream_new_child_obj(void *under_obj, hid_t under_vol_id,
                             H5VL_stream_file_state_t *file_state, const char *parent_path,
                             const char *name);
static size_t H5VL__stream_pending_append(H5VL_stream_file_state_t *fs,
                             const H5VL_stream_pending_entry_t *entry);
static hid_t  H5VL__stream_resolve_space(hid_t space_id, hid_t fallback_space_id);
static htri_t H5VL__stream_type_unsafe_to_capture(hid_t type_id);
static const uint8_t *H5VL__stream_type_cache_lookup(H5VL_stream_type_cache_entry_t *arr, size_t n,
                             const char *path, size_t *out_len);
static herr_t H5VL__stream_type_cache_upsert(H5VL_stream_type_cache_entry_t **arr, size_t *n, size_t *cap,
                             const char *path, const uint8_t *type_enc, size_t type_enc_len);
static void   H5VL__stream_type_cache_clear(H5VL_stream_type_cache_entry_t *arr, size_t n);
static herr_t H5VL__stream_build_manifest(H5VL_stream_file_state_t *fs, uint8_t **out_manifest_buf,
                             size_t *out_manifest_len, uint8_t **out_payload_buf, size_t *out_payload_len);
static herr_t H5VL__stream_replay_manifest(H5VL_stream_t *file_obj, const uint8_t *manifest_buf,
                             size_t manifest_len, const uint8_t *payload_buf,
                             H5VL_stream_pending_entry_t *pending_for_wiring, size_t n_pending_for_wiring);
static herr_t H5VL__stream_replay_step(H5VL_stream_t *file_obj);
/* M7: queue policy -- see H5VL__stream_apply_queue_policy()'s comment for
 * what each does and why they are safe to call instead of a full replay. */
static herr_t H5VL__stream_discard_step(H5VL_stream_t *file_obj);
#ifdef VOL_STREAM_HAVE_BAKE
static herr_t H5VL__stream_spill_step(H5VL_stream_t *file_obj);
static herr_t H5VL__stream_drain_spill(H5VL_stream_t *file_obj, uint64_t min_acked_step);
#endif
static herr_t H5VL__stream_apply_queue_policy(H5VL_stream_t *file_obj);
#ifdef H5_HAVE_PARALLEL
/* M6.5: heterogeneous per-rank object sets -- see
 * H5VL__stream_replay_step_parallel()'s comment for the full design. */
static herr_t H5VL__stream_build_agg_manifest(H5VL_stream_file_state_t *fs, uint8_t **out_buf,
                             size_t *out_len, uint8_t **out_payload_buf, size_t *out_payload_len);
static herr_t H5VL__stream_merge_agg_manifests(H5VL_stream_file_state_t *fs, uint8_t **bufs, int *lens,
                             uint8_t **payload_bufs, int *payload_lens, int nranks, int my_rank,
                             uint8_t **out_merged_buf, size_t *out_merged_len, uint8_t **out_merged_payload,
                             size_t *out_merged_payload_len, H5VL_stream_pending_entry_t **out_wiring,
                             size_t *out_n_wiring);
static herr_t H5VL__stream_replay_local_writes(H5VL_stream_t *file_obj, uint64_t physical_step);
static herr_t H5VL__stream_replay_step_parallel(H5VL_stream_t *file_obj);
/* M6.5 (concentrator topology): see H5VL__stream_replay_concentrated_writes()'s
 * comment for the design. */
static int    H5VL__stream_concentration_factor(void);
static herr_t H5VL__stream_write_replica(H5VL_stream_t *file_obj, uint64_t physical_step,
                             const char *rel_path, hid_t type_id, hid_t space_id, const void *payload);
static herr_t H5VL__stream_send_write_entry_to_concentrator(const H5VL_stream_pending_entry_t *pe,
                             int concentrator, MPI_Comm comm);
static herr_t H5VL__stream_recv_and_write_entry(H5VL_stream_t *file_obj, uint64_t physical_step,
                             int source, MPI_Comm comm);
static herr_t H5VL__stream_replay_concentrated_writes(H5VL_stream_t *file_obj, uint64_t physical_step,
                             int group_size);
#endif

/* M3: reader helpers */
static herr_t H5VL__stream_reader_build_index(H5VL_stream_file_state_t *fs);
static herr_t H5VL__stream_reader_advance(H5VL_stream_file_state_t *fs, int has_target, uint64_t target_step);
static herr_t H5VL__stream_path_index_resolve(H5VL_stream_file_state_t *fs, const char *path,
                             uint64_t current_step, uint64_t *resolved_step);
static char  *H5VL__stream_resolve_physical_path(H5VL_stream_file_state_t *fs, const char *logical_path);
static void  *H5VL__stream_reader_open_dataset(H5VL_stream_t *o, const char *name, hid_t dapl_id,
                             hid_t dxpl_id, void **req);
static void  *H5VL__stream_reader_open_attr(H5VL_stream_t *o, const char *name, hid_t aapl_id,
                             hid_t dxpl_id, void **req);

/* M4: deferred-request and transport helpers */
static H5VL_stream_step_completion_t *H5VL__stream_step_completion_new(void);
static void H5VL__stream_step_completion_incref(H5VL_stream_step_completion_t *c);
static void H5VL__stream_step_completion_decref(H5VL_stream_step_completion_t *c);
static herr_t H5VL__stream_make_deferred_request(H5VL_stream_file_state_t *fs, void **req);
static void H5VL__stream_deferred_request_free(H5VL_stream_t *r);
#ifdef VOL_STREAM_HAVE_MERCURY
static const char *H5VL__stream_transport_na(void);
static char *H5VL__stream_vsaddr_path(const char *filename);
static void H5VL__stream_transport_start_writer(H5VL_stream_file_state_t *fs, const char *name);
static void H5VL__stream_transport_start_reader(H5VL_stream_file_state_t *fs, const char *name);
/* M8.5 precision: see H5VL__stream_refilter_for_subscriber()'s comment --
 * vs_tr_refilter_fn's implementation, and H5VL__stream_unfilter_pushed_
 * data()'s the reverse operation, used in the get_subscribed_data handler. */
static int H5VL__stream_refilter_for_subscriber(const void *raw_buf, uint64_t elem_size, uint64_t count,
                             const uint8_t *dcpl_enc, uint64_t dcpl_enc_len, const uint8_t *type_enc,
                             uint64_t type_enc_len, const uint8_t *native_dcpl_enc,
                             uint64_t native_dcpl_enc_len, void *native_ctx, void **out_buf,
                             uint64_t *out_len, uint32_t *out_filter_mask);
static int H5VL__stream_refilter_zero_copy(const void *raw_buf, uint64_t elem_size, uint64_t count,
                             const uint8_t *dcpl_enc, uint64_t dcpl_enc_len, const uint8_t *native_dcpl_enc,
                             uint64_t native_dcpl_enc_len, void *native_ctx, void **out_buf,
                             uint64_t *out_len, uint32_t *out_filter_mask);
static int H5VL__stream_unfilter_pushed_data(const void *filtered_buf, uint64_t filtered_len,
                             const uint8_t *dcpl_enc, uint64_t dcpl_enc_len, const uint8_t *type_enc,
                             uint64_t type_enc_len, uint64_t count, uint32_t filter_mask, void **out_buf,
                             size_t *out_len);
#endif
#ifdef H5_HAVE_PARALLEL
static void H5VL__stream_detect_mpi_comm(H5VL_stream_file_state_t *fs, hid_t fapl_id);
#endif

/* "Management" callbacks */
static herr_t H5VL_stream_init(hid_t vipl_id);
static herr_t H5VL_stream_term(void);

/* VOL info callbacks */
static void  *H5VL_stream_info_copy(const void *info);
static herr_t H5VL_stream_info_cmp(int *cmp_value, const void *info1, const void *info2);
static herr_t H5VL_stream_info_free(void *info);
static herr_t H5VL_stream_info_to_str(const void *info, char **str);
static herr_t H5VL_stream_str_to_info(const char *str, void **info);

/* VOL object wrap / retrieval callbacks */
static void  *H5VL_stream_get_object(const void *obj);
static herr_t H5VL_stream_get_wrap_ctx(const void *obj, void **wrap_ctx);
static void  *H5VL_stream_wrap_object(void *obj, H5I_type_t obj_type, void *wrap_ctx);
static void  *H5VL_stream_unwrap_object(void *obj);
static herr_t H5VL_stream_free_wrap_ctx(void *obj);

/* Attribute callbacks */
static void  *H5VL_stream_attr_create(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                                            hid_t type_id, hid_t space_id, hid_t acpl_id, hid_t aapl_id,
                                            hid_t dxpl_id, void **req);
static void  *H5VL_stream_attr_open(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                                          hid_t aapl_id, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_attr_read(void *attr, hid_t mem_type_id, void *buf, hid_t dxpl_id,
                                          void **req);
static herr_t H5VL_stream_attr_write(void *attr, hid_t mem_type_id, const void *buf, hid_t dxpl_id,
                                           void **req);
static herr_t H5VL_stream_attr_get(void *obj, H5VL_attr_get_args_t *args, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_attr_specific(void *obj, const H5VL_loc_params_t *loc_params,
                                              H5VL_attr_specific_args_t *args, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_attr_optional(void *obj, H5VL_optional_args_t *args, hid_t dxpl_id,
                                              void **req);
static herr_t H5VL_stream_attr_close(void *attr, hid_t dxpl_id, void **req);

/* Dataset callbacks */
static void  *H5VL_stream_dataset_create(void *obj, const H5VL_loc_params_t *loc_params,
                                               const char *name, hid_t lcpl_id, hid_t type_id, hid_t space_id,
                                               hid_t dcpl_id, hid_t dapl_id, hid_t dxpl_id, void **req);
static void  *H5VL_stream_dataset_open(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                                             hid_t dapl_id, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_dataset_read(size_t count, void *dset[], hid_t mem_type_id[],
                                             hid_t mem_space_id[], hid_t file_space_id[], hid_t plist_id,
                                             void *buf[], void **req);
static herr_t H5VL_stream_dataset_write(size_t count, void *dset[], hid_t mem_type_id[],
                                              hid_t mem_space_id[], hid_t file_space_id[], hid_t plist_id,
                                              const void *buf[], void **req);
static herr_t H5VL_stream_dataset_get(void *dset, H5VL_dataset_get_args_t *args, hid_t dxpl_id,
                                            void **req);
static herr_t H5VL_stream_dataset_specific(void *obj, H5VL_dataset_specific_args_t *args, hid_t dxpl_id,
                                                 void **req);
static herr_t H5VL_stream_dataset_optional(void *obj, H5VL_optional_args_t *args, hid_t dxpl_id,
                                                 void **req);
static herr_t H5VL_stream_dataset_close(void *dset, hid_t dxpl_id, void **req);

/* Datatype callbacks */
static void *H5VL_stream_datatype_commit(void *obj, const H5VL_loc_params_t *loc_params,
                                               const char *name, hid_t type_id, hid_t lcpl_id, hid_t tcpl_id,
                                               hid_t tapl_id, hid_t dxpl_id, void **req);
static void *H5VL_stream_datatype_open(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                                             hid_t tapl_id, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_datatype_get(void *dt, H5VL_datatype_get_args_t *args, hid_t dxpl_id,
                                             void **req);
static herr_t H5VL_stream_datatype_specific(void *obj, H5VL_datatype_specific_args_t *args,
                                                  hid_t dxpl_id, void **req);
static herr_t H5VL_stream_datatype_optional(void *obj, H5VL_optional_args_t *args, hid_t dxpl_id,
                                                  void **req);
static herr_t H5VL_stream_datatype_close(void *dt, hid_t dxpl_id, void **req);

/* File callbacks */
static void  *H5VL_stream_file_create(const char *name, unsigned flags, hid_t fcpl_id, hid_t fapl_id,
                                            hid_t dxpl_id, void **req);
static void  *H5VL_stream_file_open(const char *name, unsigned flags, hid_t fapl_id, hid_t dxpl_id,
                                          void **req);
static herr_t H5VL_stream_file_get(void *file, H5VL_file_get_args_t *args, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_file_specific(void *file, H5VL_file_specific_args_t *args, hid_t dxpl_id,
                                              void **req);
static herr_t H5VL_stream_file_optional(void *file, H5VL_optional_args_t *args, hid_t dxpl_id,
                                              void **req);
static herr_t H5VL_stream_file_close(void *file, hid_t dxpl_id, void **req);

/* Group callbacks */
static void  *H5VL_stream_group_create(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                                             hid_t lcpl_id, hid_t gcpl_id, hid_t gapl_id, hid_t dxpl_id,
                                             void **req);
static void  *H5VL_stream_group_open(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                                           hid_t gapl_id, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_group_get(void *obj, H5VL_group_get_args_t *args, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_group_specific(void *obj, H5VL_group_specific_args_t *args, hid_t dxpl_id,
                                               void **req);
static herr_t H5VL_stream_group_optional(void *obj, H5VL_optional_args_t *args, hid_t dxpl_id,
                                               void **req);
static herr_t H5VL_stream_group_close(void *grp, hid_t dxpl_id, void **req);

/* Link callbacks */
static herr_t H5VL_stream_link_create(H5VL_link_create_args_t *args, void *obj,
                                            const H5VL_loc_params_t *loc_params, hid_t lcpl_id, hid_t lapl_id,
                                            hid_t dxpl_id, void **req);
static herr_t H5VL_stream_link_copy(void *src_obj, const H5VL_loc_params_t *loc_params1, void *dst_obj,
                                          const H5VL_loc_params_t *loc_params2, hid_t lcpl_id, hid_t lapl_id,
                                          hid_t dxpl_id, void **req);
static herr_t H5VL_stream_link_move(void *src_obj, const H5VL_loc_params_t *loc_params1, void *dst_obj,
                                          const H5VL_loc_params_t *loc_params2, hid_t lcpl_id, hid_t lapl_id,
                                          hid_t dxpl_id, void **req);
static herr_t H5VL_stream_link_get(void *obj, const H5VL_loc_params_t *loc_params,
                                         H5VL_link_get_args_t *args, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_link_specific(void *obj, const H5VL_loc_params_t *loc_params,
                                              H5VL_link_specific_args_t *args, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_link_optional(void *obj, const H5VL_loc_params_t *loc_params,
                                              H5VL_optional_args_t *args, hid_t dxpl_id, void **req);

/* Object callbacks */
static void  *H5VL_stream_object_open(void *obj, const H5VL_loc_params_t *loc_params,
                                            H5I_type_t *opened_type, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_object_copy(void *src_obj, const H5VL_loc_params_t *src_loc_params,
                                            const char *src_name, void *dst_obj,
                                            const H5VL_loc_params_t *dst_loc_params, const char *dst_name,
                                            hid_t ocpypl_id, hid_t lcpl_id, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_object_get(void *obj, const H5VL_loc_params_t *loc_params,
                                           H5VL_object_get_args_t *args, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_object_specific(void *obj, const H5VL_loc_params_t *loc_params,
                                                H5VL_object_specific_args_t *args, hid_t dxpl_id, void **req);
static herr_t H5VL_stream_object_optional(void *obj, const H5VL_loc_params_t *loc_params,
                                                H5VL_optional_args_t *args, hid_t dxpl_id, void **req);

/* Container/connector introspection callbacks */
static herr_t H5VL_stream_introspect_get_conn_cls(void *obj, H5VL_get_conn_lvl_t lvl,
                                                        const H5VL_class_t **conn_cls);
static herr_t H5VL_stream_introspect_get_cap_flags(const void *info, uint64_t *cap_flags);
static herr_t H5VL_stream_introspect_opt_query(void *obj, H5VL_subclass_t cls, int opt_type,
                                                     uint64_t *flags);

/* Async request callbacks */
static herr_t H5VL_stream_request_wait(void *req, uint64_t timeout, H5VL_request_status_t *status);
static herr_t H5VL_stream_request_notify(void *obj, H5VL_request_notify_t cb, void *ctx);
static herr_t H5VL_stream_request_cancel(void *req, H5VL_request_status_t *status);
static herr_t H5VL_stream_request_specific(void *req, H5VL_request_specific_args_t *args);
static herr_t H5VL_stream_request_optional(void *req, H5VL_optional_args_t *args);
static herr_t H5VL_stream_request_free(void *req);

/* Blob callbacks */
static herr_t H5VL_stream_blob_put(void *obj, const void *buf, size_t size, void *blob_id, void *ctx);
static herr_t H5VL_stream_blob_get(void *obj, const void *blob_id, void *buf, size_t size, void *ctx);
static herr_t H5VL_stream_blob_specific(void *obj, void *blob_id, H5VL_blob_specific_args_t *args);
static herr_t H5VL_stream_blob_optional(void *obj, void *blob_id, H5VL_optional_args_t *args);

/* Token callbacks */
static herr_t H5VL_stream_token_cmp(void *obj, const H5O_token_t *token1, const H5O_token_t *token2,
                                          int *cmp_value);
static herr_t H5VL_stream_token_to_str(void *obj, H5I_type_t obj_type, const H5O_token_t *token,
                                             char **token_str);
static herr_t H5VL_stream_token_from_str(void *obj, H5I_type_t obj_type, const char *token_str,
                                               H5O_token_t *token);

/* Generic optional callback */
static herr_t H5VL_stream_optional(void *obj, H5VL_optional_args_t *args, hid_t dxpl_id, void **req);

/*******************/
/* Local variables */
/*******************/

/* Connector ID, set by H5VL_stream_register() or by the plugin loader. */
hid_t H5VL_STREAM_g = H5I_INVALID_HID;

/* Values assigned by H5VLregister_opt_operation() during init.  H5I_INVALID_HID
 * is not meaningful for an int op value, so -1 marks "not yet registered".
 */
static int H5VL_stream_op_begin_step         = -1;
static int H5VL_stream_op_end_step           = -1;
static int H5VL_stream_op_step_status        = -1;
static int H5VL_stream_op_subscribe          = -1;
static int H5VL_stream_op_begin_logical_step = -1;
static int H5VL_stream_op_get_logical_steps  = -1;
static int H5VL_stream_op_wait_step_ready    = -1;
static int H5VL_stream_op_set_queue_policy   = -1;
static int H5VL_stream_op_get_subscribed_data = -1;

/* Argument structs for the step operations. */
typedef struct H5VL_stream_args_begin_step_t {
    size_t          n_logical;
    const uint64_t *logical_ids;
    uint64_t        wall_time_ns;
} H5VL_stream_args_begin_step_t;

typedef struct H5VL_stream_args_step_status_t {
    H5F_step_status_t *status;
} H5VL_stream_args_step_status_t;

typedef struct H5VL_stream_args_subscribe_t {
    size_t              count;
    const char *const  *paths;
    const hid_t        *spaces;
    const hid_t        *plists;
} H5VL_stream_args_subscribe_t;

/* M3 */
typedef struct H5VL_stream_args_begin_logical_step_t {
    uint64_t logical_id;
} H5VL_stream_args_begin_logical_step_t;

typedef struct H5VL_stream_args_get_logical_steps_t {
    size_t   *n_logical;   /* INOUT, two-call size-then-fill idiom */
    uint64_t *logical_ids; /* OUT; NULL on the size-query call */
} H5VL_stream_args_get_logical_steps_t;

/* M4 */
typedef struct H5VL_stream_args_wait_step_ready_t {
    uint64_t  timeout_ms;
    uint64_t *physical_step; /* OUT */
    uint64_t *wall_time_ns;  /* OUT; NULL if not wanted */
} H5VL_stream_args_wait_step_ready_t;

/* M7 */
typedef struct H5VL_stream_args_set_queue_policy_t {
    H5VL_stream_queue_policy_t policy;
    uint64_t                    reserve_slots;
} H5VL_stream_args_set_queue_policy_t;

/* M8/M8.5 */
typedef struct H5VL_stream_args_get_subscribed_data_t {
    uint64_t  timeout_ms;
    uint64_t *physical_step; /* OUT */
    char    **path;          /* OUT, newly malloc'd */
    void    **buf;           /* OUT, newly malloc'd */
    size_t   *size;          /* OUT */
    uint64_t *elem_start;    /* OUT */
    uint64_t *elem_count;    /* OUT */
} H5VL_stream_args_get_subscribed_data_t;

/*-------------------------------------------------------------------------
 * Default connector info.
 *
 * The pass-through template treats a FAPL with no connector info as an error.
 * That makes the two most natural ways to use a connector fail:
 *
 *   H5Pset_vol(fapl, vol_id, NULL);          -- no info supplied
 *   HDF5_VOL_CONNECTOR=vol-stream            -- no info string
 *
 * The latter is how the M0 exit gate drives HDF5's test/API suite, so
 * defaulting the under-VOL to native is required, not just friendlier.
 *
 * Heap-allocated with a reference taken on the native connector, so the
 * existing H5VL_stream_info_free() call sites need no special case.
 *-------------------------------------------------------------------------
 */
static H5VL_stream_info_t *
H5VL__stream_default_info(void)
{
    H5VL_stream_info_t *info;

    if (NULL == (info = (H5VL_stream_info_t *)calloc(1, sizeof(H5VL_stream_info_t))))
        return NULL;

    info->under_vol_id   = H5VL_NATIVE;
    info->under_vol_info = NULL;

    /* info_free() decrements this */
    if (H5Iinc_ref(info->under_vol_id) < 0) {
        free(info);
        return NULL;
    }

    return info;
} /* end H5VL__stream_default_info() */

/* vol-stream connector class struct */
const H5VL_class_t H5VL_stream_g = {
    H5VL_VERSION,                            /* VOL class struct version */
    (H5VL_class_value_t)H5VL_STREAM_VALUE, /* value        */
    H5VL_STREAM_NAME,                      /* name         */
    H5VL_STREAM_VERSION,                   /* connector version */
    0,                                       /* capability flags */
    H5VL_stream_init,                  /* initialize   */
    H5VL_stream_term,                  /* terminate    */
    {
        /* info_cls */
        sizeof(H5VL_stream_info_t), /* size    */
        H5VL_stream_info_copy,      /* copy    */
        H5VL_stream_info_cmp,       /* compare */
        H5VL_stream_info_free,      /* free    */
        H5VL_stream_info_to_str,    /* to_str  */
        H5VL_stream_str_to_info     /* from_str */
    },
    {
        /* wrap_cls */
        H5VL_stream_get_object,    /* get_object   */
        H5VL_stream_get_wrap_ctx,  /* get_wrap_ctx */
        H5VL_stream_wrap_object,   /* wrap_object  */
        H5VL_stream_unwrap_object, /* unwrap_object */
        H5VL_stream_free_wrap_ctx  /* free_wrap_ctx */
    },
    {
        /* attribute_cls */
        H5VL_stream_attr_create,   /* create */
        H5VL_stream_attr_open,     /* open */
        H5VL_stream_attr_read,     /* read */
        H5VL_stream_attr_write,    /* write */
        H5VL_stream_attr_get,      /* get */
        H5VL_stream_attr_specific, /* specific */
        H5VL_stream_attr_optional, /* optional */
        H5VL_stream_attr_close     /* close */
    },
    {
        /* dataset_cls */
        H5VL_stream_dataset_create,   /* create */
        H5VL_stream_dataset_open,     /* open */
        H5VL_stream_dataset_read,     /* read */
        H5VL_stream_dataset_write,    /* write */
        H5VL_stream_dataset_get,      /* get */
        H5VL_stream_dataset_specific, /* specific */
        H5VL_stream_dataset_optional, /* optional */
        H5VL_stream_dataset_close     /* close */
    },
    {
        /* datatype_cls */
        H5VL_stream_datatype_commit,   /* commit */
        H5VL_stream_datatype_open,     /* open */
        H5VL_stream_datatype_get,      /* get_size */
        H5VL_stream_datatype_specific, /* specific */
        H5VL_stream_datatype_optional, /* optional */
        H5VL_stream_datatype_close     /* close */
    },
    {
        /* file_cls */
        H5VL_stream_file_create,   /* create */
        H5VL_stream_file_open,     /* open */
        H5VL_stream_file_get,      /* get */
        H5VL_stream_file_specific, /* specific */
        H5VL_stream_file_optional, /* optional */
        H5VL_stream_file_close     /* close */
    },
    {
        /* group_cls */
        H5VL_stream_group_create,   /* create */
        H5VL_stream_group_open,     /* open */
        H5VL_stream_group_get,      /* get */
        H5VL_stream_group_specific, /* specific */
        H5VL_stream_group_optional, /* optional */
        H5VL_stream_group_close     /* close */
    },
    {
        /* link_cls */
        H5VL_stream_link_create,   /* create */
        H5VL_stream_link_copy,     /* copy */
        H5VL_stream_link_move,     /* move */
        H5VL_stream_link_get,      /* get */
        H5VL_stream_link_specific, /* specific */
        H5VL_stream_link_optional  /* optional */
    },
    {
        /* object_cls */
        H5VL_stream_object_open,     /* open */
        H5VL_stream_object_copy,     /* copy */
        H5VL_stream_object_get,      /* get */
        H5VL_stream_object_specific, /* specific */
        H5VL_stream_object_optional  /* optional */
    },
    {
        /* introspect_cls */
        H5VL_stream_introspect_get_conn_cls,  /* get_conn_cls */
        H5VL_stream_introspect_get_cap_flags, /* get_cap_flags */
        H5VL_stream_introspect_opt_query,     /* opt_query */
    },
    {
        /* request_cls */
        H5VL_stream_request_wait,     /* wait */
        H5VL_stream_request_notify,   /* notify */
        H5VL_stream_request_cancel,   /* cancel */
        H5VL_stream_request_specific, /* specific */
        H5VL_stream_request_optional, /* optional */
        H5VL_stream_request_free      /* free */
    },
    {
        /* blob_cls */
        H5VL_stream_blob_put,      /* put */
        H5VL_stream_blob_get,      /* get */
        H5VL_stream_blob_specific, /* specific */
        H5VL_stream_blob_optional  /* optional */
    },
    {
        /* token_cls */
        H5VL_stream_token_cmp,     /* cmp */
        H5VL_stream_token_to_str,  /* to_str */
        H5VL_stream_token_from_str /* from_str */
    },
    H5VL_stream_optional /* optional */
};

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_new_obj
 *
 * Purpose:     Create a new pass through object for an underlying object
 *
 * Return:      Success:    Pointer to the new pass through object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static H5VL_stream_t *
H5VL_stream_new_obj(void *under_obj, hid_t under_vol_id)
{
    H5VL_stream_t *new_obj;

    new_obj               = (H5VL_stream_t *)calloc(1, sizeof(H5VL_stream_t));
    new_obj->under_object = under_obj;
    new_obj->under_vol_id = under_vol_id;

    H5Iinc_ref(new_obj->under_vol_id);

    return new_obj;
} /* end H5VL__stream_new_obj() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_free_obj
 *
 * Purpose:     Release a pass through object
 *
 * Note:	Take care to preserve the current HDF5 error stack
 *		when calling HDF5 API calls.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_free_obj(H5VL_stream_t *obj)
{
    hid_t err_id;

    err_id = H5Eget_current_stack();

    H5Idec_ref(obj->under_vol_id);

    H5Eset_current_stack(err_id);

    free(obj->path);
    if (obj->file_state)
        H5VL__stream_file_state_decref(obj->file_state);
    free(obj);

    return 0;
} /* end H5VL__stream_free_obj() */

/*-------------------------------------------------------------------------
 * M2 step-capture helpers
 *
 * Together these give every wrapped object a borrowed, refcounted pointer to
 * its file's step state (H5VL_stream_file_state_t) and its own absolute path
 * from the file root, without changing H5VL_stream_new_obj()'s signature at
 * its ~30 existing call sites -- H5VL__stream_new_child_obj() wraps it.
 *-------------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_file_state_new
 *
 * Purpose:     Allocate step state for a newly created/opened file. Returned
 *              with refcount 1, representing the file wrapper's own
 *              reference; H5VL__stream_new_child_obj() adds one more per
 *              child object opened under the file.
 *
 * Return:      Success:    New, zeroed file state (step_state == 0 ==
 *                           H5F_STEP_NOT_IN_STEP)
 *              Failure:    NULL
 *-------------------------------------------------------------------------
 */
static H5VL_stream_file_state_t *
H5VL__stream_file_state_new(void)
{
    H5VL_stream_file_state_t *fs;

    if (NULL == (fs = (H5VL_stream_file_state_t *)calloc(1, sizeof(H5VL_stream_file_state_t))))
        return NULL;

    fs->refcount = 1;

    return fs;
} /* end H5VL__stream_file_state_new() */

static void
H5VL__stream_file_state_incref(H5VL_stream_file_state_t *fs)
{
    fs->refcount++;
} /* end H5VL__stream_file_state_incref() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_pending_entry_clear
 *
 * Purpose:     Release everything a pending entry owns: its live hid_t
 *              copies (captured but never yet encoded), path, and payload.
 *              Safe to call on an entry whose fields are still zeroed.
 *-------------------------------------------------------------------------
 */
static void
H5VL__stream_pending_entry_clear(H5VL_stream_pending_entry_t *e)
{
    if (e->type_id > 0)
        H5Tclose(e->type_id);
    if (e->space_id > 0)
        H5Sclose(e->space_id);
    if (e->dcpl_id > 0)
        H5Pclose(e->dcpl_id);
    if (e->dapl_id > 0)
        H5Pclose(e->dapl_id);
    free(e->path);
    free(e->payload);
    memset(e, 0, sizeof(*e));
} /* end H5VL__stream_pending_entry_clear() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_pending_discard_all
 *
 * Purpose:     Drop every pending entry without replaying it -- used at
 *              end_step after a successful replay, and at file_close if a
 *              step was left open (an unclosed step was never durably
 *              committed, consistent with the M1 stance that an unclosed
 *              step is a bug in the caller rather than something to rescue).
 *-------------------------------------------------------------------------
 */
static void
H5VL__stream_pending_discard_all(H5VL_stream_file_state_t *fs)
{
    size_t i;

    for (i = 0; i < fs->n_pending; i++)
        H5VL__stream_pending_entry_clear(&fs->pending[i]);

    free(fs->pending);
    fs->pending     = NULL;
    fs->n_pending   = 0;
    fs->cap_pending = 0;
} /* end H5VL__stream_pending_discard_all() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_file_state_decref
 *
 * Purpose:     Drop a reference; frees the pending-entry buffer, the M3
 *              reader indexes, and the struct itself once the last wrapper
 *              (file or child) referring to it closes.
 *-------------------------------------------------------------------------
 */
static void
H5VL__stream_file_state_decref(H5VL_stream_file_state_t *fs)
{
    size_t i;

    if (--fs->refcount > 0)
        return;

    H5VL__stream_pending_discard_all(fs);
    free(fs->logical_ids);

    for (i = 0; i < fs->n_steps_total; i++)
        free(fs->step_logical_ids[i]);
    free(fs->step_logical_ids);
    free(fs->n_logical_per_step);
    free(fs->logical_map);

    for (i = 0; i < fs->n_path_index; i++) {
        free(fs->path_index[i].path);
        free(fs->path_index[i].steps);
    }
    free(fs->path_index);

    H5VL__stream_type_cache_clear(fs->write_type_cache, fs->n_write_type_cache);
    H5VL__stream_type_cache_clear(fs->replay_type_cache, fs->n_replay_type_cache);

    /* M4: an unclosed step at file_close() leaves current_completion set --
     * same "no partial-step state worth preserving" reasoning as an unclosed
     * step's pending buffer above. Any request object still holding a
     * reference keeps the cell alive (it stays pending forever, which is
     * correct: the step it belonged to was never committed) until that
     * request is freed too. */
    if (fs->current_completion)
        H5VL__stream_step_completion_decref(fs->current_completion);

#ifdef VOL_STREAM_HAVE_BAKE
    /* M7: any step still sitting in BAKE at file_close() is simply
     * abandoned, same "no partial-step state worth preserving" reasoning as
     * an unclosed step's pending buffer above -- vs_bake_stop() releases the
     * target itself without needing each region drained first.
     *
     * Must run BEFORE vs_tr_stop() below: the BAKE provider and its abt-io
     * instance are registered on fs->transport's own margo instance, so
     * tearing that instance down (vs_tr_stop() -> margo_finalize()) first
     * destroys the Argobots execution streams BAKE/abt-io still have
     * resources on -- observed directly as an ABT_finalize() assertion
     * failure ("p_xstream_head == NULL") when this was ordered the other
     * way around. */
    for (i = 0; i < fs->n_pending_spill; i++)
        free(fs->pending_spill[i].desc);
    free(fs->pending_spill);
    if (fs->spill_bake)
        vs_bake_stop(fs->spill_bake);
    free(fs->spill_dir);
#endif

#ifdef VOL_STREAM_HAVE_MERCURY
    if (fs->transport)
        vs_tr_stop(fs->transport);
#endif

#ifdef H5_HAVE_PARALLEL
    if (fs->has_comm)
        MPI_Comm_free(&fs->comm);
#endif

    free(fs);
} /* end H5VL__stream_file_state_decref() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_step_completion_new/incref/decref
 *
 * Purpose:     The M4 deferred-request completion cell -- see its typedef's
 *              comment. _new() starts refcount at 1 (the caller's own
 *              reference, typically file_state->current_completion);
 *              request_wait/notify/cancel/free on a request created by
 *              H5VL__stream_make_deferred_request() drop the reference they
 *              took when the request was made available to the application.
 *-------------------------------------------------------------------------
 */
static H5VL_stream_step_completion_t *
H5VL__stream_step_completion_new(void)
{
    H5VL_stream_step_completion_t *c;

    if (NULL == (c = (H5VL_stream_step_completion_t *)malloc(sizeof(*c))))
        return NULL;
    c->refcount    = 1;
    c->status      = 0;
    c->notify_list = NULL;
    return c;
} /* end H5VL__stream_step_completion_new() */

static void
H5VL__stream_step_completion_incref(H5VL_stream_step_completion_t *c)
{
    c->refcount++;
} /* end H5VL__stream_step_completion_incref() */

static void
H5VL__stream_step_completion_decref(H5VL_stream_step_completion_t *c)
{
    if (--c->refcount == 0) {
        H5VL_stream_step_notify_t *n = c->notify_list;

        while (n) {
            H5VL_stream_step_notify_t *next = n->next;

            free(n);
            n = next;
        }
        free(c);
    }
} /* end H5VL__stream_step_completion_decref() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_make_deferred_request
 *
 * Purpose:     Populate *req with a request object for a dataset/attribute
 *              write just captured into fs's pending buffer. Lazily creates
 *              fs->current_completion if this is the first deferred request
 *              of the open step (begin_step() does not allocate one
 *              up front, since most steps never have req != NULL -- ordinary
 *              H5Dwrite()/H5Awrite() pass req == NULL).
 *
 *              Does nothing when req is NULL, which is the common case and
 *              not an error: a connector may always complete an operation
 *              synchronously and report no request, and the payload is
 *              already safely copied into the pending entry by the time this
 *              is called either way. What the returned request tracks is
 *              durability -- whether end_step()'s replay has landed this
 *              entry in the underlying file -- not buffer safety.
 *
 * Return:      Success:    0 (including the req == NULL no-op case)
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_make_deferred_request(H5VL_stream_file_state_t *fs, void **req)
{
    H5VL_stream_t *r;

    if (!req)
        return 0;

    if (!fs->current_completion && NULL == (fs->current_completion = H5VL__stream_step_completion_new()))
        return -1;

    if (NULL == (r = (H5VL_stream_t *)calloc(1, sizeof(*r))))
        return -1;

    H5VL__stream_step_completion_incref(fs->current_completion);
    r->obj_state           = H5VL_STREAM_OBJ_DEFERRED_REQUEST;
    r->deferred_completion = fs->current_completion;
    /* under_vol_id/under_object are deliberately left at their calloc()
     * default (0/NULL): this object is never passed to H5VLrequest_*() on
     * an underlying connector, only handled directly by obj_state ==
     * H5VL_STREAM_OBJ_DEFERRED_REQUEST branches below, and it must NOT be
     * freed via H5VL_stream_free_obj() -- that calls H5Idec_ref() on
     * under_vol_id, which we never incremented. See
     * H5VL__stream_deferred_request_free(). */

    *req = r;
    return 0;
} /* end H5VL__stream_make_deferred_request() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_deferred_request_free
 *
 * Purpose:     Release a request object made by
 *              H5VL__stream_make_deferred_request() -- drops its reference
 *              on the shared completion cell, freeing it if this was the
 *              last one. Deliberately not H5VL_stream_free_obj(): that
 *              function calls H5Idec_ref() on under_vol_id, a reference this
 *              request object never took.
 *-------------------------------------------------------------------------
 */
static void
H5VL__stream_deferred_request_free(H5VL_stream_t *r)
{
    H5VL__stream_step_completion_decref(r->deferred_completion);
    free(r);
} /* end H5VL__stream_deferred_request_free() */

#ifdef VOL_STREAM_HAVE_MERCURY
/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_transport_na
 *
 * Purpose:     The Mercury NA plugin string to start the transport with, or
 *              NULL if the transport is not requested for this process.
 *              VOL_STREAM_NA opts in explicitly (default: unset, transport
 *              off) rather than a connector-info flag, so M0-M3's info
 *              struct (and its info_to_str/str_to_info/info_copy/info_cmp
 *              callbacks) need no change -- see docs/dev-plan.md's M4
 *              section on this choice.
 *-------------------------------------------------------------------------
 */
static const char *
H5VL__stream_transport_na(void)
{
    return getenv("VOL_STREAM_NA");
} /* end H5VL__stream_transport_na() */

#ifdef VOL_STREAM_HAVE_BAKE
/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_spill_dir
 *
 * Purpose:     M7: where a writer's BAKE-backed Spill target file lives.
 *              VOL_STREAM_SPILL_DIR opts in explicitly, same convention as
 *              VOL_STREAM_NA above; falls back to /tmp (node-local on any
 *              normal HPC node, the whole point of Spill) rather than
 *              failing, since the connector cannot know a better default
 *              and the fallback still satisfies "node-local".
 *-------------------------------------------------------------------------
 */
static const char *
H5VL__stream_spill_dir(void)
{
    const char *dir = getenv("VOL_STREAM_SPILL_DIR");

    return dir ? dir : "/tmp";
} /* end H5VL__stream_spill_dir() */
#endif

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_ssg_group_path
 *
 * Purpose:     Path of the sidecar file a writer stores its SSG group id
 *              to (via vs_tr_writer_start_group()), and a reader loads it
 *              from (via vs_tr_reader_join_group()): "<filename>.vsgroup".
 *              Caller frees the returned string.
 *-------------------------------------------------------------------------
 */
static char *
H5VL__stream_ssg_group_path(const char *filename)
{
    char  *path;
    size_t len = strlen(filename) + strlen(".vsgroup") + 1;

    if (NULL == (path = (char *)malloc(len)))
        return NULL;
    snprintf(path, len, "%s.vsgroup", filename);
    return path;
} /* end H5VL__stream_ssg_group_path() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_transport_start_writer
 *
 * Purpose:     If VOL_STREAM_NA is set, start the transport for a writer:
 *              start Margo, create this process's single-member SSG group,
 *              and store its id to name's sidecar file so a reader can join
 *              it. Not an error for VOL_STREAM_NA to be unset (fs->transport
 *              stays NULL, matching M0-M3 exactly) or for Margo/SSG/the
 *              sidecar write to fail -- a writer proceeding with no readers
 *              listening is exactly the M5 exit gate this is building
 *              toward, so a transport that never comes up must not fail
 *              file_create()/file_open().
 *
 *              Flushes the underlying file before publishing the sidecar:
 *              a reader that opens the moment the sidecar appears must see
 *              a durable superblock, not race H5Fcreate()'s own buffered
 *              writes -- "file signature not found" otherwise.
 *-------------------------------------------------------------------------
 */
static void
H5VL__stream_transport_start_writer(H5VL_stream_file_state_t *fs, const char *name)
{
    const char *na_str = H5VL__stream_transport_na();
    char       *group_file;

    if (!na_str)
        return;
    if (NULL == (fs->transport = vs_tr_start(na_str)))
        return;

    /* M8.5 precision: register once, right after start -- see vs_tr_
     * set_refilter_cb()'s comment. */
    vs_tr_set_refilter_cb(fs->transport, H5VL__stream_refilter_for_subscriber);

    {
        H5VL_file_specific_args_t flush_args;

        flush_args.op_type            = H5VL_FILE_FLUSH;
        flush_args.args.flush.obj_type = H5I_FILE;
        flush_args.args.flush.scope    = H5F_SCOPE_GLOBAL;
        H5VLfile_specific(fs->file_under_object, fs->file_under_vol_id, &flush_args,
                           H5P_DATASET_XFER_DEFAULT, NULL);
    }

    if (NULL != (group_file = H5VL__stream_ssg_group_path(name))) {
        vs_tr_writer_start_group(fs->transport, group_file);
        free(group_file);
    }
} /* end H5VL__stream_transport_start_writer() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_transport_start_reader
 *
 * Purpose:     If VOL_STREAM_NA is set, start the transport for a reader:
 *              start Margo, then look for name's SSG group-id sidecar file
 *              and join the group there. The writer may not have created it
 *              yet (file_open() racing file_create() across two processes
 *              is the whole point of M4's exit gate), so this retries for
 *              up to 5 seconds rather than failing on the first miss --
 *              still bounded, so a writer that never shows up does not hang
 *              file_open() forever. Joining the group is what gives this
 *              reader a "coherent view" as of whatever step the writer has
 *              already committed -- see vs_tr_reader_join_group()'s comment.
 *-------------------------------------------------------------------------
 */
static void
H5VL__stream_transport_start_reader(H5VL_stream_file_state_t *fs, const char *name)
{
    const char *na_str = H5VL__stream_transport_na();
    char       *group_file;
    int         attempt;

    if (!na_str)
        return;
    if (NULL == (fs->transport = vs_tr_start(na_str)))
        return;
    if (NULL == (group_file = H5VL__stream_ssg_group_path(name)))
        return;

    for (attempt = 0; attempt < 50; attempt++) {
        FILE *f = fopen(group_file, "r");

        if (f) {
            fclose(f);
            vs_tr_reader_join_group(fs->transport, group_file);
            break;
        }
        usleep(100000); /* 100ms */
    }
    free(group_file);
} /* end H5VL__stream_transport_start_reader() */
#endif /* VOL_STREAM_HAVE_MERCURY */

#ifdef H5_HAVE_PARALLEL
/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_detect_mpi_comm
 *
 * Purpose:     If fapl_id carries the MPI-IO VFD (H5Pset_fapl_mpio() was
 *              called on it, or an ancestor FAPL it was copied from), pull
 *              out the communicator and record it on fs, making this file's
 *              step boundaries collective -- see H5VL_stream_file_optional()'s
 *              begin_step/end_step handling. Not an error for the fapl to
 *              carry no MPI-IO VFD at all: that is the ordinary serial case,
 *              and fs->has_comm simply stays 0 (its calloc() default),
 *              matching M0-M5 behavior exactly.
 *-------------------------------------------------------------------------
 */
static void
H5VL__stream_detect_mpi_comm(H5VL_stream_file_state_t *fs, hid_t fapl_id)
{
    MPI_Comm    comm;
    MPI_Info    info;
    H5E_auto2_t old_func;
    void       *old_data;
    hid_t       err_id;
    herr_t      r;

    /* H5Pget_fapl_mpio() errors on a fapl that does not use the MPI-IO
     * VFD -- the ordinary case -- so probe quietly rather than assume,
     * same idiom as H5VL__stream_reader_build_index()'s missing-/step
     * probe. */
    H5Eget_auto2(H5E_DEFAULT, &old_func, &old_data);
    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
    err_id = H5Eget_current_stack();
    r = H5Pget_fapl_mpio(fapl_id, &comm, &info);
    H5Eset_current_stack(err_id);
    H5Eset_auto2(H5E_DEFAULT, old_func, old_data);

    if (r < 0)
        return;

    /* H5Pget_fapl_mpio() already hands back a fresh duplicate of the
     * stored communicator (its own doc comment says so) -- ours to keep
     * and free in H5VL__stream_file_state_decref(), no extra
     * MPI_Comm_dup() needed. */
    if (info != MPI_INFO_NULL)
        MPI_Info_free(&info);

    fs->comm     = comm;
    fs->has_comm = 1;
    MPI_Comm_rank(fs->comm, &fs->mpi_rank);
    MPI_Comm_size(fs->comm, &fs->mpi_size);
} /* end H5VL__stream_detect_mpi_comm() */
#endif /* H5_HAVE_PARALLEL */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_child_path
 *
 * Purpose:     Build an absolute path for a child of parent_path (the file
 *              root's own path is ""). Returns NULL (propagating
 *              "unresolvable") if parent_path is NULL.
 *
 *              M3: strips any leading '/' from name first. Without this, a
 *              path built incrementally (H5Gcreate2(fid,"grp",...) then
 *              H5Dcreate2(grp,"sub",...), captured as "/grp/sub") and the
 *              same object opened in one shot the way a reader naturally
 *              would (H5Dopen2(fid, "/grp/sub", ...)) produce two different
 *              strings -- child_path("", "/grp/sub") would otherwise yield
 *              "//grp/sub", not matching the index key "/grp/sub" real data
 *              was captured under. Both write-time capture and read-time
 *              lookup go through this one helper, so normalizing here
 *              guarantees they agree by construction.
 *
 * Return:      Success:    malloc'd path the caller owns
 *              Failure:    NULL
 *-------------------------------------------------------------------------
 */
static char *
H5VL__stream_child_path(const char *parent_path, const char *name)
{
    char  *path;
    size_t len;

    if (!parent_path || !name)
        return NULL;

    while (*name == '/')
        name++;

    len  = strlen(parent_path) + 1 /* '/' */ + strlen(name) + 1 /* '\0' */;
    if (NULL == (path = (char *)malloc(len)))
        return NULL;

    snprintf(path, len, "%s/%s", parent_path, name);

    return path;
} /* end H5VL__stream_child_path() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_attr_path
 *
 * Purpose:     Build the manifest path for an attribute: parent_path + '@'
 *              + name (e.g. "/data@units"), separating the attribute
 *              namespace from the object namespace so it can never collide
 *              with a dataset/group path. The one place this convention is
 *              defined; both write-time capture (H5VL_stream_attr_create())
 *              and M3 read-time lookup (H5VL__stream_reader_open_attr())
 *              call this instead of constructing it themselves.
 *
 * Return:      Success:    malloc'd path the caller owns
 *              Failure:    NULL
 *-------------------------------------------------------------------------
 */
static char *
H5VL__stream_attr_path(const char *parent_path, const char *name)
{
    char  *path;
    size_t len;

    if (!parent_path || !name)
        return NULL;

    len = strlen(parent_path) + 1 /* '@' */ + strlen(name) + 1 /* '\0' */;
    if (NULL == (path = (char *)malloc(len)))
        return NULL;

    snprintf(path, len, "%s@%s", parent_path, name);

    return path;
} /* end H5VL__stream_attr_path() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_new_child_obj
 *
 * Purpose:     H5VL_stream_new_obj(), plus the M2 path/file_state bookkeeping
 *              a child object (dataset, attribute, group, datatype) needs.
 *              file_state may be NULL (an untraceable parent, e.g. the result
 *              of wrap_object()); parent_path may be NULL (an unresolvable
 *              creation entry point, e.g. BY_NAME/BY_IDX/BY_TOKEN) -- either
 *              way the child is simply left without that piece of
 *              bookkeeping rather than failing, so a non-BY_SELF create still
 *              passes through correctly, just uncaptured.
 *
 * Return:      Success:    Pointer to the new wrapper
 *              Failure:    NULL
 *-------------------------------------------------------------------------
 */
static H5VL_stream_t *
H5VL__stream_new_child_obj(void *under_obj, hid_t under_vol_id, H5VL_stream_file_state_t *file_state,
                            const char *parent_path, const char *name)
{
    H5VL_stream_t *new_obj;

    if (NULL == (new_obj = H5VL_stream_new_obj(under_obj, under_vol_id)))
        return NULL;

    if (file_state) {
        new_obj->file_state = file_state;
        H5VL__stream_file_state_incref(file_state);
    }
    new_obj->path = H5VL__stream_child_path(parent_path, name);

    return new_obj;
} /* end H5VL__stream_new_child_obj() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_pending_append
 *
 * Purpose:     Append a pending entry (by value -- the caller hands off
 *              ownership of entry->path/payload/hid_t copies) to a file's
 *              step buffer, growing it as needed.
 *
 * Return:      Success:    Index of the newly appended entry
 *              Failure:    (size_t)-1
 *-------------------------------------------------------------------------
 */
static size_t
H5VL__stream_pending_append(H5VL_stream_file_state_t *fs, const H5VL_stream_pending_entry_t *entry)
{
    if (fs->n_pending == fs->cap_pending) {
        size_t                        new_cap = fs->cap_pending ? fs->cap_pending * 2 : 8;
        H5VL_stream_pending_entry_t *grown;

        if (NULL == (grown = (H5VL_stream_pending_entry_t *)realloc(
                         fs->pending, new_cap * sizeof(H5VL_stream_pending_entry_t))))
            return (size_t)-1;

        fs->pending     = grown;
        fs->cap_pending = new_cap;
    }

    fs->pending[fs->n_pending] = *entry;

    return fs->n_pending++;
} /* end H5VL__stream_pending_append() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_resolve_space
 *
 * Purpose:     H5S_ALL (0) is a request to use "the obvious dataspace", not a
 *              real dataspace id -- H5Sencode2() on it fails outright. Every
 *              capture path reading a file_space_id/mem_space_id argument
 *              must resolve it through this first.
 *-------------------------------------------------------------------------
 */
static hid_t
H5VL__stream_resolve_space(hid_t space_id, hid_t fallback_space_id)
{
    return (space_id == H5S_ALL) ? fallback_space_id : space_id;
} /* end H5VL__stream_resolve_space() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_type_unsafe_to_capture_1
 *
 * Purpose:     Recursive worker for H5VL__stream_type_unsafe_to_capture().
 *              top_level distinguishes type_id itself from a type_id
 *              reached by recursing into a compound member or array base
 *              type -- see that function's comment for why a top-level
 *              H5T_REFERENCE is exempted here but a nested one is not.
 *
 * Return:      Success:    TRUE/FALSE -- whether type_id (or a member of it,
 *                          for compound/array types) is unsafe to capture
 *              Failure:    -1 (an H5T call itself failed)
 *-------------------------------------------------------------------------
 */
static htri_t
H5VL__stream_type_unsafe_to_capture_1(hid_t type_id, bool top_level)
{
    H5T_class_t cls = H5Tget_class(type_id);

    if (cls == H5T_NO_CLASS)
        return -1;
    if (cls == H5T_VLEN)
        return 1;
    if (cls == H5T_REFERENCE)
        return top_level ? 0 : 1;
    if (cls == H5T_STRING) {
        htri_t is_vl = H5Tis_variable_str(type_id);
        return is_vl;
    }
    if (cls == H5T_COMPOUND) {
        int nmembers = H5Tget_nmembers(type_id);
        int m;

        if (nmembers < 0)
            return -1;
        for (m = 0; m < nmembers; m++) {
            hid_t  member_type = H5Tget_member_type(type_id, m);
            htri_t unsafe;

            if (member_type < 0)
                return -1;
            unsafe = H5VL__stream_type_unsafe_to_capture_1(member_type, false);
            H5Tclose(member_type);
            if (unsafe != 0)
                return unsafe;
        }
        return 0;
    }
    if (cls == H5T_ARRAY) {
        hid_t base_type = H5Tget_super(type_id);
        htri_t unsafe;

        if (base_type < 0)
            return -1;
        unsafe = H5VL__stream_type_unsafe_to_capture_1(base_type, false);
        H5Tclose(base_type);
        return unsafe;
    }
    return 0;
} /* end H5VL__stream_type_unsafe_to_capture_1() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_type_unsafe_to_capture
 *
 * Purpose:     A DsetWrite/Attr capture memcpy()s whatever buffer the
 *              caller handed H5Dwrite()/H5Awrite() straight into the
 *              pending entry's payload, replayed later via a *different*
 *              H5VLdataset_write()/H5VLattr_write() call once end_step()
 *              runs. That is only correct for buffers whose bytes are
 *              self-contained. Variable-length types (H5T_VLEN, and
 *              variable-length strings, H5Tis_variable_str()) violate that:
 *              the buffer is an array of {len, pointer} structs whose
 *              pointers are only valid in the caller's own process memory,
 *              not portable bytes -- capturing them verbatim and replaying
 *              later reads through a pointer that may already be stale (the
 *              caller is free to reclaim it, H5Treclaim(), the moment
 *              H5Dwrite() returns).
 *
 *              H5T_REFERENCE at the TOP level (a plain reference or array of
 *              references, not one buried inside a compound) is NOT rejected
 *              here, unlike earlier in this project's history. Measured
 *              directly (three probe programs, not inferred): a new-style
 *              H5R_ref_t created against an object in *this same file*
 *              (H5R_OBJECT2/H5R_DATASET_REGION2/H5R_ATTR -- the only kind
 *              reachable through this connector's own API, since
 *              H5Rcreate_object() takes a loc_id and constructs a reference
 *              relative to loc_id's own file; there is no way to name a
 *              genuinely foreign file without a loc_id in THAT file, which
 *              would not route through this connector's object_specific()
 *              at all) carries a NULL cached filename pointer -- inspected
 *              directly at the byte level against H5Rpkg.h's private
 *              H5R_ref_priv_obj_t layout. There is no heap pointer to
 *              dangle, so an ordinary memcpy is exactly as safe as for any
 *              other fixed-size scalar type, and end-to-end replay of a
 *              raw-captured reference dataset was verified to resolve to
 *              the correct target. A prior attempt at a separate
 *              name-translation capture path (call H5Rget_obj_name() from
 *              inside dataset_write, or cache token -> logical path at
 *              H5Rcreate_object() time and translate at capture) is not
 *              what is running here -- both were built, both hit the same
 *              crash during end_step()'s replay, and both were abandoned;
 *              see docs/dev-plan.md's reference-support section for the
 *              full account. A reference nested inside a compound or array
 *              still IS rejected: translating one at some byte offset inside
 *              a struct would mean rewriting the buffer member by member,
 *              which nothing here does.
 *
 * Return:      Success:    TRUE/FALSE -- whether type_id (or a member of it,
 *                          for compound/array types) is unsafe to capture
 *              Failure:    -1 (an H5T call itself failed)
 *-------------------------------------------------------------------------
 */
static htri_t
H5VL__stream_type_unsafe_to_capture(hid_t type_id)
{
    return H5VL__stream_type_unsafe_to_capture_1(type_id, true);
} /* end H5VL__stream_type_unsafe_to_capture() */

/*-------------------------------------------------------------------------
 * H5Tencode/H5Sencode2/H5Pencode2 all use a two-call size-then-fill idiom,
 * with slightly different signatures (H5Tencode takes no fapl; H5Sencode2
 * and H5Pencode2 do). Wrapping each lets H5VL__stream_replay_step() below
 * read as the entry-by-entry replay logic it is.
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_encode_type(hid_t type_id, uint8_t **buf, size_t *len)
{
    size_t nalloc = 0;

    if (H5Tencode(type_id, NULL, &nalloc) < 0)
        return -1;
    if (NULL == (*buf = (uint8_t *)malloc(nalloc)))
        return -1;
    if (H5Tencode(type_id, *buf, &nalloc) < 0) {
        free(*buf);
        *buf = NULL;
        return -1;
    }
    *len = nalloc;
    return 0;
} /* end H5VL__stream_encode_type() */

static herr_t
H5VL__stream_encode_space(hid_t space_id, uint8_t **buf, size_t *len)
{
    size_t nalloc = 0;

    if (H5Sencode2(space_id, NULL, &nalloc, H5P_DEFAULT) < 0)
        return -1;
    if (NULL == (*buf = (uint8_t *)malloc(nalloc)))
        return -1;
    if (H5Sencode2(space_id, *buf, &nalloc, H5P_DEFAULT) < 0) {
        free(*buf);
        *buf = NULL;
        return -1;
    }
    *len = nalloc;
    return 0;
} /* end H5VL__stream_encode_space() */

static herr_t
H5VL__stream_encode_dcpl(hid_t dcpl_id, uint8_t **buf, size_t *len)
{
    size_t nalloc = 0;

    if (H5Pencode2(dcpl_id, NULL, &nalloc, H5P_DEFAULT) < 0)
        return -1;
    if (NULL == (*buf = (uint8_t *)malloc(nalloc)))
        return -1;
    if (H5Pencode2(dcpl_id, *buf, &nalloc, H5P_DEFAULT) < 0) {
        free(*buf);
        *buf = NULL;
        return -1;
    }
    *len = nalloc;
    return 0;
} /* end H5VL__stream_encode_dcpl() */

#ifdef VOL_STREAM_HAVE_MERCURY
/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_refilter_zero_copy
 *
 * Purpose:     M8.5.1, the chunk-level fast path behind
 *              H5VL__stream_refilter_for_subscriber(). Serve a subscriber
 *              straight from the already-filtered chunk that replay just
 *              wrote, instead of rebuilding an identical throwaway dataset
 *              and re-running the same filter pipeline over the same bytes
 *              to arrive at the same answer. This is dev-plan.md's
 *              "FilteredChunks" idea applied where it actually pays off
 *              first: the writer already holds a real, chunked, filtered
 *              dataset by this point in replay, so no new capture machinery
 *              is needed to reach it.
 *
 *              Only taken when it is provably equivalent to the slow path.
 *              All of these must hold, and any one failing simply declines:
 *
 *              - The subscriber's DCPL and the dataset's own write-time DCPL
 *                have identical filter pipelines (H5Pequal on the decoded
 *                property lists -- which compares chunking too, so a
 *                subscriber asking for a different chunk shape correctly
 *                falls through rather than being silently served the
 *                dataset's shape instead).
 *              - The push covers the whole dataset, starting at element 0.
 *                A partial overlap would need the chunk grid walked and
 *                per-chunk intersection computed; dev-plan.md's own rule is
 *                that partial writes degrade to Raw, and that is what
 *                declining here does.
 *              - The extent is exactly one chunk. The slow path always
 *                produces a single whole-extent chunk, and the receiving
 *                side (H5VL__stream_unfilter_pushed_data()) assumes exactly
 *                that, so serving multiple chunks would need a wire-format
 *                change this increment deliberately does not make.
 *
 *              raw_buf/elem_size are used only to confirm the push really
 *              is the whole dataset; the bytes sent come from the dataset.
 *
 * Return:      0 if served zero-copy (*out_buf malloc'd, caller frees),
 *              -1 to decline -- never fatal, the caller falls back.
 *-------------------------------------------------------------------------
 */
static int
H5VL__stream_refilter_zero_copy(const void *raw_buf, uint64_t elem_size, uint64_t count,
                                const uint8_t *dcpl_enc, uint64_t dcpl_enc_len,
                                const uint8_t *native_dcpl_enc, uint64_t native_dcpl_enc_len,
                                void *native_ctx, void **out_buf, uint64_t *out_len,
                                uint32_t *out_filter_mask)
{
    H5VL_stream_native_chunk_ctx_t *ctx  = (H5VL_stream_native_chunk_ctx_t *)native_ctx;
    hid_t                           sub  = H5I_INVALID_HID, nat = H5I_INVALID_HID;
    hsize_t                         chunk_dims[H5S_MAX_RANK];
    hsize_t                         chunk_offset[H5S_MAX_RANK];
    void                           *filtered = NULL;
    unsigned                        mask     = 0;
    haddr_t                         addr;
    hsize_t                         chunk_size = 0;
    int                             rank, r;
    int                             ret_value = -1;

    (void)raw_buf;

    if (!ctx || !ctx->under_dset)
        return -1;
    /* Whole-dataset pushes only -- see the partial-overlap note above. */
    if (ctx->write_start != 0 || ctx->total_elems != count || elem_size == 0)
        return -1;

    if ((sub = H5Pdecode(dcpl_enc)) < 0)
        goto done;
    if ((nat = H5Pdecode(native_dcpl_enc)) < 0)
        goto done;

    /* The equivalence test. H5Pequal covers the filter pipeline *and* the
     * chunk dimensions, which is exactly the pair that has to match for the
     * dataset's own stored bytes to be what this subscriber asked for. */
    if (H5Pequal(sub, nat) <= 0)
        goto done;

    if (H5Pget_layout(nat) != H5D_CHUNKED)
        goto done;
    if ((rank = H5Pget_chunk(nat, H5S_MAX_RANK, chunk_dims)) < 0)
        goto done;

    /* Single-chunk extents only, matching what the receiving side expects. */
    {
        hsize_t chunk_elems = 1;

        for (r = 0; r < rank; r++) {
            chunk_elems *= chunk_dims[r];
            chunk_offset[r] = 0;
        }
        if (chunk_elems != (hsize_t)count)
            goto done;
    }

    /* Native-connector optional ops rather than H5Dget_chunk_info_by_coord()/
     * H5Dread_chunk2(): those need an hid_t, and the only way to get one for
     * an under-VOL object hands back an owning reference (see the ctx
     * struct's comment). These take the object directly. A non-native under
     * connector simply fails the call and the caller falls back. */
    {
        H5VL_optional_args_t                args;
        H5VL_native_dataset_optional_args_t dset_args;

        memset(&dset_args, 0, sizeof(dset_args));
        dset_args.get_chunk_info_by_coord.offset      = chunk_offset;
        dset_args.get_chunk_info_by_coord.filter_mask = &mask;
        dset_args.get_chunk_info_by_coord.addr        = &addr;
        dset_args.get_chunk_info_by_coord.size        = &chunk_size;

        args.op_type = H5VL_NATIVE_DATASET_GET_CHUNK_INFO_BY_COORD;
        args.args    = &dset_args;

        if (H5VLdataset_optional(ctx->under_dset, ctx->under_vol_id, &args, H5P_DATASET_XFER_DEFAULT,
                                 NULL) < 0)
            goto done;
    }
    if (chunk_size == 0)
        goto done;
    if (NULL == (filtered = malloc((size_t)chunk_size)))
        goto done;
    {
        H5VL_optional_args_t                args;
        H5VL_native_dataset_optional_args_t dset_args;
        size_t                              buf_size = (size_t)chunk_size;

        memset(&dset_args, 0, sizeof(dset_args));
        dset_args.chunk_read.offset   = chunk_offset;
        dset_args.chunk_read.filters  = (uint32_t)mask;
        dset_args.chunk_read.buf      = filtered;
        dset_args.chunk_read.buf_size = &buf_size;

        args.op_type = H5VL_NATIVE_DATASET_CHUNK_READ;
        args.args    = &dset_args;

        if (H5VLdataset_optional(ctx->under_dset, ctx->under_vol_id, &args, H5P_DATASET_XFER_DEFAULT,
                                 NULL) < 0)
            goto done;
    }

    /* Same diagnostic the slow path emits, so a test can tell the two apart
     * (and confirm the fast path is the one that ran) rather than only
     * seeing that some re-filtering happened. */
    if (getenv("VOL_STREAM_DEBUG_REFILTER"))
        fprintf(stderr, "  refilter  raw=%llu filtered=%llu bytes (zero-copy)\n",
                (unsigned long long)(elem_size * count), (unsigned long long)chunk_size);

    *out_buf         = filtered;
    *out_len         = (uint64_t)chunk_size;
    *out_filter_mask = (uint32_t)mask;
    filtered         = NULL;
    ret_value        = 0;

done:
    free(filtered);
    if (sub >= 0)
        H5Pclose(sub);
    if (nat >= 0)
        H5Pclose(nat);
    return ret_value;
} /* end H5VL__stream_refilter_zero_copy() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_refilter_for_subscriber
 *
 * Purpose:     M8.5 precision: vs_tr_refilter_fn's implementation (see that
 *              typedef's comment in tr_mercury.h) -- run raw_buf through a
 *              subscriber's own requested filter pipeline before it goes on
 *              the wire. HDF5 has no public "apply this filter pipeline to
 *              a buffer" call outside ordinary dataset I/O, so this builds
 *              a throwaway in-memory dataset (H5FD_CORE, backing_store =
 *              false -- nothing ever touches disk) using the subscriber's
 *              own decoded DCPL, writes raw_buf into it (H5Dwrite() applies
 *              the filters), then pulls the resulting *filtered* bytes back
 *              out directly via H5Dread_chunk2() (bypasses the chunk cache
 *              and decompression -- the whole point).
 *
 *              Always forces a single chunk spanning the whole extent,
 *              overriding whatever chunk shape the subscriber's own DCPL
 *              requested, so exactly one H5Dget_chunk_info_by_coord()/
 *              H5Dread_chunk2() pair always suffices -- a real limitation
 *              (the subscriber's own chunk-size preference is not
 *              honored), acceptable for this first increment given the
 *              alternative is iterating an arbitrary number of chunks.
 *              H5VL__stream_unfilter_pushed_data() undoes this on the
 *              receiving end.
 *
 * Return:      0 on success, -1 to fall back to sending this subscriber
 *              raw, unfiltered bytes instead (not a fatal error to the
 *              caller -- see vs_tr_refilter_fn's own comment).
 *-------------------------------------------------------------------------
 */
static int
H5VL__stream_refilter_for_subscriber(const void *raw_buf, uint64_t elem_size, uint64_t count,
                                       const uint8_t *dcpl_enc, uint64_t dcpl_enc_len, const uint8_t *type_enc,
                                       uint64_t type_enc_len, const uint8_t *native_dcpl_enc,
                                       uint64_t native_dcpl_enc_len, void *native_ctx, void **out_buf,
                                       uint64_t *out_len, uint32_t *out_filter_mask)
{
    hid_t    dcpl = -1, type = -1, space = -1, fapl = -1, fid = -1, ds = -1;
    hsize_t  dims[1];
    hsize_t  chunk_offset[1] = {0};
    void    *filtered = NULL;
    unsigned chunk_filter_mask = 0;
    haddr_t  addr;
    hsize_t  chunk_size = 0;
    int      ret_value = -1;

    if (count == 0 || !dcpl_enc || dcpl_enc_len == 0 || !type_enc || type_enc_len == 0)
        return -1;

    /* M8.5.1 zero-copy fast path: when this subscriber's requested pipeline
     * is the one the data was *already* written under, the compressed bytes
     * it wants exist verbatim in the real dataset that replay just wrote --
     * so read them straight out instead of rebuilding an identical throwaway
     * dataset and re-running the same filters over the same input to get the
     * same answer. Declining here is always safe: everything below still
     * produces a correct result, just more expensively. */
    if (native_ctx && native_dcpl_enc && native_dcpl_enc_len > 0 &&
        0 == H5VL__stream_refilter_zero_copy(raw_buf, elem_size, count, dcpl_enc, dcpl_enc_len,
                                             native_dcpl_enc, native_dcpl_enc_len, native_ctx, out_buf,
                                             out_len, out_filter_mask))
        return 0;

    if ((dcpl = H5Pdecode(dcpl_enc)) < 0)
        goto done;
    if ((type = H5Tdecode2(type_enc, (size_t)type_enc_len)) < 0)
        goto done;

    dims[0] = (hsize_t)count;
    if ((space = H5Screate_simple(1, dims, NULL)) < 0)
        goto done;
    if (H5Pset_chunk(dcpl, 1, dims) < 0)
        goto done;

    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_fapl_core(fapl, 1 << 20, false) < 0)
        goto done;
    if ((fid = H5Fcreate("vol_stream_refilter_tmp.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0)
        goto done;
    if ((ds = H5Dcreate2(fid, "d", type, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0)
        goto done;
    if (H5Dwrite(ds, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, raw_buf) < 0)
        goto done;
    if (H5Dflush(ds) < 0)
        goto done;

    if (H5Dget_chunk_info_by_coord(ds, chunk_offset, &chunk_filter_mask, &addr, &chunk_size) < 0)
        goto done;
    if (chunk_size == 0)
        goto done;
    if (NULL == (filtered = malloc((size_t)chunk_size)))
        goto done;
    {
        size_t buf_size = (size_t)chunk_size;
        uint32_t filters32 = (uint32_t)chunk_filter_mask;

        if (H5Dread_chunk2(ds, H5P_DEFAULT, chunk_offset, &filters32, filtered, &buf_size) < 0)
            goto done;
        chunk_filter_mask = (unsigned)filters32;
    }

    /* Diagnostic only, gated the same opt-in-env-var way as VOL_STREAM_
     * DEBUG_PMI_ENV (test/t_parallel.c): real, observable evidence a test
     * can check that precision re-filtering actually ran and actually
     * changed the wire size, since H5Fget_subscribed_data() itself always
     * hands back decoded values regardless -- transparent by design, so
     * there is no other way for a caller to see this happened. */
    if (getenv("VOL_STREAM_DEBUG_REFILTER"))
        fprintf(stderr, "  refilter  raw=%llu filtered=%llu bytes\n", (unsigned long long)(elem_size * count),
                (unsigned long long)chunk_size);

    *out_buf         = filtered;
    *out_len         = (uint64_t)chunk_size;
    *out_filter_mask = (uint32_t)chunk_filter_mask;
    filtered         = NULL;
    ret_value        = 0;

done:
    free(filtered);
    if (ds >= 0)
        H5Dclose(ds);
    if (fid >= 0)
        H5Fclose(fid);
    if (fapl >= 0)
        H5Pclose(fapl);
    if (space >= 0)
        H5Sclose(space);
    if (type >= 0)
        H5Tclose(type);
    if (dcpl >= 0)
        H5Pclose(dcpl);
    return ret_value;
} /* end H5VL__stream_refilter_for_subscriber() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_unfilter_pushed_data
 *
 * Purpose:     M8.5 precision: the reverse of H5VL__stream_refilter_for_
 *              subscriber() -- given the filtered bytes a push carried plus
 *              the dcpl_enc/type_enc/filter_mask it arrived with, reconstruct
 *              the decoded values a caller of H5Fget_subscribed_data()
 *              actually wants. Builds the identical single-whole-extent-
 *              chunk in-memory dataset the writer side built, injects the
 *              already-filtered bytes directly into chunk storage via
 *              H5Dwrite_chunk() (bypassing the pipeline on the way in, since
 *              they are already filtered), then reads it back normally
 *              (H5Dread(), which decodes: unfilters via that same pipeline)
 *              to get the real values.
 *
 * Return:      0 on success, with *out_buf (malloc'd, caller frees) and
 *              *out_len filled in; -1 on failure (caller falls back to
 *              handing back the raw filtered bytes -- degraded, not fatal).
 *-------------------------------------------------------------------------
 */
static int
H5VL__stream_unfilter_pushed_data(const void *filtered_buf, uint64_t filtered_len, const uint8_t *dcpl_enc,
                                    uint64_t dcpl_enc_len, const uint8_t *type_enc, uint64_t type_enc_len,
                                    uint64_t count, uint32_t filter_mask, void **out_buf, size_t *out_len)
{
    hid_t   dcpl = -1, type = -1, space = -1, fapl = -1, fid = -1, ds = -1;
    hsize_t dims[1];
    hsize_t chunk_offset[1] = {0};
    void   *decoded = NULL;
    size_t  elem_size;
    int     ret_value = -1;

    if (count == 0 || !dcpl_enc || dcpl_enc_len == 0 || !type_enc || type_enc_len == 0)
        return -1;

    if ((dcpl = H5Pdecode(dcpl_enc)) < 0)
        goto done;
    if ((type = H5Tdecode2(type_enc, (size_t)type_enc_len)) < 0)
        goto done;
    if ((elem_size = H5Tget_size(type)) == 0)
        goto done;

    dims[0] = (hsize_t)count;
    if ((space = H5Screate_simple(1, dims, NULL)) < 0)
        goto done;
    if (H5Pset_chunk(dcpl, 1, dims) < 0)
        goto done;

    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_fapl_core(fapl, 1 << 20, false) < 0)
        goto done;
    if ((fid = H5Fcreate("vol_stream_unfilter_tmp.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0)
        goto done;
    if ((ds = H5Dcreate2(fid, "d", type, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0)
        goto done;

    if (H5Dwrite_chunk(ds, H5P_DEFAULT, filter_mask, chunk_offset, (size_t)filtered_len, filtered_buf) < 0)
        goto done;

    if (NULL == (decoded = malloc(elem_size * (size_t)count)))
        goto done;
    if (H5Dread(ds, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, decoded) < 0)
        goto done;

    *out_buf  = decoded;
    *out_len  = elem_size * (size_t)count;
    decoded   = NULL;
    ret_value = 0;

done:
    free(decoded);
    if (ds >= 0)
        H5Dclose(ds);
    if (fid >= 0)
        H5Fclose(fid);
    if (fapl >= 0)
        H5Pclose(fapl);
    if (space >= 0)
        H5Sclose(space);
    if (type >= 0)
        H5Tclose(type);
    if (dcpl >= 0)
        H5Pclose(dcpl);
    return ret_value;
} /* end H5VL__stream_unfilter_pushed_data() */
#endif /* VOL_STREAM_HAVE_MERCURY */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_type_cache_lookup / H5VL__stream_type_cache_upsert
 *
 * Purpose:     dev-plan.md's residual risks: a DsetWrite entry's type_enc
 *              was unconditionally re-H5Tencode()'d and re-persisted to
 *              /step/<n>/.manifest on every single write, even though a
 *              dataset's type never changes after creation -- real,
 *              unbounded metadata bloat at high step cadence. These two
 *              helpers back a small per-path cache (one array on the
 *              writer side, a mirror on the replay side, both hung off
 *              file_state) that lets H5VL__stream_build_manifest() omit a
 *              DsetWrite entry's type_enc entirely when it is byte-identical
 *              to the last one sent for that path, and lets
 *              H5VL__stream_replay_manifest() resolve that omission back
 *              into real bytes. No schema change was needed: an omitted
 *              flatbuffers vector field decodes as NULL with
 *              flatbuffers_uint8_vec_len() == 0 (verified against flatcc's
 *              own __flatbuffers_vec_len() macro), a sentinel no valid
 *              H5Tencode() output can ever produce, so this is unambiguous
 *              and fully backward compatible with a manifest that predates
 *              caching (every entry there has real bytes, so the cache
 *              simply never triggers for it).
 *
 * Return:      lookup:  the cached entry's type_enc/len (may be a 0-length
 *                        result if truly not found -- callers distinguish
 *                        via the returned pointer being NULL)
 *              upsert:  0 on success, -1 on failure (out of memory)
 *-------------------------------------------------------------------------
 */
static const uint8_t *
H5VL__stream_type_cache_lookup(H5VL_stream_type_cache_entry_t *arr, size_t n, const char *path, size_t *out_len)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (0 == strcmp(arr[i].path, path)) {
            *out_len = arr[i].type_enc_len;
            return arr[i].type_enc;
        }
    }
    return NULL;
} /* end H5VL__stream_type_cache_lookup() */

static herr_t
H5VL__stream_type_cache_upsert(H5VL_stream_type_cache_entry_t **arr, size_t *n, size_t *cap, const char *path,
                                const uint8_t *type_enc, size_t type_enc_len)
{
    size_t i;
    uint8_t *copy;

    if (NULL == (copy = (uint8_t *)malloc(type_enc_len ? type_enc_len : 1)))
        return -1;
    if (type_enc_len > 0)
        memcpy(copy, type_enc, type_enc_len);

    for (i = 0; i < *n; i++) {
        if (0 == strcmp((*arr)[i].path, path)) {
            free((*arr)[i].type_enc);
            (*arr)[i].type_enc     = copy;
            (*arr)[i].type_enc_len = type_enc_len;
            return 0;
        }
    }

    if (*n == *cap) {
        size_t                            new_cap = *cap ? *cap * 2 : 8;
        H5VL_stream_type_cache_entry_t *grown;

        if (NULL == (grown = (H5VL_stream_type_cache_entry_t *)realloc(*arr, new_cap * sizeof(**arr)))) {
            free(copy);
            return -1;
        }
        *arr = grown;
        *cap = new_cap;
    }

    if (NULL == ((*arr)[*n].path = strdup(path))) {
        free(copy);
        return -1;
    }
    (*arr)[*n].type_enc     = copy;
    (*arr)[*n].type_enc_len = type_enc_len;
    (*n)++;
    return 0;
} /* end H5VL__stream_type_cache_upsert() */

static void
H5VL__stream_type_cache_clear(H5VL_stream_type_cache_entry_t *arr, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        free(arr[i].path);
        free(arr[i].type_enc);
    }
    free(arr);
} /* end H5VL__stream_type_cache_clear() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_replay_ensure_group
 *
 * Purpose:     Open the group at abs_path under the file, creating it (and
 *              any missing ancestors) if it does not exist yet. Used to land
 *              a dataset's parent and an attribute's parent group that was
 *              not itself created by a DsetCreate entry replayed earlier in
 *              the same pass.
 *
 *              Walks one path component at a time rather than creating the
 *              whole path in a single H5VLgroup_create() call with
 *              H5Pset_create_intermediate_group() set: that property is read
 *              from HDF5's internal API-context state (H5CX), which only the
 *              *public* H5Gcreate2()/H5Dcreate2() wrappers populate from an
 *              LCPL before dispatching -- calling the H5VL*-level create
 *              functions directly, as replay does throughout, bypasses that
 *              plumbing entirely and the property is silently not honored.
 *              A single-level create against an already-resolved parent has
 *              no such dependency, so walking sidesteps the problem rather
 *              than working around it.
 *
 * Return:      Success:    Underlying group object (caller closes it)
 *              Failure:    NULL
 *-------------------------------------------------------------------------
 */
static void *
H5VL__stream_replay_ensure_group(void *file_under, hid_t under_vol_id, const char *abs_path)
{
    H5VL_loc_params_t loc_params;
    void              *cur;
    int                cur_is_file;
    const char        *p = abs_path;

    memset(&loc_params, 0, sizeof(loc_params));
    loc_params.type = H5VL_OBJECT_BY_SELF;

    cur         = file_under;
    cur_is_file = 1;

    while (*p == '/')
        p++;

    while (*p) {
        const char *slash    = strchr(p, '/');
        size_t      seg_len  = slash ? (size_t)(slash - p) : strlen(p);
        char        seg[256];
        void       *next;
        hid_t       err_id;

        if (seg_len == 0 || seg_len >= sizeof(seg)) {
            if (!cur_is_file)
                H5VLgroup_close(cur, under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
            return NULL;
        }
        memcpy(seg, p, seg_len);
        seg[seg_len] = '\0';

        loc_params.obj_type = cur_is_file ? H5I_FILE : H5I_GROUP;

        /* A missing group is the common case -- most of /step/<n>/ is fresh
         * every step -- so probe quietly: swap in a scratch error stack
         * (discarded on restore, so the caller's own stack sees no residue)
         * and disable auto-print for the duration (the stack swap alone
         * only stops the error from lingering -- HDF5's error macros print
         * each frame synchronously as it is pushed, regardless of which
         * stack it lands on). */
        {
            H5E_auto2_t old_func;
            void       *old_data;

            H5Eget_auto2(H5E_DEFAULT, &old_func, &old_data);
            H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
            err_id = H5Eget_current_stack();
            next   = H5VLgroup_open(cur, &loc_params, under_vol_id, seg, H5P_GROUP_ACCESS_DEFAULT,
                                    H5P_DATASET_XFER_DEFAULT, NULL);
            H5Eset_current_stack(err_id);
            H5Eset_auto2(H5E_DEFAULT, old_func, old_data);
        }

        if (!next)
            next = H5VLgroup_create(cur, &loc_params, under_vol_id, seg, H5P_LINK_CREATE_DEFAULT,
                                    H5P_GROUP_CREATE_DEFAULT, H5P_GROUP_ACCESS_DEFAULT,
                                    H5P_DATASET_XFER_DEFAULT, NULL);

        if (!cur_is_file)
            H5VLgroup_close(cur, under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);

        if (!next)
            return NULL;

        cur         = next;
        cur_is_file = 0;

        p = slash ? slash + 1 : p + seg_len;
    }

    return cur_is_file ? NULL : cur; /* abs_path had no components -- nothing to return */
} /* end H5VL__stream_replay_ensure_group() */

/*-------------------------------------------------------------------------
 * M3 reader helpers.
 *
 * A reader's index (physical_step -> logical_ids[], path -> steps carrying
 * it, logical_id -> its authoritative/latest physical_step) is built once,
 * lazily, on the first reader begin_step -- see H5VL__stream_reader_advance().
 * The set of steps is static for the whole reader session (M3 has no live
 * transport yet), so there is nothing to invalidate afterward.
 *-------------------------------------------------------------------------
 */

/* H5L_iterate2_t callback for iterating "/step"'s children: collect every
 * name that parses as a whole base-10 uint64_t -- every step group name
 * H5VL__stream_replay_step() ever creates. Anything else (a hand-added
 * child, say) is skipped, not fatal. */
typedef struct H5VL_stream_step_collector_t {
    uint64_t *steps;
    size_t    n, cap;
    int       failed;
} H5VL_stream_step_collector_t;

static herr_t
H5VL__stream_reader_step_iter_cb(hid_t grp, const char *name, const H5L_info2_t *info, void *op_data)
{
    H5VL_stream_step_collector_t *c = (H5VL_stream_step_collector_t *)op_data;
    char                         *end;
    uint64_t                      v;

    (void)grp;
    (void)info;

    v = strtoull(name, &end, 10);
    if (end == name || *end != '\0')
        return 0; /* not one of ours */

    if (c->n == c->cap) {
        size_t    new_cap = c->cap ? c->cap * 2 : 16;
        uint64_t *grown   = (uint64_t *)realloc(c->steps, new_cap * sizeof(uint64_t));

        if (!grown) {
            c->failed = 1;
            return -1;
        }
        c->steps = grown;
        c->cap   = new_cap;
    }
    c->steps[c->n++] = v;

    return 0;
} /* end H5VL__stream_reader_step_iter_cb() */

static int
H5VL__stream_uint64_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return (x > y) - (x < y);
} /* end H5VL__stream_uint64_cmp() */

static int
H5VL__stream_logical_map_cmp(const void *a, const void *b)
{
    const H5VL_stream_logical_map_entry_t *x = (const H5VL_stream_logical_map_entry_t *)a;
    const H5VL_stream_logical_map_entry_t *y = (const H5VL_stream_logical_map_entry_t *)b;

    return (x->logical_id > y->logical_id) - (x->logical_id < y->logical_id);
} /* end H5VL__stream_logical_map_cmp() */

/* logical_id -> largest physical_step seen for it. Callers scan p ascending,
 * so the final value after all steps are folded in is always the
 * authoritative one -- a later restart write supersedes an earlier one, per
 * dev-plan.md decision #1. */
static herr_t
H5VL__stream_logical_map_set(H5VL_stream_file_state_t *fs, uint64_t logical_id, uint64_t physical_step)
{
    size_t i;

    for (i = 0; i < fs->n_logical_map; i++)
        if (fs->logical_map[i].logical_id == logical_id) {
            fs->logical_map[i].physical_step = physical_step;
            return 0;
        }

    if (fs->n_logical_map == fs->cap_logical_map) {
        size_t                            new_cap = fs->cap_logical_map ? fs->cap_logical_map * 2 : 16;
        H5VL_stream_logical_map_entry_t *grown =
            (H5VL_stream_logical_map_entry_t *)realloc(fs->logical_map, new_cap * sizeof(*grown));

        if (!grown)
            return -1;
        fs->logical_map     = grown;
        fs->cap_logical_map = new_cap;
    }

    fs->logical_map[fs->n_logical_map].logical_id    = logical_id;
    fs->logical_map[fs->n_logical_map].physical_step = physical_step;
    fs->n_logical_map++;

    return 0;
} /* end H5VL__stream_logical_map_set() */

/* Append physical_step to path's step list, creating the path's entry in
 * fs->path_index if this is the first time it's seen. Callers scan p
 * ascending, so the per-path step list ends up sorted with no extra work. */
static herr_t
H5VL__stream_path_index_add(H5VL_stream_file_state_t *fs, const char *path, uint64_t physical_step)
{
    size_t                     i;
    H5VL_stream_path_steps_t *e = NULL;

    for (i = 0; i < fs->n_path_index; i++)
        if (strcmp(fs->path_index[i].path, path) == 0) {
            e = &fs->path_index[i];
            break;
        }

    if (!e) {
        if (fs->n_path_index == fs->cap_path_index) {
            size_t                     new_cap = fs->cap_path_index ? fs->cap_path_index * 2 : 16;
            H5VL_stream_path_steps_t *grown =
                (H5VL_stream_path_steps_t *)realloc(fs->path_index, new_cap * sizeof(*grown));

            if (!grown)
                return -1;
            fs->path_index     = grown;
            fs->cap_path_index = new_cap;
        }
        e = &fs->path_index[fs->n_path_index++];
        memset(e, 0, sizeof(*e));
        if (NULL == (e->path = strdup(path)))
            return -1;
    }

    if (e->n_steps == e->cap_steps) {
        size_t    new_cap = e->cap_steps ? e->cap_steps * 2 : 4;
        uint64_t *grown   = (uint64_t *)realloc(e->steps, new_cap * sizeof(uint64_t));

        if (!grown)
            return -1;
        e->steps     = grown;
        e->cap_steps = new_cap;
    }
    e->steps[e->n_steps++] = physical_step;

    return 0;
} /* end H5VL__stream_path_index_add() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_path_index_resolve
 *
 * Purpose:     The largest physical step <= current_step carrying a
 *              DsetCreate/Attr entry at path -- "the state of path as of
 *              step current_step". -1 if path never appears at all, or
 *              every occurrence is later than current_step (the object
 *              legitimately doesn't exist yet as of this step).
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_path_index_resolve(H5VL_stream_file_state_t *fs, const char *path, uint64_t current_step,
                                 uint64_t *resolved_step)
{
    size_t i, j;

    for (i = 0; i < fs->n_path_index; i++)
        if (strcmp(fs->path_index[i].path, path) == 0) {
            H5VL_stream_path_steps_t *e = &fs->path_index[i];

            for (j = e->n_steps; j-- > 0;)
                if (e->steps[j] <= current_step) {
                    *resolved_step = e->steps[j];
                    return 0;
                }
            return -1;
        }

    return -1;
} /* end H5VL__stream_path_index_resolve() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_resolve_physical_path
 *
 * Purpose:     Map an application-facing logical path ("/target") to where
 *              the object actually lives in the underlying file
 *              ("/step/<k>/target"), for the largest committed step <= the
 *              file's current one. The two namespaces differ because steps
 *              land group-based, so anything that hands a path to the
 *              *under* VOL by name -- rather than through an already-opened
 *              object -- has to translate first.
 *
 *              Used by the H5R path (H5VL_stream_object_specific()'s
 *              lookup): H5Rcreate_object(fid, "/target") resolves the name
 *              through the under connector, which knows nothing about
 *              logical paths and would report "object doesn't exist".
 *
 *              Only resolves objects from *already committed* steps. An
 *              object created in the currently-open step is deliberately
 *              not found: it does not exist in the underlying file until
 *              end_step() replays the manifest, so there is nothing to make
 *              a reference to yet. That is a real property of deferred
 *              writes, not a lookup bug -- see docs/dev-plan.md's
 *              reference-support findings.
 *
 * Return:      Success:    malloc'd resolved path (caller frees)
 *              Failure:    NULL (no such object as of this step)
 *-------------------------------------------------------------------------
 */
static char *
H5VL__stream_resolve_physical_path(H5VL_stream_file_state_t *fs, const char *logical_path)
{
    uint64_t step;
    char    *resolved;
    size_t   len;

    if (!fs || !logical_path || logical_path[0] != '/')
        return NULL;
    if (H5VL__stream_path_index_resolve(fs, logical_path, fs->current_step, &step) < 0)
        return NULL;

    len = strlen("/step/") + 20 /* digits */ + strlen(logical_path) + 1;
    if (NULL == (resolved = (char *)malloc(len)))
        return NULL;
    snprintf(resolved, len, "/step/%llu%s", (unsigned long long)step, logical_path);
    return resolved;
} /* end H5VL__stream_resolve_physical_path() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_reader_index_one_step
 *
 * Purpose:     Decode one step's .manifest, folding it into fs's indexes.
 *              Verified with flatcc's own generated verifier
 *              (vs_Step_verify_as_root()) rather than trusted blindly: unlike
 *              H5VL__stream_replay_step()'s just-built buffer, this manifest
 *              is coming back from durable storage.
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_reader_index_one_step(H5VL_stream_file_state_t *fs, size_t p)
{
    H5VL_loc_params_t loc_params;
    char               path[48]; /* "/step/" + up to 20 digits + "/.manifest" + NUL */
    void              *mds;
    hid_t              dtype = -1, space = -1;
    hssize_t           npoints;
    uint8_t           *buf = NULL;
    vs_Step_table_t    step;
    herr_t             ret_value = -1;

    snprintf(path, sizeof(path), "/step/%zu/.manifest", p);

    memset(&loc_params, 0, sizeof(loc_params));
    loc_params.obj_type = H5I_FILE;
    loc_params.type     = H5VL_OBJECT_BY_SELF; /* ignored by dataset_open; name
                                                 * does all the work */

    if (NULL == (mds = H5VLdataset_open(fs->file_under_object, &loc_params, fs->file_under_vol_id, path,
                                         H5P_DATASET_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, NULL)))
        return -1;

    {
        H5VL_dataset_get_args_t gargs;

        gargs.op_type = H5VL_DATASET_GET_TYPE;
        if (H5VLdataset_get(mds, fs->file_under_vol_id, &gargs, H5P_DATASET_XFER_DEFAULT, NULL) < 0)
            goto done;
        dtype = gargs.args.get_type.type_id;

        gargs.op_type = H5VL_DATASET_GET_SPACE;
        if (H5VLdataset_get(mds, fs->file_under_vol_id, &gargs, H5P_DATASET_XFER_DEFAULT, NULL) < 0)
            goto done;
        space = gargs.args.get_space.space_id;
    }

    if ((npoints = H5Sget_simple_extent_npoints(space)) < 0)
        goto done;
    if (NULL == (buf = (uint8_t *)malloc(npoints ? (size_t)npoints : 1)))
        goto done;

    if (npoints > 0) {
        void *bufp = buf;

        if (H5VLdataset_read(1, &mds, fs->file_under_vol_id, &dtype, &space, &space,
                              H5P_DATASET_XFER_DEFAULT, &bufp, NULL) < 0)
            goto done;
    }

    if (vs_Step_verify_as_root(buf, (size_t)npoints) != 0 || NULL == (step = vs_Step_as_root(buf)))
        goto done;

    {
        flatbuffers_uint64_vec_t lids = vs_Step_logical_ids(step);
        size_t                   n    = lids ? flatbuffers_uint64_vec_len(lids) : 0;
        size_t                   i;

        if (n > 0) {
            if (NULL == (fs->step_logical_ids[p] = (uint64_t *)malloc(n * sizeof(uint64_t))))
                goto done;
            for (i = 0; i < n; i++) {
                uint64_t lid = flatbuffers_uint64_vec_at(lids, i);

                fs->step_logical_ids[p][i] = lid;
                if (H5VL__stream_logical_map_set(fs, lid, (uint64_t)p) < 0)
                    goto done;
            }
            fs->n_logical_per_step[p] = n;
        }
    }

    {
        vs_Entry_vec_t entries = vs_Step_entries(step);
        size_t         n       = entries ? vs_Entry_vec_len(entries) : 0;
        size_t         i;

        for (i = 0; i < n; i++) {
            vs_Entry_table_t e     = vs_Entry_vec_at(entries, i);
            vs_Kind_enum_t   kind  = vs_Entry_kind(e);
            const char      *epath = vs_Entry_path(e);

            if ((kind == vs_Kind_DsetCreate || kind == vs_Kind_Attr) && epath &&
                H5VL__stream_path_index_add(fs, epath, (uint64_t)p) < 0)
                goto done;
        }
    }

    ret_value = 0;

done:
    if (dtype >= 0)
        H5Tclose(dtype);
    if (space >= 0)
        H5Sclose(space);
    if (mds)
        H5VLdataset_close(mds, fs->file_under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
    free(buf);

    return ret_value;
} /* end H5VL__stream_reader_index_one_step() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_reader_build_index
 *
 * Purpose:     Enumerate "/step"'s children, confirm they are exactly
 *              0..n-1 (physical steps are provably contiguous -- end_step()
 *              always does an unconditional physical_step++ after a
 *              successful replay, so a gap or duplicate means this file
 *              wasn't produced by this connector), and decode every step's
 *              manifest into fs's indexes.
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_reader_build_index(H5VL_stream_file_state_t *fs)
{
    H5VL_stream_step_collector_t coll;
    H5VL_loc_params_t             loc_params;
    H5VL_link_specific_args_t     largs;
    hsize_t                       idx = 0;
    size_t                        p;

    memset(&coll, 0, sizeof(coll));
    memset(&loc_params, 0, sizeof(loc_params));
    loc_params.obj_type                     = H5I_FILE;
    loc_params.type                         = H5VL_OBJECT_BY_NAME;
    loc_params.loc_data.loc_by_name.name    = "/step";
    loc_params.loc_data.loc_by_name.lapl_id = H5P_LINK_ACCESS_DEFAULT;

    largs.op_type                = H5VL_LINK_ITER;
    largs.args.iterate.recursive = false;
    largs.args.iterate.idx_type  = H5_INDEX_NAME;
    largs.args.iterate.order     = H5_ITER_INC;
    largs.args.iterate.idx_p     = &idx;
    largs.args.iterate.op        = H5VL__stream_reader_step_iter_cb;
    largs.args.iterate.op_data   = &coll;

    /* A missing /step (zero steps written yet) is the routine case for a
     * freshly created, never-stepped file -- probe quietly, same idiom as
     * H5VL__stream_replay_ensure_group(). */
    {
        H5E_auto2_t old_func;
        void       *old_data;
        hid_t       err_id;
        herr_t      r;

        H5Eget_auto2(H5E_DEFAULT, &old_func, &old_data);
        H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
        err_id = H5Eget_current_stack();
        r = H5VLlink_specific(fs->file_under_object, &loc_params, fs->file_under_vol_id, &largs,
                               H5P_DATASET_XFER_DEFAULT, NULL);
        H5Eset_current_stack(err_id);
        H5Eset_auto2(H5E_DEFAULT, old_func, old_data);

        if (r < 0 || coll.failed) {
            free(coll.steps);
            fs->index_built = 1;
            return 0;
        }
    }

    /* H5_INDEX_NAME iterates alphabetically ("10" < "2"), not numerically --
     * always re-sort before trusting order. */
    qsort(coll.steps, coll.n, sizeof(uint64_t), H5VL__stream_uint64_cmp);

    for (p = 0; p < coll.n; p++)
        if (coll.steps[p] != (uint64_t)p) { /* gap/dup: not our file */
            free(coll.steps);
            return -1;
        }

    fs->n_steps_total = coll.n;
    free(coll.steps);

    if (fs->n_steps_total > 0 &&
        (NULL == (fs->step_logical_ids = (uint64_t **)calloc(fs->n_steps_total, sizeof(uint64_t *))) ||
         NULL == (fs->n_logical_per_step = (size_t *)calloc(fs->n_steps_total, sizeof(size_t)))))
        return -1;

    for (p = 0; p < fs->n_steps_total; p++)
        if (H5VL__stream_reader_index_one_step(fs, p) < 0)
            return -1;

    qsort(fs->logical_map, fs->n_logical_map, sizeof(*fs->logical_map), H5VL__stream_logical_map_cmp);

    fs->index_built = 1;

    return 0;
} /* end H5VL__stream_reader_build_index() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_reader_advance
 *
 * Purpose:     Shared by H5Fbegin_step()-as-reader (has_target=0, advance
 *              sequentially) and H5Fbegin_logical_step() (has_target=1, jump
 *              to a resolved physical step). No such step: fail,
 *              current_step/step_state untouched -- real EOS is a
 *              live-writer concept M3's single-process scope cannot
 *              exercise; see the M3 plan note in H5VLstream.h.
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_reader_advance(H5VL_stream_file_state_t *fs, int has_target, uint64_t target_step)
{
    uint64_t next;

    if (!fs->index_built && H5VL__stream_reader_build_index(fs) < 0)
        return -1;

    next = has_target ? target_step : (fs->step_state == H5F_STEP_READING ? fs->current_step + 1 : 0);

    if (next >= fs->n_steps_total)
        return -1;

    fs->current_step = next;
    fs->step_state    = H5F_STEP_READING;

    return 0;
} /* end H5VL__stream_reader_advance() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_reader_open_dataset
 *
 * Purpose:     Resolve name (relative to o, a file or reader-virtual group)
 *              to the largest physical step <= fs->current_step that has it,
 *              and open the real object at "/step/<k>/<abs_path>" directly
 *              through the file root -- never through o's own under_object,
 *              even if o is itself a resolved group, since a reader-mode
 *              group has no single underlying object (M3 plan, "Critical
 *              finding #1"). One H5VLdataset_open() call does the whole
 *              multi-component path; loc_params->type is ignored by the
 *              native connector for datasets/groups (M3 plan, "Critical
 *              finding #3"), so BY_SELF here is conventional, not load-
 *              bearing.
 *
 * Return:      Success:    Pointer to a new (LIVE) dataset wrapper
 *              Failure:    NULL
 *-------------------------------------------------------------------------
 */
static void *
H5VL__stream_reader_open_dataset(H5VL_stream_t *o, const char *name, hid_t dapl_id, hid_t dxpl_id,
                                  void **req)
{
    H5VL_stream_file_state_t *fs = o->file_state;
    char                      *abs_path, *resolved;
    uint64_t                   step;
    size_t                     resolved_len;
    void                      *under;
    H5VL_stream_t             *dset;
    H5VL_loc_params_t          loc_params;

    if (NULL == (abs_path = H5VL__stream_child_path(o->path, name)))
        return NULL;

    if (H5VL__stream_path_index_resolve(fs, abs_path, fs->current_step, &step) < 0) {
        free(abs_path); /* legitimately doesn't exist yet as of this step */
        return NULL;
    }

    resolved_len = strlen("/step/") + 20 /* digits */ + strlen(abs_path) + 1;
    if (NULL == (resolved = (char *)malloc(resolved_len))) {
        free(abs_path);
        return NULL;
    }
    snprintf(resolved, resolved_len, "/step/%llu%s", (unsigned long long)step, abs_path);

    memset(&loc_params, 0, sizeof(loc_params));
    loc_params.obj_type = H5I_FILE;
    loc_params.type     = H5VL_OBJECT_BY_SELF;

    under = H5VLdataset_open(fs->file_under_object, &loc_params, fs->file_under_vol_id, resolved, dapl_id,
                              dxpl_id, req);
    free(resolved);
    if (!under) {
        free(abs_path);
        return NULL;
    }

    if (NULL == (dset = H5VL_stream_new_obj(under, fs->file_under_vol_id))) {
        H5VLdataset_close(under, fs->file_under_vol_id, dxpl_id, NULL);
        free(abs_path);
        return NULL;
    }
    dset->file_state = fs;
    H5VL__stream_file_state_incref(fs);
    dset->path = abs_path; /* the LOGICAL path, not the physical /step/<k>/
                             * form -- so a further child opened on this
                             * dataset (not applicable to datasets today, but
                             * kept consistent with groups/attrs) resolves
                             * correctly rather than double-prefixing. */

    if (req && *req)
        *req = H5VL_stream_new_obj(*req, fs->file_under_vol_id);

    return (void *)dset;
} /* end H5VL__stream_reader_open_dataset() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_reader_open_attr
 *
 * Purpose:     Same idea as H5VL__stream_reader_open_dataset(), but for
 *              attributes: resolves via the "@"-joined manifest path
 *              (H5VL__stream_attr_path()), then opens using
 *              H5VL_OBJECT_BY_NAME against the resolved parent's physical
 *              path -- required because, unlike dataset/group opens, the
 *              native connector's attribute open *does* need two separate
 *              name levels (M3 plan, "Critical finding #3"): the parent
 *              object's path, and the attribute's own leaf name, which
 *              can't itself contain '/'.
 *
 * Return:      Success:    Pointer to a new (LIVE) attribute wrapper
 *              Failure:    NULL
 *-------------------------------------------------------------------------
 */
static void *
H5VL__stream_reader_open_attr(H5VL_stream_t *o, const char *name, hid_t aapl_id, hid_t dxpl_id, void **req)
{
    H5VL_stream_file_state_t *fs = o->file_state;
    char                      *abs_path, *full_parent;
    const char                *at;
    uint64_t                   step;
    size_t                     full_parent_len;
    void                      *under;
    H5VL_stream_t             *attr;
    H5VL_loc_params_t          loc_params;

    if (NULL == (abs_path = H5VL__stream_attr_path(o->path, name)))
        return NULL;

    if (H5VL__stream_path_index_resolve(fs, abs_path, fs->current_step, &step) < 0) {
        free(abs_path);
        return NULL;
    }

    at = strrchr(abs_path, '@'); /* always present -- H5VL__stream_attr_path() inserts it */

    full_parent_len = strlen("/step/") + 20 + (size_t)(at - abs_path) + 1;
    if (NULL == (full_parent = (char *)malloc(full_parent_len))) {
        free(abs_path);
        return NULL;
    }
    snprintf(full_parent, full_parent_len, "/step/%llu%.*s", (unsigned long long)step, (int)(at - abs_path),
              abs_path);

    memset(&loc_params, 0, sizeof(loc_params));
    loc_params.obj_type                     = H5I_FILE;
    loc_params.type                         = H5VL_OBJECT_BY_NAME;
    loc_params.loc_data.loc_by_name.name    = full_parent;
    loc_params.loc_data.loc_by_name.lapl_id = H5P_LINK_ACCESS_DEFAULT;

    under = H5VLattr_open(fs->file_under_object, &loc_params, fs->file_under_vol_id, at + 1, aapl_id, dxpl_id,
                          req);
    free(full_parent);
    if (!under) {
        free(abs_path);
        return NULL;
    }

    if (NULL == (attr = H5VL_stream_new_obj(under, fs->file_under_vol_id))) {
        H5VLattr_close(under, fs->file_under_vol_id, dxpl_id, NULL);
        free(abs_path);
        return NULL;
    }
    attr->file_state = fs;
    H5VL__stream_file_state_incref(fs);
    attr->path = abs_path;

    if (req && *req)
        *req = H5VL_stream_new_obj(*req, fs->file_under_vol_id);

    return (void *)attr;
} /* end H5VL__stream_reader_open_attr() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_build_manifest
 *
 * Purpose:     M7: split out of what was the first half of
 *              H5VL__stream_replay_step() (still below, now a thin
 *              build-then-replay wrapper) so M7's Spill policy can build a
 *              step's manifest+payload bytes without paying for the second
 *              half -- the real H5Dcreate2()/H5Awrite() etc. calls that
 *              would touch the (possibly congested) shared file, which is
 *              exactly the cost Spill exists to defer. Encodes fs's pending
 *              entries into a flatcc Step manifest exactly as before; see
 *              H5VL__stream_replay_manifest()'s comment for the decode half.
 *
 * Return:      Success:    0, with *out_manifest_buf/*out_payload_buf newly
 *                          allocated (caller frees: flatcc_builder_free() for
 *                          the former, free() for the latter)
 *              Failure:    -1, *out_manifest_buf/*out_payload_buf untouched
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_build_manifest(H5VL_stream_file_state_t *fs, uint8_t **out_manifest_buf,
                              size_t *out_manifest_len, uint8_t **out_payload_buf, size_t *out_payload_len)
{
    flatcc_builder_t B;
    int               builder_ready = 0;
    vs_Entry_ref_t   *entry_refs    = NULL;
    uint8_t          *payload_buf   = NULL;
    size_t            payload_cap   = 0;
    size_t            payload_len   = 0;
    uint8_t          *manifest_buf  = NULL;
    size_t            manifest_len  = 0;
    unsigned          maj, minor, rel;
    size_t            i;
    herr_t            ret_value = 0;

    if (NULL ==
        (entry_refs = (vs_Entry_ref_t *)malloc((fs->n_pending ? fs->n_pending : 1) * sizeof(vs_Entry_ref_t)))) {
        ret_value = -1;
        goto done;
    }

    flatcc_builder_init(&B);
    builder_ready = 1;

    /* Pass 1: encode each pending entry's blobs, build its flatcc Entry, and
     * append its payload bytes to one contiguous buffer -- payload_off/len
     * index into it, the schema's "payload stored outside the flatbuffer"
     * design (dev-plan.md's Step manifest section). */
    for (i = 0; i < fs->n_pending; i++) {
        H5VL_stream_pending_entry_t *pe        = &fs->pending[i];
        uint8_t                     *type_enc  = NULL;
        uint8_t                     *space_enc = NULL;
        uint8_t                     *dcpl_enc  = NULL;
        size_t                       type_len  = 0;
        size_t                       space_len = 0;
        size_t                       dcpl_len  = 0;
        uint64_t                     this_off  = (uint64_t)payload_len;

        if (H5VL__stream_encode_type(pe->type_id, &type_enc, &type_len) < 0 ||
            H5VL__stream_encode_space(pe->space_id, &space_enc, &space_len) < 0 ||
            (pe->dcpl_id >= 0 && H5VL__stream_encode_dcpl(pe->dcpl_id, &dcpl_enc, &dcpl_len) < 0)) {
            free(type_enc);
            free(space_enc);
            free(dcpl_enc);
            ret_value = -1;
            goto done;
        }

        /* dev-plan.md's residual risks: a DsetWrite's type never changes
         * across steps for a given path, so once this exact type_enc has
         * already been sent for pe->path, omit it here (see
         * H5VL__stream_type_cache_lookup()'s comment for the wire-format
         * reasoning) instead of re-persisting the same bytes to
         * /step/<n>/.manifest every step. DsetCreate/Attr entries are
         * unaffected -- they only ever happen once per object, so there is
         * nothing to cache for them. */
        if (pe->kind == vs_Kind_DsetWrite) {
            size_t          cached_len = 0;
            const uint8_t *cached     = H5VL__stream_type_cache_lookup(fs->write_type_cache,
                                                                        fs->n_write_type_cache, pe->path,
                                                                        &cached_len);

            if (cached && cached_len == type_len && (type_len == 0 || 0 == memcmp(cached, type_enc, type_len))) {
                free(type_enc);
                type_enc = NULL;
                type_len = 0;
            }
            else if (H5VL__stream_type_cache_upsert(&fs->write_type_cache, &fs->n_write_type_cache,
                                                      &fs->cap_write_type_cache, pe->path, type_enc,
                                                      type_len) < 0) {
                free(type_enc);
                free(space_enc);
                free(dcpl_enc);
                ret_value = -1;
                goto done;
            }
        }

        if (pe->payload_len > 0) {
            if (payload_len + pe->payload_len > payload_cap) {
                size_t   new_cap = payload_cap ? payload_cap * 2 : 4096;
                uint8_t *grown;

                while (new_cap < payload_len + pe->payload_len)
                    new_cap *= 2;
                if (NULL == (grown = (uint8_t *)realloc(payload_buf, new_cap))) {
                    free(type_enc);
                    free(space_enc);
                    free(dcpl_enc);
                    ret_value = -1;
                    goto done;
                }
                payload_buf = grown;
                payload_cap = new_cap;
            }
            memcpy(payload_buf + payload_len, pe->payload, pe->payload_len);
            payload_len += pe->payload_len;
        }

        vs_Entry_start(&B);
        vs_Entry_kind_add(&B, (vs_Kind_enum_t)pe->kind);
        vs_Entry_path_create_str(&B, pe->path);
        if (type_enc) /* NULL/0 here means: identical to the cache, omitted -- see above */
            vs_Entry_type_enc_create(&B, type_enc, type_len);
        vs_Entry_space_enc_create(&B, space_enc, space_len);
        if (dcpl_enc)
            vs_Entry_dcpl_enc_create(&B, dcpl_enc, dcpl_len);
        vs_Entry_form_add(&B, vs_Payload_Raw);
        vs_Entry_payload_off_add(&B, this_off);
        vs_Entry_payload_len_add(&B, (uint64_t)pe->payload_len);
        entry_refs[i] = vs_Entry_end(&B);

        free(type_enc);
        free(space_enc);
        free(dcpl_enc);
    }

    H5get_libversion(&maj, &minor, &rel);

    vs_Step_start_as_root(&B);
    vs_Step_physical_step_add(&B, fs->physical_step);
    if (fs->n_logical > 0)
        vs_Step_logical_ids_create(&B, fs->logical_ids, fs->n_logical);
    vs_Step_wall_time_ns_add(&B, fs->wall_time_ns);
    vs_Step_hdf5_version_add(&B, (uint32_t)(maj * 1000000u + minor * 1000u + rel));
    if (fs->n_pending > 0)
        vs_Step_entries_create(&B, entry_refs, fs->n_pending);
    vs_Step_payload_bytes_add(&B, (uint64_t)payload_len);
    vs_Step_end_as_root(&B);

    if (NULL == (manifest_buf = (uint8_t *)flatcc_builder_finalize_buffer(&B, &manifest_len))) {
        ret_value = -1;
        goto done;
    }

    *out_manifest_buf = manifest_buf;
    *out_manifest_len = manifest_len;
    *out_payload_buf  = payload_buf;
    *out_payload_len  = payload_len;
    manifest_buf      = NULL; /* ownership moved to the caller */
    payload_buf       = NULL;

done:
    if (manifest_buf)
        flatcc_builder_free(manifest_buf);
    if (builder_ready)
        flatcc_builder_clear(&B);
    free(entry_refs);
    free(payload_buf);

    return ret_value;
} /* end H5VL__stream_build_manifest() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_space_1d_bounds
 *
 * Purpose:     M8.5. The 1-D element range [*start, *start + *count) space_id
 *              covers, for subscription routing (H5Fsubscribe()'s handler
 *              and the DsetWrite/Attr push hooks in
 *              H5VL__stream_replay_manifest()). H5Sget_select_bounds()
 *              writes one array entry per dimension, so for a SCALAR
 *              dataspace (rank 0 -- H5S_SCALAR, the natural shape for an
 *              attribute) it writes nothing at all into low[0]/high[0],
 *              which would otherwise be read as uninitialized stack
 *              garbage (found by actually running the attribute-push test
 *              this function exists for -- see project_m8_status.md).
 *              Scalar is treated as exactly one element at index 0.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_space_1d_bounds(hid_t space_id, uint64_t *start, uint64_t *count)
{
    if (H5Sget_simple_extent_type(space_id) == H5S_SCALAR) {
        *start = 0;
        *count = 1;
        return 0;
    }
    {
        hsize_t low[32], high[32];

        if (H5Sget_select_bounds(space_id, low, high) < 0)
            return -1;
        *start = (uint64_t)low[0];
        *count = (uint64_t)(high[0] - low[0] + 1);
    }
    return 0;
} /* end H5VL__stream_space_1d_bounds() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_replay_manifest
 *
 * Purpose:     The M2 core (M7: the second half of what was
 *              H5VL__stream_replay_step(), now reusable). Decodes
 *              manifest_buf and replays it -- creating real objects
 *              group-based under /step/<physical_step>/ (physical_step read
 *              from the *decoded* manifest, not passed in, so this always
 *              targets exactly the step the manifest itself claims) from
 *              the decoded ids, never any live ones. That decode round trip
 *              is the point: it is what proves the manifest -- not just the
 *              connector's own bookkeeping -- faithfully reproduces HDF5's
 *              data model (types, selections, chunking/filters).
 *
 *              Entries replay in original capture order. A DsetWrite or Attr
 *              entry always has a DsetCreate entry earlier in the same pass:
 *              a placeholder (the only source of either) can only be created
 *              by dataset_create()/attr_create() while IN_STEP. Replay finds
 *              it by scanning already-replayed entries for a matching path
 *              rather than needing a separate index.
 *
 *              pending_for_wiring/n_pending_for_wiring wire a DsetCreate/Attr
 *              entry's replayed object into the application's still-open
 *              placeholder handle (see H5VL_stream_dataset_close()), index-
 *              for-index with the manifest's own entries -- only valid when
 *              this manifest was JUST built from that exact pending buffer
 *              (the normal, same-step case: pass fs->pending/fs->n_pending).
 *              Pass NULL/0 when replaying a manifest recovered from
 *              elsewhere (M7's spill drain, H5VL__stream_drain_spill()) --
 *              every replayed object is then closed immediately instead,
 *              which M7's H5VL__stream_apply_queue_policy() guarantees is
 *              correct by never spilling or discarding a step that has an
 *              open placeholder in the first place (see its comment).
 *
 *              M6: this function is unchanged for a parallel (has_comm)
 *              writer -- it is simply called once per rank, each replaying
 *              its own pending[] against the same collectively-opened
 *              underlying file. That is sufficient, not a shortcut,
 *              *given* this milestone's scope decision: every rank
 *              creates the same set of objects (the ordinary parallel-HDF5
 *              pattern of all ranks calling H5Dcreate2()/H5Acreate2() etc.
 *              with matching arguments), varying only in which hyperslab
 *              each rank's own DsetWrite entries cover. Under that
 *              assumption, N ranks independently replaying identical
 *              create-entries against a shared file *is* the correct,
 *              standard parallel-HDF5 collective-create pattern -- HDF5
 *              itself coordinates it into one shared object, not this
 *              connector. Each rank's writes are independent (no DXPL
 *              collective I/O requested), which is safe because writer
 *              ranks' hyperslabs do not overlap. Heterogeneous per-rank
 *              object sets -- rank 0 creating a dataset rank 1 never
 *              touches -- are out of scope for this increment; that needs
 *              real cross-rank manifest aggregation (H5Sselect_project_
 *              intersection, per dev-plan.md's M6 section) and is where
 *              the Subfiling-style I/O-concentrator topology belongs.
 *              See H5VL_stream_file_optional()'s begin_step/end_step
 *              handling for the collective barriers around this call.
 *
 * Return:      Success:    0
 *              Failure:    -1 (the caller discards the pending buffer either
 *                          way -- a partial replay is not salvaged)
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_replay_manifest(H5VL_stream_t *file_obj, const uint8_t *manifest_buf, size_t manifest_len,
                               const uint8_t *payload_buf, H5VL_stream_pending_entry_t *pending_for_wiring,
                               size_t n_pending_for_wiring)
{
    H5VL_stream_file_state_t *fs           = file_obj->file_state;
    void                     **replay_under = NULL;
    uint8_t                  *needs_close  = NULL;
    char                      *step_root    = NULL;
    size_t                     payload_len  = 0;
    uint64_t                   physical_step = 0;
    size_t                     i;
    herr_t                     ret_value = 0;

    /* Pass 2: decode the manifest and replay it entry by entry, using only
     * the decoded ids -- see the function comment above. */
    {
        vs_Step_table_t step      = vs_Step_as_root(manifest_buf);
        vs_Entry_vec_t  entries   = step ? vs_Step_entries(step) : NULL;
        size_t          n_entries = entries ? vs_Entry_vec_len(entries) : 0;

        if (!step || (pending_for_wiring && n_entries != n_pending_for_wiring)) {
            ret_value = -1;
            goto done;
        }

        /* dev-plan.md's residual risks: H5Tdecode2()/H5Sdecode() below lean
         * on HDF5's own encoding compatibility, which is not guaranteed
         * across a minor version bump (H5Sencode2() itself widened to
         * 64-bit selections between 1.10 and 1.12 -- the precedent this
         * schema's hdf5_version field exists to catch). Compare at
         * major.minor granularity, ignoring the release/patch digits
         * (HDF5's versioning policy never changes on-disk/encoding format
         * in a patch release): a mismatch here is refused with a clear
         * diagnostic before either decode call runs, rather than risking
         * whatever H5Tdecode2()/H5Sdecode() do with bytes from an
         * incompatible encoder. written_ver encodes maj*1e6 + minor*1e3 +
         * release, the same packing H5VL__stream_build_manifest() (and its
         * M6.5/M7 siblings) used to write it. */
        {
            uint32_t written_ver = vs_Step_hdf5_version(step);
            unsigned  cur_maj, cur_minor, cur_rel;

            H5get_libversion(&cur_maj, &cur_minor, &cur_rel);

            if (written_ver / 1000u != ((uint32_t)cur_maj * 1000u + (uint32_t)cur_minor)) {
                fprintf(stderr,
                        "vol-stream: manifest was written by HDF5 %u.%u.x, this process is running HDF5 "
                        "%u.%u.%u -- refusing to decode (H5Tdecode2()/H5Sdecode() compatibility is not "
                        "guaranteed across a minor version change; see docs/dev-plan.md's residual risks)\n",
                        written_ver / 1000000u, (written_ver / 1000u) % 1000u, cur_maj, cur_minor, cur_rel);
                ret_value = -1;
                goto done;
            }
        }

        /* Schema-recorded, not recomputed from entries: build_manifest()
         * writes it once as the authoritative total (dev-plan.md's Step
         * manifest section), and this is exactly the value the persistence
         * block below needs for the .payload dataspace. */
        payload_len   = (size_t)vs_Step_payload_bytes(step);
        physical_step = vs_Step_physical_step(step);

        if (NULL == (step_root = (char *)malloc(32))) { /* "/step/" + up to 20 digits + '\0' */
            ret_value = -1;
            goto done;
        }
        snprintf(step_root, 32, "/step/%llu", (unsigned long long)physical_step);

        if (n_entries > 0) {
            if (NULL == (replay_under = (void **)calloc(n_entries, sizeof(void *))) ||
                NULL == (needs_close = (uint8_t *)calloc(n_entries, sizeof(uint8_t)))) {
                ret_value = -1;
                goto done;
            }
        }

        for (i = 0; i < n_entries; i++) {
            vs_Entry_table_t        e     = vs_Entry_vec_at(entries, i);
            vs_Kind_enum_t           kind  = vs_Entry_kind(e);
            const char              *path  = vs_Entry_path(e);
            flatbuffers_uint8_vec_t  type_enc  = vs_Entry_type_enc(e);
            flatbuffers_uint8_vec_t  space_enc = vs_Entry_space_enc(e);
            flatbuffers_uint8_vec_t  dcpl_enc  = vs_Entry_dcpl_enc(e);
            uint64_t                 poff  = vs_Entry_payload_off(e);
            uint64_t                 plen  = vs_Entry_payload_len(e);
            hid_t                    dtype = -1, dspace = -1, ddcpl = -1;
            const uint8_t           *type_enc_ptr = type_enc;
            size_t                   type_enc_actual_len = flatbuffers_uint8_vec_len(type_enc);

            /* H5VL__stream_type_cache_lookup()'s comment: a DsetWrite entry
             * with an empty type_enc means the writer omitted it because it
             * was identical to the last one sent for this path -- resolve
             * it from this process's own mirror of that same cache instead
             * (this always runs in the same process, immediately after
             * H5VL__stream_build_manifest() built the buffer being decoded
             * here -- see this function's own comment on why "reader" does
             * not mean a separate process for this call). */
            if (kind == vs_Kind_DsetWrite && type_enc_actual_len == 0) {
                type_enc_ptr = H5VL__stream_type_cache_lookup(fs->replay_type_cache, fs->n_replay_type_cache,
                                                                path, &type_enc_actual_len);
                if (!type_enc_ptr) {
                    ret_value = -1;
                    goto done;
                }
            }

            /* Record where this object physically landed, the same way
             * H5VL__stream_reader_index_one_step() does for a reader. A
             * writer needs the identical logical -> "/step/<k>/..." mapping
             * to resolve an H5R reference target: the application names
             * "/target", but the object only ever exists at
             * "/step/<k>/target", and only from the step that created it
             * onward. See H5VL__stream_resolve_physical_path(). */
            if ((kind == vs_Kind_DsetCreate || kind == vs_Kind_Attr) && path &&
                H5VL__stream_path_index_add(fs, path, physical_step) < 0) {
                ret_value = -1;
                goto done;
            }

            if ((dtype = H5Tdecode2(type_enc_ptr, type_enc_actual_len)) < 0 ||
                (dspace = H5Sdecode(space_enc)) < 0) {
                if (dtype >= 0)
                    H5Tclose(dtype);
                ret_value = -1;
                goto done;
            }

            if (kind == vs_Kind_DsetWrite && flatbuffers_uint8_vec_len(type_enc) > 0 &&
                H5VL__stream_type_cache_upsert(&fs->replay_type_cache, &fs->n_replay_type_cache,
                                                 &fs->cap_replay_type_cache, path, type_enc,
                                                 flatbuffers_uint8_vec_len(type_enc)) < 0) {
                H5Tclose(dtype);
                H5Sclose(dspace);
                ret_value = -1;
                goto done;
            }
            if ((kind == vs_Kind_DsetCreate || kind == vs_Kind_Attr) && dcpl_enc &&
                flatbuffers_uint8_vec_len(dcpl_enc) > 0) {
                if ((ddcpl = H5Pdecode(dcpl_enc)) < 0) {
                    H5Tclose(dtype);
                    H5Sclose(dspace);
                    ret_value = -1;
                    goto done;
                }
            }

            if (kind == vs_Kind_DsetCreate) {
                char              *full_path;
                size_t             full_len = strlen(step_root) + strlen(path) + 1;
                H5VL_loc_params_t  loc_params;
                void              *real;

                if (NULL == (full_path = (char *)malloc(full_len))) {
                    H5Tclose(dtype);
                    H5Sclose(dspace);
                    if (ddcpl >= 0)
                        H5Pclose(ddcpl);
                    ret_value = -1;
                    goto done;
                }
                snprintf(full_path, full_len, "%s%s", step_root, path);

                memset(&loc_params, 0, sizeof(loc_params));
                loc_params.obj_type = H5I_FILE;
                loc_params.type     = H5VL_OBJECT_BY_SELF;

                {
                    char *last_slash = strrchr(full_path, '/');
                    void *parent;

                    *last_slash = '\0'; /* split into ancestor path + leaf name */

                    parent = H5VL__stream_replay_ensure_group(file_obj->under_object, file_obj->under_vol_id,
                                                               full_path);
                    *last_slash = '/';

                    if (!parent) {
                        free(full_path);
                        H5Tclose(dtype);
                        H5Sclose(dspace);
                        if (ddcpl >= 0)
                            H5Pclose(ddcpl);
                        ret_value = -1;
                        goto done;
                    }

                    loc_params.obj_type = H5I_GROUP;

                    real = H5VLdataset_create(parent, &loc_params, file_obj->under_vol_id, last_slash + 1,
                                              H5P_LINK_CREATE_DEFAULT, dtype, dspace,
                                              ddcpl >= 0 ? ddcpl : H5P_DATASET_CREATE_DEFAULT,
                                              H5P_DATASET_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, NULL);
                    H5VLgroup_close(parent, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
                }
                free(full_path);

                if (!real) {
                    H5Tclose(dtype);
                    H5Sclose(dspace);
                    if (ddcpl >= 0)
                        H5Pclose(ddcpl);
                    ret_value = -1;
                    goto done;
                }

                replay_under[i] = real;

                if (pending_for_wiring && pending_for_wiring[i].owner_wrapper) {
                    pending_for_wiring[i].owner_wrapper->under_object = real;
                    pending_for_wiring[i].owner_wrapper->obj_state    = H5VL_STREAM_OBJ_LIVE;
                }
                else
                    needs_close[i] = 1;
            }
            else if (kind == vs_Kind_DsetWrite) {
                void                   *real = NULL;
                size_t                  j;
                hid_t                   mem_space = -1;
                hssize_t                n_elem;
                flatbuffers_uint8_vec_t create_dcpl_enc     = NULL;
                size_t                  create_dcpl_enc_len = 0;

                /* Also picks up the creating entry's own dcpl_enc: a
                 * DsetWrite entry carries no DCPL of its own (see
                 * H5VL_stream_pending_entry_t), so the pipeline this data
                 * actually landed under -- what M8.5.1's zero-copy fast path
                 * has to compare a subscriber's request against -- is only
                 * available from the DsetCreate being resolved here. */
                for (j = i; j-- > 0;)
                    if (vs_Entry_kind(vs_Entry_vec_at(entries, j)) == vs_Kind_DsetCreate &&
                        strcmp(vs_Entry_path(vs_Entry_vec_at(entries, j)), path) == 0) {
                        vs_Entry_table_t ce = vs_Entry_vec_at(entries, j);

                        real                = replay_under[j];
                        create_dcpl_enc     = vs_Entry_dcpl_enc(ce);
                        create_dcpl_enc_len = flatbuffers_uint8_vec_len(create_dcpl_enc);
                        break;
                    }

                if (!real || (n_elem = H5Sget_select_npoints(dspace)) < 0) {
                    H5Tclose(dtype);
                    H5Sclose(dspace);
                    ret_value = -1;
                    goto done;
                }

                {
                    hsize_t n_elem_h = (hsize_t)n_elem;

                    if ((mem_space = H5Screate_simple(1, &n_elem_h, NULL)) < 0) {
                        H5Tclose(dtype);
                        H5Sclose(dspace);
                        ret_value = -1;
                        goto done;
                    }
                }

                {
                    const void *payload_ptr = payload_buf + poff;
                    herr_t      w = H5VLdataset_write(1, &real, file_obj->under_vol_id, &dtype, &mem_space,
                                                      &dspace, H5P_DATASET_XFER_DEFAULT, &payload_ptr, NULL);
                    H5Sclose(mem_space);
                    if (w < 0) {
                        H5Tclose(dtype);
                        H5Sclose(dspace);
                        ret_value = -1;
                        goto done;
                    }

#ifdef VOL_STREAM_HAVE_MERCURY
                    /* M8/M8.5: push the subset of this entry's bytes that
                     * overlaps each current subscriber's own requested
                     * range (see tr_mercury.c's header comment) -- 1-D
                     * element bounds of THIS write's own file-space
                     * selection, matching H5Fsubscribe()'s own
                     * H5Sget_select_bounds() use. vs_tr_writer_push_data()
                     * is itself a no-op when there is no transport or no
                     * subscriber overlapping this range, so this costs
                     * nothing on the ordinary M0-M7 path. Best-effort, same
                     * as vs_tr_writer_broadcast_step_ready(): a failed push
                     * must not fail the replay that already durably
                     * committed this data to the real file. */
                    if (file_obj->file_state && file_obj->file_state->transport) {
                        uint64_t w_start, w_count;

                        if (H5VL__stream_space_1d_bounds(dspace, &w_start, &w_count) >= 0) {
                            uint8_t *push_type_enc     = NULL;
                            size_t   push_type_enc_len = 0;
                            uint8_t *native_dcpl_enc     = NULL;
                            size_t   native_dcpl_enc_len = 0;

                            H5VL_stream_native_chunk_ctx_t native_ctx;

                            /* Only needed if a subscriber actually wants
                             * precision re-filtering (vs_tr_writer_push_
                             * data() hands this to the registered callback,
                             * see vs_tr_refilter_fn's comment) -- encoded
                             * unconditionally here since whether any
                             * subscriber wants it is tr_mercury.c's own
                             * per-subscriber decision, not something this
                             * call site can know in advance. */
                            H5VL__stream_encode_type(dtype, &push_type_enc, &push_type_enc_len);

                            /* M8.5.1: offer the chunk-level zero-copy fast
                             * path what it needs -- the DCPL this write
                             * actually landed under, and a borrowed handle on
                             * the real dataset holding the already-filtered
                             * bytes. Only worth building when the write was
                             * chunked and filtered; an unfiltered or
                             * contiguous dataset has no stored compressed
                             * chunk to serve, so the fast path could not
                             * apply anyway. Everything here is strictly
                             * optional: on any failure the push simply goes
                             * out with NULLs and the callback takes its
                             * ordinary temporary-dataset route. */
                            memset(&native_ctx, 0, sizeof(native_ctx));
                            if (create_dcpl_enc && create_dcpl_enc_len > 0) {
                                hid_t nat = H5Pdecode(create_dcpl_enc);

                                if (nat >= 0) {
                                    if (H5Pget_layout(nat) == H5D_CHUNKED && H5Pget_nfilters(nat) > 0) {
                                        /* The manifest's own bytes are already
                                         * exactly what the callback needs to
                                         * compare against -- no re-encode. */
                                        native_dcpl_enc     = (uint8_t *)create_dcpl_enc;
                                        native_dcpl_enc_len = create_dcpl_enc_len;

                                        native_ctx.under_dset   = real;
                                        native_ctx.under_vol_id = file_obj->under_vol_id;
                                        native_ctx.write_start  = w_start;
                                        native_ctx.total_elems  = (uint64_t)n_elem;
                                    }
                                    H5Pclose(nat);
                                }
                            }

                            vs_tr_writer_push_data(file_obj->file_state->transport, physical_step, path,
                                                     payload_ptr, H5Tget_size(dtype), w_start,
                                                     (uint64_t)n_elem, push_type_enc,
                                                     (uint64_t)push_type_enc_len, native_dcpl_enc,
                                                     (uint64_t)native_dcpl_enc_len,
                                                     native_ctx.under_dset ? &native_ctx : NULL);

                            /* native_dcpl_enc is deliberately not freed: it
                             * points into the manifest buffer, not to an
                             * allocation of this call's own. Nothing in
                             * native_ctx is owned here either. */
                            free(push_type_enc);
                        }
                    }
#endif
                    (void)plen;
                }
            }
            else if (kind == vs_Kind_Attr) {
                const char        *at = strrchr(path, '@');
                char              *parent_path;
                char              *full_parent;
                H5VL_loc_params_t  loc_params;
                void              *attr;

                if (!at) {
                    H5Tclose(dtype);
                    H5Sclose(dspace);
                    if (ddcpl >= 0)
                        H5Pclose(ddcpl);
                    ret_value = -1;
                    goto done;
                }

                if (NULL == (parent_path = (char *)malloc((size_t)(at - path) + 1))) {
                    H5Tclose(dtype);
                    H5Sclose(dspace);
                    if (ddcpl >= 0)
                        H5Pclose(ddcpl);
                    ret_value = -1;
                    goto done;
                }
                memcpy(parent_path, path, (size_t)(at - path));
                parent_path[at - path] = '\0';

                {
                    size_t full_parent_len = strlen(step_root) + strlen(parent_path) + 1;

                    if (NULL == (full_parent = (char *)malloc(full_parent_len))) {
                        free(parent_path);
                        H5Tclose(dtype);
                        H5Sclose(dspace);
                        if (ddcpl >= 0)
                            H5Pclose(ddcpl);
                        ret_value = -1;
                        goto done;
                    }
                    snprintf(full_parent, full_parent_len, "%s%s", step_root, parent_path);
                }
                free(parent_path);

                memset(&loc_params, 0, sizeof(loc_params));
                loc_params.obj_type                     = H5I_FILE;
                loc_params.type                         = H5VL_OBJECT_BY_NAME;
                loc_params.loc_data.loc_by_name.name    = full_parent;
                loc_params.loc_data.loc_by_name.lapl_id = H5P_LINK_ACCESS_DEFAULT;

                /* The first attempt is a routine probe whenever the parent
                 * hasn't been created by an earlier entry this pass (e.g. an
                 * attribute directly on the file root, or on a plain group)
                 * -- suppress it the same way
                 * H5VL__stream_replay_ensure_group() does. If it fails,
                 * materialize the parent group and retry for real, with
                 * normal error visibility. */
                {
                    H5E_auto2_t old_func;
                    void       *old_data;
                    hid_t       err_id;

                    H5Eget_auto2(H5E_DEFAULT, &old_func, &old_data);
                    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
                    err_id = H5Eget_current_stack();

                    attr = H5VLattr_create(file_obj->under_object, &loc_params, file_obj->under_vol_id, at + 1,
                                           dtype, dspace, ddcpl >= 0 ? ddcpl : H5P_ATTRIBUTE_CREATE_DEFAULT,
                                           H5P_ATTRIBUTE_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, NULL);

                    H5Eset_current_stack(err_id);
                    H5Eset_auto2(H5E_DEFAULT, old_func, old_data);
                }

                if (!attr) {
                    void *grp = H5VL__stream_replay_ensure_group(file_obj->under_object,
                                                                   file_obj->under_vol_id, full_parent);
                    if (grp) {
                        attr = H5VLattr_create(file_obj->under_object, &loc_params, file_obj->under_vol_id,
                                               at + 1, dtype, dspace,
                                               ddcpl >= 0 ? ddcpl : H5P_ATTRIBUTE_CREATE_DEFAULT,
                                               H5P_ATTRIBUTE_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, NULL);
                        H5VLgroup_close(grp, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
                    }
                }
                free(full_parent);

                if (!attr) {
                    H5Tclose(dtype);
                    H5Sclose(dspace);
                    if (ddcpl >= 0)
                        H5Pclose(ddcpl);
                    ret_value = -1;
                    goto done;
                }

                if (plen > 0) {
                    const void *payload_ptr = payload_buf + poff;

                    if (H5VLattr_write(attr, file_obj->under_vol_id, dtype, payload_ptr,
                                       H5P_DATASET_XFER_DEFAULT, NULL) < 0) {
                        H5VLattr_close(attr, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
                        H5Tclose(dtype);
                        H5Sclose(dspace);
                        if (ddcpl >= 0)
                            H5Pclose(ddcpl);
                        ret_value = -1;
                        goto done;
                    }

#ifdef VOL_STREAM_HAVE_MERCURY
                    /* M8.5: same push as a DsetWrite entry gets, for the same
                     * reason -- a subscription to an attribute path
                     * previously validated fine but silently never received
                     * anything. path is the "@"-joined internal form
                     * H5VL__stream_attr_path() builds (e.g. "/group@name"),
                     * so a subscriber wanting attribute data must subscribe
                     * using that same form -- not documented as public API
                     * yet, since M8/M8.5 never claimed attribute
                     * subscriptions as in scope; this closes the silent gap
                     * without promising more than that. */
                    if (file_obj->file_state && file_obj->file_state->transport) {
                        uint64_t a_start, a_count;

                        if (H5VL__stream_space_1d_bounds(dspace, &a_start, &a_count) >= 0) {
                            uint8_t *push_type_enc     = NULL;
                            size_t   push_type_enc_len = 0;

                            H5VL__stream_encode_type(dtype, &push_type_enc, &push_type_enc_len);
                            /* No native chunk context: an attribute has no
                             * chunked/filtered storage to serve bytes from,
                             * so the zero-copy fast path never applies here
                             * and a subscriber's precision request always
                             * takes the temporary-dataset route. */
                            vs_tr_writer_push_data(file_obj->file_state->transport, physical_step, path,
                                                     payload_ptr, H5Tget_size(dtype), a_start, a_count,
                                                     push_type_enc, (uint64_t)push_type_enc_len, NULL, 0,
                                                     NULL);
                            free(push_type_enc);
                        }
                    }
#endif
                }

                if (pending_for_wiring && pending_for_wiring[i].owner_wrapper) {
                    pending_for_wiring[i].owner_wrapper->under_object = attr;
                    pending_for_wiring[i].owner_wrapper->obj_state    = H5VL_STREAM_OBJ_LIVE;
                }
                else
                    H5VLattr_close(attr, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
            }

            H5Tclose(dtype);
            H5Sclose(dspace);
            if (ddcpl >= 0)
                H5Pclose(ddcpl);
        }

        /* Close every replayed dataset the application is not still holding
         * open via a placeholder handle -- see H5VL_stream_dataset_close(). */
        for (i = 0; i < n_entries; i++)
            if (needs_close[i] && replay_under[i])
                H5VLdataset_close(replay_under[i], file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
    }

    /* Persist the manifest and payload themselves, for inspectability and
     * future transport reuse -- after every real entry, so they are never
     * part of what they describe. H5VL__stream_replay_ensure_group() covers
     * the empty-step case (no entries at all, so /step/<n>/ was never
     * otherwise created). */
    {
        H5VL_loc_params_t loc_params;
        hid_t              opaque_type = -1;
        void              *step_group  = NULL;
        char               name[32];

        memset(&loc_params, 0, sizeof(loc_params));
        loc_params.obj_type = H5I_GROUP;
        loc_params.type     = H5VL_OBJECT_BY_SELF;

        if (NULL == (step_group = H5VL__stream_replay_ensure_group(file_obj->under_object,
                                                                     file_obj->under_vol_id, step_root)) ||
            (opaque_type = H5Tcreate(H5T_OPAQUE, 1)) < 0) {
            if (step_group)
                H5VLgroup_close(step_group, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
            ret_value = -1;
            goto done;
        }

        {
            hsize_t dims[1] = {(hsize_t)manifest_len};
            hid_t   space   = H5Screate_simple(1, dims, NULL);
            void   *mds;

            H5Tset_tag(opaque_type, "vol-stream manifest v1 (flatcc)");
            snprintf(name, sizeof(name), ".manifest");
            mds = H5VLdataset_create(step_group, &loc_params, file_obj->under_vol_id, name,
                                     H5P_LINK_CREATE_DEFAULT, opaque_type, space, H5P_DATASET_CREATE_DEFAULT,
                                     H5P_DATASET_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, NULL);
            if (mds) {
                if (manifest_len > 0) {
                    const void *bufp = manifest_buf;
                    H5VLdataset_write(1, &mds, file_obj->under_vol_id, &opaque_type, &space, &space,
                                      H5P_DATASET_XFER_DEFAULT, &bufp, NULL);
                }
                H5VLdataset_close(mds, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
            }
            H5Sclose(space);
        }

        {
            hsize_t dims[1] = {(hsize_t)payload_len};
            hid_t   space   = H5Screate_simple(1, dims, NULL);
            void   *pds;

            H5Tset_tag(opaque_type, "vol-stream payload v1 (raw bytes, offsets in .manifest)");
            snprintf(name, sizeof(name), ".payload");
            pds = H5VLdataset_create(step_group, &loc_params, file_obj->under_vol_id, name,
                                     H5P_LINK_CREATE_DEFAULT, opaque_type, space, H5P_DATASET_CREATE_DEFAULT,
                                     H5P_DATASET_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, NULL);
            if (pds) {
                if (payload_len > 0) {
                    const void *bufp = payload_buf;
                    H5VLdataset_write(1, &pds, file_obj->under_vol_id, &opaque_type, &space, &space,
                                      H5P_DATASET_XFER_DEFAULT, &bufp, NULL);
                }
                H5VLdataset_close(pds, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
            }
            H5Sclose(space);
        }

        H5VLgroup_close(step_group, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);

        H5Tclose(opaque_type);
    }

done:
    free(replay_under);
    free(needs_close);
    free(step_root);

    return ret_value;
} /* end H5VL__stream_replay_manifest() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_replay_step
 *
 * Purpose:     M0-M6 behavior, unchanged: build this step's manifest and
 *              replay it immediately. What M7 adds lives in
 *              H5VL__stream_apply_queue_policy(), which calls this only
 *              when there is no reason to Block/Discard/Spill instead --
 *              see its comment. Kept as a separate, trivial function (not
 *              inlined into the caller) so every existing call site and
 *              this function's own long-standing doc comment on the
 *              M2-M6 replay design keep meaning exactly what they did
 *              before M7.
 *
 * Return:      Success:    0
 *              Failure:    -1 (the caller discards the pending buffer either
 *                          way -- a partial replay is not salvaged)
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_replay_step(H5VL_stream_t *file_obj)
{
    H5VL_stream_file_state_t *fs = file_obj->file_state;
    uint8_t                   *manifest_buf = NULL;
    size_t                     manifest_len = 0;
    uint8_t                   *payload_buf  = NULL;
    size_t                     payload_len  = 0;
    herr_t                     ret_value;

    if (H5VL__stream_build_manifest(fs, &manifest_buf, &manifest_len, &payload_buf, &payload_len) < 0)
        return -1;

    ret_value = H5VL__stream_replay_manifest(file_obj, manifest_buf, manifest_len, payload_buf, fs->pending,
                                               fs->n_pending);

    flatcc_builder_free(manifest_buf);
    free(payload_buf);

    return ret_value;
} /* end H5VL__stream_replay_step() */

#ifdef H5_HAVE_PARALLEL
/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_build_agg_manifest
 *
 * Purpose:     M6.5. The subset of H5VL__stream_build_manifest()'s pass 1
 *              that a heterogeneous parallel writer's ranks must exchange
 *              before they can replay collectively: DsetCreate entries
 *              (metadata only -- they never carry payload) and Attr entries
 *              (WITH payload, since H5Acreate/H5Awrite are collective
 *              metadata operations too, so every rank must replay the exact
 *              same attribute value, not just agree it exists). DsetWrite
 *              entries are deliberately excluded -- raw dataset I/O is
 *              independent, needs no cross-rank agreement, and its payload
 *              can be arbitrarily large, so it stays local and is replayed
 *              by H5VL__stream_replay_local_writes() instead, after this
 *              aggregated pass has ensured the target objects exist.
 *
 * Return:      Success:    0, with *out_buf/*out_payload_buf newly
 *                          allocated (caller frees: flatcc_builder_free()
 *                          for the former, free() for the latter)
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_build_agg_manifest(H5VL_stream_file_state_t *fs, uint8_t **out_buf, size_t *out_len,
                                  uint8_t **out_payload_buf, size_t *out_payload_len)
{
    flatcc_builder_t B;
    int               builder_ready = 0;
    vs_Entry_ref_t   *entry_refs    = NULL;
    size_t            n_refs        = 0;
    uint8_t          *payload_buf   = NULL;
    size_t            payload_cap   = 0;
    size_t            payload_len   = 0;
    uint8_t          *manifest_buf  = NULL;
    size_t            manifest_len  = 0;
    unsigned          maj, minor, rel;
    size_t            i;
    herr_t            ret_value = 0;

    if (NULL == (entry_refs = (vs_Entry_ref_t *)malloc((fs->n_pending ? fs->n_pending : 1) * sizeof(vs_Entry_ref_t)))) {
        ret_value = -1;
        goto done;
    }

    flatcc_builder_init(&B);
    builder_ready = 1;

    for (i = 0; i < fs->n_pending; i++) {
        H5VL_stream_pending_entry_t *pe = &fs->pending[i];
        uint8_t                     *type_enc  = NULL;
        uint8_t                     *space_enc = NULL;
        uint8_t                     *dcpl_enc  = NULL;
        size_t                       type_len  = 0;
        size_t                       space_len = 0;
        size_t                       dcpl_len  = 0;
        uint64_t                     this_off  = (uint64_t)payload_len;
        uint64_t                     this_len  = 0;

        if (pe->kind != vs_Kind_DsetCreate && pe->kind != vs_Kind_Attr)
            continue; /* DsetWrite -- stays local, see this function's comment */

        if (H5VL__stream_encode_type(pe->type_id, &type_enc, &type_len) < 0 ||
            H5VL__stream_encode_space(pe->space_id, &space_enc, &space_len) < 0 ||
            (pe->dcpl_id >= 0 && H5VL__stream_encode_dcpl(pe->dcpl_id, &dcpl_enc, &dcpl_len) < 0)) {
            free(type_enc);
            free(space_enc);
            free(dcpl_enc);
            ret_value = -1;
            goto done;
        }

        if (pe->kind == vs_Kind_Attr && pe->payload_len > 0) {
            if (payload_len + pe->payload_len > payload_cap) {
                size_t   new_cap = payload_cap ? payload_cap * 2 : 4096;
                uint8_t *grown;

                while (new_cap < payload_len + pe->payload_len)
                    new_cap *= 2;
                if (NULL == (grown = (uint8_t *)realloc(payload_buf, new_cap))) {
                    free(type_enc);
                    free(space_enc);
                    free(dcpl_enc);
                    ret_value = -1;
                    goto done;
                }
                payload_buf = grown;
                payload_cap = new_cap;
            }
            memcpy(payload_buf + payload_len, pe->payload, pe->payload_len);
            payload_len += pe->payload_len;
            this_len = (uint64_t)pe->payload_len;
        }

        vs_Entry_start(&B);
        vs_Entry_kind_add(&B, (vs_Kind_enum_t)pe->kind);
        vs_Entry_path_create_str(&B, pe->path);
        vs_Entry_type_enc_create(&B, type_enc, type_len);
        vs_Entry_space_enc_create(&B, space_enc, space_len);
        if (dcpl_enc)
            vs_Entry_dcpl_enc_create(&B, dcpl_enc, dcpl_len);
        vs_Entry_form_add(&B, vs_Payload_Raw);
        vs_Entry_payload_off_add(&B, this_off);
        vs_Entry_payload_len_add(&B, this_len);
        entry_refs[n_refs++] = vs_Entry_end(&B);

        free(type_enc);
        free(space_enc);
        free(dcpl_enc);
    }

    H5get_libversion(&maj, &minor, &rel);

    vs_Step_start_as_root(&B);
    vs_Step_physical_step_add(&B, fs->physical_step);
    vs_Step_wall_time_ns_add(&B, fs->wall_time_ns);
    vs_Step_hdf5_version_add(&B, (uint32_t)(maj * 1000000u + minor * 1000u + rel));
    if (n_refs > 0)
        vs_Step_entries_create(&B, entry_refs, n_refs);
    vs_Step_payload_bytes_add(&B, (uint64_t)payload_len);
    vs_Step_end_as_root(&B);

    if (NULL == (manifest_buf = (uint8_t *)flatcc_builder_finalize_buffer(&B, &manifest_len))) {
        ret_value = -1;
        goto done;
    }

    *out_buf          = manifest_buf;
    *out_len          = manifest_len;
    *out_payload_buf  = payload_buf;
    *out_payload_len  = payload_len;
    manifest_buf      = NULL;
    payload_buf       = NULL;

done:
    if (manifest_buf)
        flatcc_builder_free(manifest_buf);
    if (builder_ready)
        flatcc_builder_clear(&B);
    free(entry_refs);
    free(payload_buf);

    return ret_value;
} /* end H5VL__stream_build_agg_manifest() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_merge_agg_manifests
 *
 * Purpose:     M6.5. bufs[r]/lens[r] and payload_bufs[r]/payload_lens[r] are
 *              every rank's own H5VL__stream_build_agg_manifest() output
 *              (rank r's slot always at index r, an MPI_Allgatherv
 *              guarantee this function's caller relies on). Every rank
 *              calls this with identical input, so every rank's merge
 *              produces an identical result -- no further communication
 *              needed for the merge itself, only for gathering the raw
 *              per-rank data beforehand.
 *
 *              Walks ranks in ascending order, entries within a rank in
 *              original order, and deduplicates by (kind, path): the first
 *              occurrence wins, later ones (whether a genuine duplicate
 *              create from the ordinary "every rank creates the same
 *              object" case, or a same-path Attr with a different value
 *              from a different rank) are dropped. This is "first-seen-
 *              wins" for both DsetCreate and Attr -- for DsetCreate,
 *              harmless (a well-formed application creates the same shared
 *              object identically from every rank that touches it); for
 *              Attr, a real simplifying assumption for divergent per-rank
 *              values, documented in project_m6_status.md.
 *
 * Return:      Success:    0, with *out_merged_buf/*out_merged_payload/
 *                          *out_wiring newly allocated (caller frees:
 *                          flatcc_builder_free() for the first, free() for
 *                          the other two)
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_merge_agg_manifests(H5VL_stream_file_state_t *fs, uint8_t **bufs, int *lens,
                                   uint8_t **payload_bufs, int *payload_lens, int nranks, int my_rank,
                                   uint8_t **out_merged_buf, size_t *out_merged_len,
                                   uint8_t **out_merged_payload, size_t *out_merged_payload_len,
                                   H5VL_stream_pending_entry_t **out_wiring, size_t *out_n_wiring)
{
    typedef struct { int kind; char *path; } H5VL_stream_seen_key_t;

    flatcc_builder_t                 B;
    int                                builder_ready = 0;
    vs_Entry_ref_t                   *entry_refs = NULL;
    size_t                             cap_refs = 0, n_refs = 0;
    uint8_t                          *merged_payload = NULL;
    size_t                             merged_payload_cap = 0, merged_payload_len = 0;
    H5VL_stream_pending_entry_t     *wiring = NULL;
    size_t                             cap_wiring = 0, n_wiring = 0;
    H5VL_stream_seen_key_t          *seen = NULL;
    size_t                             n_seen = 0, cap_seen = 0;
    uint8_t                          *merged_buf = NULL;
    size_t                             merged_len = 0;
    unsigned                           maj, minor, rel;
    int                                 r;
    herr_t                             ret_value = 0;

    flatcc_builder_init(&B);
    builder_ready = 1;

    for (r = 0; r < nranks; r++) {
        vs_Step_table_t step;
        vs_Entry_vec_t  entries;
        size_t          n_entries, i;
        const uint8_t  *payload_buf_r = payload_bufs[r];

        if (lens[r] <= 0)
            continue;
        step    = vs_Step_as_root(bufs[r]);
        entries = step ? vs_Step_entries(step) : NULL;
        n_entries = entries ? vs_Entry_vec_len(entries) : 0;

        for (i = 0; i < n_entries; i++) {
            vs_Entry_table_t        e     = vs_Entry_vec_at(entries, i);
            vs_Kind_enum_t           kind  = vs_Entry_kind(e);
            const char              *path  = vs_Entry_path(e);
            flatbuffers_uint8_vec_t  type_enc  = vs_Entry_type_enc(e);
            flatbuffers_uint8_vec_t  space_enc = vs_Entry_space_enc(e);
            flatbuffers_uint8_vec_t  dcpl_enc  = vs_Entry_dcpl_enc(e);
            uint64_t                 poff  = vs_Entry_payload_off(e);
            uint64_t                 plen  = vs_Entry_payload_len(e);
            uint64_t                 new_off;
            size_t                   si;
            int                      dup = 0;

            for (si = 0; si < n_seen; si++)
                if (seen[si].kind == (int)kind && strcmp(seen[si].path, path) == 0) {
                    dup = 1;
                    break;
                }
            if (dup)
                continue;

            if (n_seen == cap_seen) {
                size_t                    new_cap = cap_seen ? cap_seen * 2 : 16;
                H5VL_stream_seen_key_t *grown   = (H5VL_stream_seen_key_t *)realloc(seen, new_cap * sizeof(*seen));

                if (!grown) {
                    ret_value = -1;
                    goto done;
                }
                seen     = grown;
                cap_seen = new_cap;
            }
            seen[n_seen].kind = (int)kind;
            if (NULL == (seen[n_seen].path = strdup(path))) {
                ret_value = -1;
                goto done;
            }
            n_seen++;

            new_off = (uint64_t)merged_payload_len;
            if (plen > 0) {
                if (merged_payload_len + plen > merged_payload_cap) {
                    size_t   new_cap = merged_payload_cap ? merged_payload_cap * 2 : 4096;
                    uint8_t *grown;

                    while (new_cap < merged_payload_len + plen)
                        new_cap *= 2;
                    if (NULL == (grown = (uint8_t *)realloc(merged_payload, new_cap))) {
                        ret_value = -1;
                        goto done;
                    }
                    merged_payload = grown;
                    merged_payload_cap = new_cap;
                }
                memcpy(merged_payload + merged_payload_len, payload_buf_r + poff, plen);
                merged_payload_len += plen;
            }

            vs_Entry_start(&B);
            vs_Entry_kind_add(&B, kind);
            vs_Entry_path_create_str(&B, path);
            vs_Entry_type_enc_create(&B, type_enc, flatbuffers_uint8_vec_len(type_enc));
            vs_Entry_space_enc_create(&B, space_enc, flatbuffers_uint8_vec_len(space_enc));
            if (dcpl_enc && flatbuffers_uint8_vec_len(dcpl_enc) > 0)
                vs_Entry_dcpl_enc_create(&B, dcpl_enc, flatbuffers_uint8_vec_len(dcpl_enc));
            vs_Entry_form_add(&B, vs_Payload_Raw);
            vs_Entry_payload_off_add(&B, new_off);
            vs_Entry_payload_len_add(&B, plen);

            if (n_refs == cap_refs) {
                size_t          new_cap = cap_refs ? cap_refs * 2 : 16;
                vs_Entry_ref_t *grown   = (vs_Entry_ref_t *)realloc(entry_refs, new_cap * sizeof(*entry_refs));

                if (!grown) {
                    ret_value = -1;
                    goto done;
                }
                entry_refs = grown;
                cap_refs   = new_cap;
            }
            entry_refs[n_refs] = vs_Entry_end(&B);

            /* Wiring: only for entries this rank itself originated -- every
             * other slot stays zeroed (NULL owner_wrapper), which
             * H5VL__stream_replay_manifest() already treats as "close
             * immediately", exactly correct since only the originating
             * rank ever has an application-level placeholder handle open
             * for it. See H5VL__stream_replay_manifest()'s pending_for_
             * wiring comment. */
            if (n_wiring == cap_wiring) {
                size_t                          new_cap = cap_wiring ? cap_wiring * 2 : 16;
                H5VL_stream_pending_entry_t *grown   =
                    (H5VL_stream_pending_entry_t *)realloc(wiring, new_cap * sizeof(*wiring));

                if (!grown) {
                    ret_value = -1;
                    goto done;
                }
                wiring     = grown;
                cap_wiring = new_cap;
            }
            memset(&wiring[n_wiring], 0, sizeof(wiring[n_wiring]));
            if (r == my_rank) {
                size_t pj;

                for (pj = 0; pj < fs->n_pending; pj++)
                    if (fs->pending[pj].kind == (int)kind && strcmp(fs->pending[pj].path, path) == 0) {
                        wiring[n_wiring].owner_wrapper = fs->pending[pj].owner_wrapper;
                        break;
                    }
            }
            n_wiring++;
            n_refs++;
        }
    }

    H5get_libversion(&maj, &minor, &rel);

    vs_Step_start_as_root(&B);
    vs_Step_physical_step_add(&B, fs->physical_step);
    vs_Step_wall_time_ns_add(&B, fs->wall_time_ns);
    vs_Step_hdf5_version_add(&B, (uint32_t)(maj * 1000000u + minor * 1000u + rel));
    if (n_refs > 0)
        vs_Step_entries_create(&B, entry_refs, n_refs);
    vs_Step_payload_bytes_add(&B, (uint64_t)merged_payload_len);
    vs_Step_end_as_root(&B);

    if (NULL == (merged_buf = (uint8_t *)flatcc_builder_finalize_buffer(&B, &merged_len))) {
        ret_value = -1;
        goto done;
    }

    *out_merged_buf         = merged_buf;
    *out_merged_len         = merged_len;
    *out_merged_payload     = merged_payload;
    *out_merged_payload_len = merged_payload_len;
    *out_wiring             = wiring;
    *out_n_wiring           = n_wiring;
    merged_buf     = NULL;
    merged_payload = NULL;
    wiring         = NULL;

done:
    if (merged_buf)
        flatcc_builder_free(merged_buf);
    if (builder_ready)
        flatcc_builder_clear(&B);
    free(entry_refs);
    free(merged_payload);
    free(wiring);
    {
        size_t si;

        for (si = 0; si < n_seen; si++)
            free(seen[si].path);
        free(seen);
    }

    return ret_value;
} /* end H5VL__stream_merge_agg_manifests() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_write_replica
 *
 * Purpose:     M6.5. Reopen "/step/<physical_step><rel_path>" by path and
 *              issue one real H5VLdataset_write() of payload against
 *              space_id/type_id. Factored out of H5VL__stream_replay_local_
 *              writes() so the concentrator path below
 *              (H5VL__stream_recv_and_write_entry()) can perform the exact
 *              same write on behalf of a payload that arrived over MPI
 *              instead of from this rank's own pending[] array -- the file
 *              ends up with byte-identical content either way, since the
 *              path/type/space/payload are all that ever mattered.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_write_replica(H5VL_stream_t *file_obj, uint64_t physical_step, const char *rel_path,
                            hid_t type_id, hid_t space_id, const void *payload)
{
    char               step_root[32];
    char              *full_path;
    size_t             full_len;
    H5VL_loc_params_t  loc_params;
    void              *real;
    hid_t              mem_space = -1;
    hssize_t           n_elem;
    herr_t             ret_value = 0;

    snprintf(step_root, sizeof(step_root), "/step/%llu", (unsigned long long)physical_step);

    full_len = strlen(step_root) + strlen(rel_path) + 1;
    if (NULL == (full_path = (char *)malloc(full_len)))
        return -1;
    snprintf(full_path, full_len, "%s%s", step_root, rel_path);

    memset(&loc_params, 0, sizeof(loc_params));
    loc_params.obj_type = H5I_FILE;
    loc_params.type     = H5VL_OBJECT_BY_SELF;

    real = H5VLdataset_open(file_obj->under_object, &loc_params, file_obj->under_vol_id, full_path,
                              H5P_DATASET_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, NULL);
    free(full_path);
    if (!real)
        return -1;

    if ((n_elem = H5Sget_select_npoints(space_id)) < 0) {
        H5VLdataset_close(real, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
        return -1;
    }
    {
        hsize_t n_elem_h = (hsize_t)n_elem;

        if ((mem_space = H5Screate_simple(1, &n_elem_h, NULL)) < 0) {
            H5VLdataset_close(real, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
            return -1;
        }
    }

    {
        herr_t w = H5VLdataset_write(1, &real, file_obj->under_vol_id, &type_id, &mem_space, &space_id,
                                      H5P_DATASET_XFER_DEFAULT, &payload, NULL);

        H5Sclose(mem_space);
        H5VLdataset_close(real, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);
        if (w < 0)
            ret_value = -1;
    }

    return ret_value;
} /* end H5VL__stream_write_replica() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_replay_local_writes
 *
 * Purpose:     M6.5. The independent half of a heterogeneous parallel
 *              writer's replay: this rank's own DsetWrite entries, applied
 *              after H5VL__stream_merge_agg_manifests()'s collective pass
 *              has ensured every target object exists (whether this rank
 *              or another one originated its creation). DsetWrite entries
 *              always have a matching DsetCreate earlier in the same step
 *              (the capture side never lets one exist otherwise), so
 *              H5VL__stream_write_replica()'s reopen-by-path always
 *              succeeds once the collective pass has run.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_replay_local_writes(H5VL_stream_t *file_obj, uint64_t physical_step)
{
    H5VL_stream_file_state_t *fs = file_obj->file_state;
    size_t                     i;

    for (i = 0; i < fs->n_pending; i++) {
        H5VL_stream_pending_entry_t *pe = &fs->pending[i];

        if (pe->kind != vs_Kind_DsetWrite)
            continue;

        if (H5VL__stream_write_replica(file_obj, physical_step, pe->path, pe->type_id, pe->space_id,
                                        pe->payload) < 0)
            return -1;
    }

    return 0;
} /* end H5VL__stream_replay_local_writes() */

/*-------------------------------------------------------------------------
 * H5VL__stream_concentration_factor: how many consecutive writer ranks
 * share one I/O concentrator (Subfiling VFD's sf_topology_t/
 * n_io_concentrators, mirrored here at the application level). 1 (the
 * unset default) means "no concentration", exactly M6.5's first
 * increment -- every rank still does its own raw-data I/O directly.
 * VOL_STREAM_CONCENTRATION opts in explicitly, same convention as
 * VOL_STREAM_NA/VOL_STREAM_SPILL_DIR, so this is fully backward
 * compatible unless requested.
 *-------------------------------------------------------------------------
 */
static int
H5VL__stream_concentration_factor(void)
{
    const char *s = getenv("VOL_STREAM_CONCENTRATION");
    long        v;

    if (!s || !*s)
        return 1;
    v = strtol(s, NULL, 10);
    return (v > 1) ? (int)v : 1;
} /* end H5VL__stream_concentration_factor() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_send_write_entry_to_concentrator
 *
 * Purpose:     M6.5 concentrator topology. Ship one DsetWrite pending
 *              entry to the rank that will perform the actual I/O on this
 *              rank's behalf: [path][type_enc][space_enc][payload], each
 *              length-prefixed with a uint64_t, one MPI_Send per entry.
 *              type_enc/space_enc reuse H5VL__stream_encode_type()/
 *              H5VL__stream_encode_space() -- the same H5Tencode()/
 *              H5Sencode2() idiom this file already uses to move a
 *              type/dataspace's id across a process boundary (see the
 *              manifest replay code above), so an arbitrary selection
 *              survives the trip, not just a 1-D contiguous slice.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_send_write_entry_to_concentrator(const H5VL_stream_pending_entry_t *pe, int concentrator,
                                               MPI_Comm comm)
{
    uint8_t *type_enc = NULL, *space_enc = NULL;
    size_t   type_len = 0, space_len = 0;
    size_t   path_len = strlen(pe->path);
    uint8_t *msg      = NULL;
    size_t   msg_len, off = 0;
    herr_t   ret_value = 0;

    if (H5VL__stream_encode_type(pe->type_id, &type_enc, &type_len) < 0 ||
        H5VL__stream_encode_space(pe->space_id, &space_enc, &space_len) < 0) {
        ret_value = -1;
        goto done;
    }

    msg_len = 4 * sizeof(uint64_t) + path_len + type_len + space_len + pe->payload_len;
    if (NULL == (msg = (uint8_t *)malloc(msg_len))) {
        ret_value = -1;
        goto done;
    }

    {
        uint64_t v;

        v = (uint64_t)path_len;
        memcpy(msg + off, &v, sizeof(v));
        off += sizeof(v);
        memcpy(msg + off, pe->path, path_len);
        off += path_len;

        v = (uint64_t)type_len;
        memcpy(msg + off, &v, sizeof(v));
        off += sizeof(v);
        memcpy(msg + off, type_enc, type_len);
        off += type_len;

        v = (uint64_t)space_len;
        memcpy(msg + off, &v, sizeof(v));
        off += sizeof(v);
        memcpy(msg + off, space_enc, space_len);
        off += space_len;

        v = (uint64_t)pe->payload_len;
        memcpy(msg + off, &v, sizeof(v));
        off += sizeof(v);
        if (pe->payload_len > 0)
            memcpy(msg + off, pe->payload, pe->payload_len);
    }

    if (MPI_SUCCESS != MPI_Send(msg, (int)msg_len, MPI_BYTE, concentrator, 9102, comm))
        ret_value = -1;

done:
    free(type_enc);
    free(space_enc);
    free(msg);
    return ret_value;
} /* end H5VL__stream_send_write_entry_to_concentrator() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_recv_and_write_entry
 *
 * Purpose:     M6.5 concentrator topology. The receive side of
 *              H5VL__stream_send_write_entry_to_concentrator(): MPI_Probe
 *              to learn the incoming message's exact size (entries are
 *              variable-length -- payload size differs per write), decode
 *              the type/space back into real ids via H5Tdecode2()/
 *              H5Sdecode() (mirrors how H5VL__stream_replay_manifest()
 *              already decodes a manifest entry's type_enc/space_enc), and
 *              perform the write via H5VL__stream_write_replica() exactly
 *              as if this rank had originated the entry itself.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_recv_and_write_entry(H5VL_stream_t *file_obj, uint64_t physical_step, int source, MPI_Comm comm)
{
    MPI_Status status;
    int        count = 0;
    uint8_t   *msg  = NULL;
    size_t     off  = 0;
    uint64_t   v;
    char      *path = NULL;
    uint8_t   *type_enc, *space_enc, *payload;
    hid_t      type_id = -1, space_id = -1;
    herr_t     ret_value = 0;

    if (MPI_SUCCESS != MPI_Probe(source, 9102, comm, &status)) {
        ret_value = -1;
        goto done;
    }
    MPI_Get_count(&status, MPI_BYTE, &count);
    if (NULL == (msg = (uint8_t *)malloc((size_t)count))) {
        ret_value = -1;
        goto done;
    }
    if (MPI_SUCCESS != MPI_Recv(msg, count, MPI_BYTE, source, 9102, comm, MPI_STATUS_IGNORE)) {
        ret_value = -1;
        goto done;
    }

    memcpy(&v, msg + off, sizeof(v));
    off += sizeof(v);
    if (NULL == (path = (char *)malloc((size_t)v + 1))) {
        ret_value = -1;
        goto done;
    }
    memcpy(path, msg + off, (size_t)v);
    path[v] = '\0';
    off += (size_t)v;

    memcpy(&v, msg + off, sizeof(v));
    off += sizeof(v);
    type_enc = msg + off;
    off += (size_t)v;
    if ((type_id = H5Tdecode2(type_enc, (size_t)v)) < 0) {
        ret_value = -1;
        goto done;
    }

    memcpy(&v, msg + off, sizeof(v));
    off += sizeof(v);
    space_enc = msg + off;
    off += (size_t)v;
    if ((space_id = H5Sdecode(space_enc)) < 0) {
        ret_value = -1;
        goto done;
    }

    memcpy(&v, msg + off, sizeof(v));
    off += sizeof(v);
    payload = msg + off;

    if (H5VL__stream_write_replica(file_obj, physical_step, path, type_id, space_id, payload) < 0)
        ret_value = -1;

done:
    if (type_id >= 0)
        H5Tclose(type_id);
    if (space_id >= 0)
        H5Sclose(space_id);
    free(path);
    free(msg);
    return ret_value;
} /* end H5VL__stream_recv_and_write_entry() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_replay_concentrated_writes
 *
 * Purpose:     M6.5's remaining scope: route DsetWrite entries through a
 *              Subfiling-style I/O-concentrator topology instead of every
 *              rank touching the underlying file directly (see the
 *              dev-plan.md M6.5 exit gate's "...routed through at least
 *              one concentrator that aggregates more than one writer
 *              rank"). Ranks are partitioned into contiguous groups of
 *              group_size; the first rank in each group is that group's
 *              concentrator. A non-concentrator rank ships each of its own
 *              DsetWrite entries to its concentrator
 *              (H5VL__stream_send_write_entry_to_concentrator()) instead
 *              of writing it directly; the concentrator writes its own
 *              entries as usual (H5VL__stream_write_replica()) and then,
 *              for each other member of its group in ascending rank
 *              order, receives and writes that member's entries too
 *              (H5VL__stream_recv_and_write_entry()). The file ends up
 *              byte-identical to the non-concentrated path -- this changes
 *              *which rank* issues each H5VLdataset_write() call, not
 *              *what* gets written -- so it does not need
 *              H5Sselect_project_intersection() to combine contributors'
 *              selections into fewer, larger writes; that would be a
 *              further throughput optimization on top of this, not
 *              something correctness depends on, and is not attempted
 *              here.
 *
 *              Deadlock-safety: a non-concentrator rank's sends never wait
 *              on anything from its concentrator, and a concentrator
 *              receives from its members in a fixed order, one member's
 *              entire entry count at a time -- a bipartite, acyclic
 *              communication pattern (members only ever talk to their own
 *              concentrator), so ordinary blocking MPI_Send/MPI_Recv
 *              cannot deadlock regardless of message size or how far
 *              ahead/behind any one rank runs.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_replay_concentrated_writes(H5VL_stream_t *file_obj, uint64_t physical_step, int group_size)
{
    H5VL_stream_file_state_t *fs            = file_obj->file_state;
    int                        my_rank       = fs->mpi_rank;
    int                        nranks        = fs->mpi_size;
    int                        concentrator  = (my_rank / group_size) * group_size;
    int                        group_end     = concentrator + group_size;
    size_t                     i;
    int                        m;

    if (group_end > nranks)
        group_end = nranks;

    if (my_rank != concentrator) {
        int n_entries = 0;

        for (i = 0; i < fs->n_pending; i++)
            if (fs->pending[i].kind == vs_Kind_DsetWrite)
                n_entries++;

        if (MPI_SUCCESS != MPI_Send(&n_entries, 1, MPI_INT, concentrator, 9101, fs->comm))
            return -1;

        for (i = 0; i < fs->n_pending; i++) {
            if (fs->pending[i].kind != vs_Kind_DsetWrite)
                continue;
            if (H5VL__stream_send_write_entry_to_concentrator(&fs->pending[i], concentrator, fs->comm) < 0)
                return -1;
        }

        if (n_entries > 0)
            printf("  rank %d routed %d DsetWrite entries through concentrator %d\n", my_rank, n_entries,
                   concentrator);

        return 0;
    }

    /* This rank is a concentrator: its own entries first, exactly like the
     * non-concentrated path. */
    if (H5VL__stream_replay_local_writes(file_obj, physical_step) < 0)
        return -1;

    for (m = concentrator + 1; m < group_end; m++) {
        int n_entries = 0, k;

        if (MPI_SUCCESS != MPI_Recv(&n_entries, 1, MPI_INT, m, 9101, fs->comm, MPI_STATUS_IGNORE))
            return -1;

        for (k = 0; k < n_entries; k++)
            if (H5VL__stream_recv_and_write_entry(file_obj, physical_step, m, fs->comm) < 0)
                return -1;

        if (n_entries > 0)
            printf("  concentrator rank %d wrote %d DsetWrite entries on behalf of rank %d\n", my_rank,
                   n_entries, m);
    }

    return 0;
} /* end H5VL__stream_replay_concentrated_writes() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_replay_step_parallel
 *
 * Purpose:     M6.5's replacement for H5VL__stream_replay_step() when the
 *              writer is parallel (fs->has_comm): supports heterogeneous
 *              per-rank object sets -- rank 0 creating a dataset rank 1
 *              never touches -- which M6's first increment explicitly could
 *              not (every rank had to create the identical object set,
 *              relying on ordinary parallel HDF5 collective-create
 *              semantics with no connector-level help).
 *
 *              The mechanism: every rank encodes its own create-kind
 *              entries (DsetCreate, Attr -- metadata only for the former,
 *              full payload for the latter, see H5VL__stream_build_agg_
 *              manifest()'s comment) and exchanges them with every other
 *              rank via two MPI_Allgatherv calls (sizes gathered first via
 *              MPI_Allgather, then the variable-length payloads). Every
 *              rank then independently computes the identical deterministic
 *              merge (H5VL__stream_merge_agg_manifests()) and replays that
 *              SAME merged set collectively (H5VL__stream_replay_manifest()
 *              -- the ordinary H5Dcreate2()/H5Acreate2() collective-
 *              metadata calls this connector has always used, now just
 *              covering the union of what every rank asked for rather than
 *              only this rank's own subset). Finally, each rank replays
 *              its own DsetWrite entries -- directly
 *              (H5VL__stream_replay_local_writes()) by default, or routed
 *              through a Subfiling-style I/O-concentrator topology
 *              (H5VL__stream_replay_concentrated_writes()) when
 *              VOL_STREAM_CONCENTRATION opts in (see that function's
 *              comment) -- now that every target object exists.
 *
 *              M7's queue policy is not yet integrated with this parallel
 *              path; end_step()'s Block/Discard/Spill handling still only
 *              applies to the serial (!has_comm) path. See docs/dev-plan.md's
 *              M6.5 section.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_replay_step_parallel(H5VL_stream_t *file_obj)
{
    H5VL_stream_file_state_t    *fs = file_obj->file_state;
    uint8_t                      *my_buf = NULL, *my_payload = NULL;
    size_t                         my_len = 0, my_payload_len = 0;
    int                            my_rank = fs->mpi_rank, nranks = fs->mpi_size;
    int                           *lens = NULL, *payload_lens = NULL, *displs = NULL, *payload_displs = NULL;
    uint8_t                       *all_bufs_flat = NULL, *all_payloads_flat = NULL;
    uint8_t                      **bufs = NULL, **payload_bufs = NULL;
    uint8_t                       *merged_buf = NULL, *merged_payload = NULL;
    size_t                          merged_len = 0, merged_payload_len = 0;
    H5VL_stream_pending_entry_t *wiring = NULL;
    size_t                          n_wiring = 0;
    int                            my_len_i, my_payload_len_i;
    herr_t                         ret_value = 0;
    int                            r;

    if (H5VL__stream_build_agg_manifest(fs, &my_buf, &my_len, &my_payload, &my_payload_len) < 0)
        return -1;
    if (!my_payload) /* MPI_Allgatherv wants a valid pointer even for a 0-length send */
        my_payload = (uint8_t *)malloc(1);

    my_len_i         = (int)my_len;
    my_payload_len_i = (int)my_payload_len;

    if (NULL == (lens = (int *)malloc((size_t)nranks * sizeof(int))) ||
        NULL == (payload_lens = (int *)malloc((size_t)nranks * sizeof(int))) ||
        NULL == (displs = (int *)malloc((size_t)nranks * sizeof(int))) ||
        NULL == (payload_displs = (int *)malloc((size_t)nranks * sizeof(int)))) {
        ret_value = -1;
        goto done;
    }

    if (MPI_SUCCESS != MPI_Allgather(&my_len_i, 1, MPI_INT, lens, 1, MPI_INT, fs->comm) ||
        MPI_SUCCESS != MPI_Allgather(&my_payload_len_i, 1, MPI_INT, payload_lens, 1, MPI_INT, fs->comm)) {
        ret_value = -1;
        goto done;
    }

    {
        int total = 0, ptotal = 0;

        for (r = 0; r < nranks; r++) {
            displs[r] = total;
            total += lens[r];
            payload_displs[r] = ptotal;
            ptotal += payload_lens[r];
        }
        if (NULL == (all_bufs_flat = (uint8_t *)malloc(total > 0 ? (size_t)total : 1)) ||
            NULL == (all_payloads_flat = (uint8_t *)malloc(ptotal > 0 ? (size_t)ptotal : 1))) {
            ret_value = -1;
            goto done;
        }
    }

    if (MPI_SUCCESS !=
            MPI_Allgatherv(my_buf, my_len_i, MPI_BYTE, all_bufs_flat, lens, displs, MPI_BYTE, fs->comm) ||
        MPI_SUCCESS != MPI_Allgatherv(my_payload, my_payload_len_i, MPI_BYTE, all_payloads_flat, payload_lens,
                                        payload_displs, MPI_BYTE, fs->comm)) {
        ret_value = -1;
        goto done;
    }

    if (NULL == (bufs = (uint8_t **)malloc((size_t)nranks * sizeof(uint8_t *))) ||
        NULL == (payload_bufs = (uint8_t **)malloc((size_t)nranks * sizeof(uint8_t *)))) {
        ret_value = -1;
        goto done;
    }
    for (r = 0; r < nranks; r++) {
        bufs[r]         = all_bufs_flat + displs[r];
        payload_bufs[r] = all_payloads_flat + payload_displs[r];
    }

    if (H5VL__stream_merge_agg_manifests(fs, bufs, lens, payload_bufs, payload_lens, nranks, my_rank,
                                           &merged_buf, &merged_len, &merged_payload, &merged_payload_len,
                                           &wiring, &n_wiring) < 0) {
        ret_value = -1;
        goto done;
    }

    if (H5VL__stream_replay_manifest(file_obj, merged_buf, merged_len, merged_payload, wiring, n_wiring) < 0) {
        ret_value = -1;
        goto done;
    }

    {
        int group_size = H5VL__stream_concentration_factor();
        herr_t write_ret = (group_size > 1)
                                ? H5VL__stream_replay_concentrated_writes(file_obj, fs->physical_step, group_size)
                                : H5VL__stream_replay_local_writes(file_obj, fs->physical_step);

        if (write_ret < 0) {
            ret_value = -1;
            goto done;
        }
    }

done:
    flatcc_builder_free(my_buf);
    free(my_payload);
    free(lens);
    free(payload_lens);
    free(displs);
    free(payload_displs);
    free(all_bufs_flat);
    free(all_payloads_flat);
    free(bufs);
    free(payload_bufs);
    flatcc_builder_free(merged_buf);
    free(merged_payload);
    free(wiring);

    return ret_value;
} /* end H5VL__stream_replay_step_parallel() */
#endif /* H5_HAVE_PARALLEL */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_discard_step
 *
 * Purpose:     M7's Discard (and, as a building block, Spill) queue policy:
 *              materialize an empty /step/<physical_step>/ placeholder --
 *              satisfying the reader index's hard contiguity requirement
 *              (H5VL__stream_reader_build_index(): "physical steps are
 *              always 0..n-1, contiguous", a gap makes a reader refuse the
 *              whole file as foreign) -- without replaying any of this
 *              step's real entries into it. A reader landing on this step
 *              via sequential H5Fbegin_step() finds the group exists but
 *              nothing new: H5VL__stream_path_index_resolve()'s "largest
 *              physical step <= current with an entry" already falls back
 *              to the last real value with no code change needed here.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_discard_step(H5VL_stream_t *file_obj)
{
    H5VL_stream_file_state_t *fs = file_obj->file_state;
    char                       step_root[32];
    void                      *step_group;

    snprintf(step_root, sizeof(step_root), "/step/%llu", (unsigned long long)fs->physical_step);

    if (NULL ==
        (step_group = H5VL__stream_replay_ensure_group(file_obj->under_object, file_obj->under_vol_id, step_root)))
        return -1;
    H5VLgroup_close(step_group, file_obj->under_vol_id, H5P_DATASET_XFER_DEFAULT, NULL);

    return 0;
} /* end H5VL__stream_discard_step() */

#ifdef VOL_STREAM_HAVE_BAKE
/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_spill_step
 *
 * Purpose:     M7's Spill queue policy. Creates the same empty placeholder
 *              H5VL__stream_discard_step() does (contiguity), then builds
 *              this step's manifest+payload (H5VL__stream_build_manifest(),
 *              the cheap half of ordinary replay -- no H5Dcreate2()/
 *              H5Awrite() etc. against the shared file) and writes both,
 *              with a tiny length-prefixed header so they can be split
 *              apart again, as ONE region to node-local storage via
 *              tr_bake.c. One region because the BAKE file backend does not
 *              support writes to a non-zero region offset (confirmed
 *              against BAKE 0.6.4 directly): two separate regions would
 *              need two round trips for no benefit here anyway. The
 *              resulting descriptor is queued in fs->pending_spill for
 *              H5VL__stream_drain_spill() to complete later.
 *
 * Return:      Success:    0
 *              Failure:    -1 (fs->pending_spill unchanged; the caller,
 *                          H5VL__stream_apply_queue_policy(), falls back to
 *                          Block rather than silently lose the step)
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_spill_step(H5VL_stream_t *file_obj)
{
    H5VL_stream_file_state_t *fs           = file_obj->file_state;
    uint8_t                   *manifest_buf = NULL;
    uint8_t                   *payload_buf  = NULL;
    size_t                     manifest_len = 0;
    size_t                     payload_len  = 0;
    uint8_t                   *combined     = NULL;
    size_t                     combined_len;
    char                      *desc         = NULL;
    herr_t                     ret_value = 0;

    if (H5VL__stream_discard_step(file_obj) < 0)
        return -1;

    /* Lazily started on this file's first Spill eviction -- most steps of
     * most files never trigger Spill even with the policy set, so most
     * files never pay for a BAKE provider/target at all. */
    if (!fs->spill_bake && fs->transport)
        fs->spill_bake = vs_bake_start(vs_tr_get_mid(fs->transport), H5VL__stream_spill_dir());
    if (!fs->spill_bake)
        return -1;

    if (H5VL__stream_build_manifest(fs, &manifest_buf, &manifest_len, &payload_buf, &payload_len) < 0)
        return -1;

    /* [0..8): manifest_len, [8..16): payload_len, then manifest bytes, then
     * payload bytes -- an internal-only layout, never read back by anything
     * but H5VL__stream_drain_spill() on this same process. */
    combined_len = 16 + manifest_len + payload_len;
    if (NULL == (combined = (uint8_t *)malloc(combined_len))) {
        ret_value = -1;
        goto done;
    }
    {
        uint64_t mlen = (uint64_t)manifest_len, plen = (uint64_t)payload_len;

        memcpy(combined, &mlen, 8);
        memcpy(combined + 8, &plen, 8);
        memcpy(combined + 16, manifest_buf, manifest_len);
        memcpy(combined + 16 + manifest_len, payload_buf, payload_len);
    }

    if (!fs->spill_bake || vs_bake_spill_write(fs->spill_bake, combined, (uint64_t)combined_len, &desc) < 0) {
        ret_value = -1;
        goto done;
    }

    if (fs->n_pending_spill == fs->cap_pending_spill) {
        size_t                       new_cap = fs->cap_pending_spill ? fs->cap_pending_spill * 2 : 8;
        H5VL_stream_spill_entry_t *grown = (H5VL_stream_spill_entry_t *)realloc(
            fs->pending_spill, new_cap * sizeof(*fs->pending_spill));

        if (!grown) {
            ret_value = -1;
            goto done;
        }
        fs->pending_spill   = grown;
        fs->cap_pending_spill = new_cap;
    }
    fs->pending_spill[fs->n_pending_spill].physical_step = fs->physical_step;
    fs->pending_spill[fs->n_pending_spill].desc          = desc;
    fs->pending_spill[fs->n_pending_spill].size          = (uint64_t)combined_len;
    fs->n_pending_spill++;
    desc = NULL; /* ownership moved into pending_spill */

done:
    flatcc_builder_free(manifest_buf);
    free(payload_buf);
    free(combined);
    free(desc);

    return ret_value;
} /* end H5VL__stream_spill_step() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_drain_spill
 *
 * Purpose:     Complete the real replay -- H5Dcreate2()/H5Awrite() etc.,
 *              same as an un-spilled step would have gotten synchronously
 *              -- for every queued spill entry whose physical_step is now
 *              <= min_acked_step + reserve_slots, oldest first. Entries are
 *              always appended to fs->pending_spill in increasing
 *              physical_step order (each spill happens at the
 *              then-current, forward-only step counter), so the array is
 *              already sorted and the first entry still outside the window
 *              means every later one is too -- safe to stop there rather
 *              than scanning the rest.
 *
 *              pending_for_wiring is NULL for every drained entry (see
 *              H5VL__stream_replay_manifest()'s comment): safe because
 *              H5VL__stream_apply_queue_policy() never spills a step with
 *              an open placeholder in the first place.
 *
 * Return:      0 whether or not anything was drained; -1 only if a read or
 *              replay actually failed (the failing entry, and everything
 *              after it, stays queued for a later call to retry).
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_drain_spill(H5VL_stream_t *file_obj, uint64_t min_acked_step)
{
    H5VL_stream_file_state_t *fs = file_obj->file_state;
    size_t                     drained = 0;
    herr_t                     ret_value = 0;

    while (drained < fs->n_pending_spill) {
        H5VL_stream_spill_entry_t *e = &fs->pending_spill[drained];
        void                        *buf = NULL;

        if (e->physical_step > min_acked_step + fs->reserve_slots)
            break; /* still outside the window -- so is everything after it */

        if (vs_bake_spill_read(fs->spill_bake, e->desc, e->size, &buf) < 0) {
            ret_value = -1;
            break;
        }

        {
            uint64_t mlen, plen;

            if (e->size < 16) { /* corrupt/truncated -- cannot happen from our own writer, but do not trust blindly */
                free(buf);
                ret_value = -1;
                break;
            }
            memcpy(&mlen, buf, 8);
            memcpy(&plen, (const uint8_t *)buf + 8, 8);

            if (e->size != 16 + mlen + plen ||
                H5VL__stream_replay_manifest(file_obj, (const uint8_t *)buf + 16, (size_t)mlen,
                                               (const uint8_t *)buf + 16 + mlen, NULL, 0) < 0) {
                free(buf);
                ret_value = -1;
                break;
            }
        }
        free(buf);

        vs_bake_spill_remove(fs->spill_bake, e->desc); /* best-effort -- not fatal to leave it */
        free(e->desc);
        drained++;
    }

    if (drained > 0) {
        memmove(fs->pending_spill, fs->pending_spill + drained,
                (fs->n_pending_spill - drained) * sizeof(*fs->pending_spill));
        fs->n_pending_spill -= drained;
    }

    return ret_value;
} /* end H5VL__stream_drain_spill() */
#endif /* VOL_STREAM_HAVE_BAKE */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_apply_queue_policy
 *
 * Purpose:     M7. Called from end_step() instead of calling
 *              H5VL__stream_replay_step() directly. With no policy set
 *              (fs->queue_policy_set == 0, the default) or no transport to
 *              check reader progress against, this reduces to exactly
 *              H5VL__stream_replay_step() -- M0-M6 behavior, unchanged.
 *
 *              Otherwise: first, best-effort drain whatever earlier spilled
 *              steps the currently-tracked reader has now caught up to
 *              (independent of what this step itself needs, so a spilled
 *              backlog clears as soon as it can rather than only when the
 *              *next* step also happens to be under pressure). Then decide
 *              this step's own fate: if the furthest-behind tracked reader
 *              (vs_tr_writer_min_acked_step() -- readers that only ever
 *              jump via H5Fbegin_logical_step() are never tracked, so a
 *              monitoring/latest-only reader never counts here) is more
 *              than reserve_slots steps behind the one about to commit,
 *              apply the configured policy; otherwise, same as no pressure
 *              at all, do the ordinary full replay.
 *
 *              A step with any placeholder object still open (an
 *              application holding a handle from dataset_create()/
 *              attr_create() across end_step(), see
 *              H5VL__stream_replay_manifest()'s pending_for_wiring comment)
 *              is never spilled or discarded, regardless of pressure or
 *              policy -- deferring that resolution past end_step() is a
 *              real generalization this increment does not attempt, so
 *              such a step always gets the full immediate replay.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_apply_queue_policy(H5VL_stream_t *file_obj)
{
    H5VL_stream_file_state_t *fs = file_obj->file_state;

#ifdef VOL_STREAM_HAVE_MERCURY
    if (fs->queue_policy_set && fs->transport) {
        uint64_t min_acked = 0;
        int       have_lag;

#ifdef VOL_STREAM_HAVE_BAKE
        if (fs->n_pending_spill > 0) {
            uint64_t drain_to;

            if (!vs_tr_writer_min_acked_step(fs->transport, &drain_to))
                drain_to = fs->physical_step; /* nothing tracked -- no pressure, drain everything */
            H5VL__stream_drain_spill(file_obj, drain_to);
        }
#endif

        have_lag = vs_tr_writer_min_acked_step(fs->transport, &min_acked);

        if (have_lag && fs->physical_step > min_acked + fs->reserve_slots) {
            size_t i;
            int    has_open_placeholder = 0;

            for (i = 0; i < fs->n_pending; i++)
                if (fs->pending[i].owner_wrapper) {
                    has_open_placeholder = 1;
                    break;
                }

            if (!has_open_placeholder) {
                if (fs->queue_policy == H5VL_STREAM_QUEUE_DISCARD)
                    return H5VL__stream_discard_step(file_obj);

#ifdef VOL_STREAM_HAVE_BAKE
                if (fs->queue_policy == H5VL_STREAM_QUEUE_SPILL) {
                    /* H5VL__stream_spill_step() lazily starts fs->spill_bake
                     * on its own on the very first call -- do not pre-check
                     * it here, or that first start never happens at all. */
                    if (H5VL__stream_spill_step(file_obj) == 0)
                        return 0;
                    /* Either BAKE never came up or the spill write itself
                     * failed -- fall through to Block rather than silently
                     * lose the step. */
                }
#else
                if (fs->queue_policy == H5VL_STREAM_QUEUE_SPILL)
                    return H5VL__stream_discard_step(file_obj); /* connector built without BAKE */
#endif
                /* Block: wait for the tracked reader to catch up. Bounded
                 * (~60s) so a genuine failure, not just slowness, cannot
                 * hang the writer forever -- Margo's progress engine runs
                 * independently of this thread, so the ack table keeps
                 * updating while it sleeps. */
                {
                    int iter;

                    for (iter = 0; iter < 600; iter++) {
                        uint64_t cur_min;

                        if (!vs_tr_writer_min_acked_step(fs->transport, &cur_min))
                            break; /* reader departed -- nothing left to wait for */
                        if (fs->physical_step <= cur_min + fs->reserve_slots)
                            break;
                        usleep(100000); /* 100ms */
                    }
                }
            }
        }
    }
#endif /* VOL_STREAM_HAVE_MERCURY */

    return H5VL__stream_replay_step(file_obj);
} /* end H5VL__stream_apply_queue_policy() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_init
 *
 * Purpose:     Initialize this VOL connector, performing any necessary
 *              operations for the connector that will apply to all containers
 *              accessed with the connector.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_init(hid_t vipl_id)
{
#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM INIT\n");
#endif

    /* Shut compiler up about unused parameter */
    (void)vipl_id;

    /* Register the step operations.  H5VLregister_opt_operation() is public, so
     * a connector can define its own operations with no HDF5 library change.
     * The op values land in file-scope statics that the H5F* wrappers below and
     * the 'optional' callback both consult.
     */
    if (H5VLregister_opt_operation(H5VL_SUBCLS_FILE, H5VL_STREAM_OP_BEGIN_STEP, &H5VL_stream_op_begin_step) < 0)
        return -1;
    if (H5VLregister_opt_operation(H5VL_SUBCLS_FILE, H5VL_STREAM_OP_END_STEP, &H5VL_stream_op_end_step) < 0)
        return -1;
    if (H5VLregister_opt_operation(H5VL_SUBCLS_FILE, H5VL_STREAM_OP_STEP_STATUS, &H5VL_stream_op_step_status) < 0)
        return -1;
    if (H5VLregister_opt_operation(H5VL_SUBCLS_FILE, H5VL_STREAM_OP_SUBSCRIBE, &H5VL_stream_op_subscribe) < 0)
        return -1;
    if (H5VLregister_opt_operation(H5VL_SUBCLS_FILE, H5VL_STREAM_OP_BEGIN_LOGICAL_STEP,
                                    &H5VL_stream_op_begin_logical_step) < 0)
        return -1;
    if (H5VLregister_opt_operation(H5VL_SUBCLS_FILE, H5VL_STREAM_OP_GET_LOGICAL_STEPS,
                                    &H5VL_stream_op_get_logical_steps) < 0)
        return -1;
    if (H5VLregister_opt_operation(H5VL_SUBCLS_FILE, H5VL_STREAM_OP_WAIT_STEP_READY,
                                    &H5VL_stream_op_wait_step_ready) < 0)
        return -1;
    if (H5VLregister_opt_operation(H5VL_SUBCLS_FILE, H5VL_STREAM_OP_SET_QUEUE_POLICY,
                                    &H5VL_stream_op_set_queue_policy) < 0)
        return -1;
    if (H5VLregister_opt_operation(H5VL_SUBCLS_FILE, H5VL_STREAM_OP_GET_SUBSCRIBED_DATA,
                                    &H5VL_stream_op_get_subscribed_data) < 0)
        return -1;

    return 0;
} /* end H5VL_stream_init() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_term
 *
 * Purpose:     Terminate this VOL connector, performing any necessary
 *              operations for the connector that release connector-wide
 *              resources (usually created / initialized with the 'init'
 *              callback).
 *
 * Return:      Success:    0
 *              Failure:    (Can't fail)
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_term(void)
{
#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM TERM\n");
#endif

    return 0;
} /* end H5VL_stream_term() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_info_copy
 *
 * Purpose:     Duplicate the connector's info object.
 *
 * Returns:     Success:    New connector info object
 *              Failure:    NULL
 *
 *---------------------------------------------------------------------------
 */
static void *
H5VL_stream_info_copy(const void *_info)
{
    const H5VL_stream_info_t *info = (const H5VL_stream_info_t *)_info;
    H5VL_stream_info_t       *new_info;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM INFO Copy\n");
#endif

    /* Make sure the underneath VOL of this vol-stream is specified */
    if (!info) {
        printf("\nH5VLstream.c line %d in %s: info for vol-stream can't be null\n", __LINE__,
               __func__);
        return NULL;
    }

    if (H5Iis_valid(info->under_vol_id) <= 0) {
        printf("\nH5VLstream.c line %d in %s: not a valid underneath VOL ID for vol-stream\n",
               __LINE__, __func__);
        return NULL;
    }

    /* Allocate new VOL info struct for the pass through connector */
    new_info = (H5VL_stream_info_t *)calloc(1, sizeof(H5VL_stream_info_t));

    /* Increment reference count on underlying VOL ID, and copy the VOL info */
    new_info->under_vol_id = info->under_vol_id;

    H5Iinc_ref(new_info->under_vol_id);

    if (info->under_vol_info)
        H5VLcopy_connector_info(new_info->under_vol_id, &(new_info->under_vol_info), info->under_vol_info);

    return new_info;
} /* end H5VL_stream_info_copy() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_info_cmp
 *
 * Purpose:     Compare two of the connector's info objects, setting *cmp_value,
 *              following the same rules as strcmp().
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_info_cmp(int *cmp_value, const void *_info1, const void *_info2)
{
    const H5VL_stream_info_t *info1 = (const H5VL_stream_info_t *)_info1;
    const H5VL_stream_info_t *info2 = (const H5VL_stream_info_t *)_info2;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM INFO Compare\n");
#endif

    /* Sanity checks */
    assert(info1);
    assert(info2);

    /* Initialize comparison value */
    *cmp_value = 0;

    /* Compare under VOL connector classes */
    H5VLcmp_connector_cls(cmp_value, info1->under_vol_id, info2->under_vol_id);
    if (*cmp_value != 0)
        return 0;

    /* Compare under VOL connector info objects */
    H5VLcmp_connector_info(cmp_value, info1->under_vol_id, info1->under_vol_info, info2->under_vol_info);
    if (*cmp_value != 0)
        return 0;

    return 0;
} /* end H5VL_stream_info_cmp() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_info_free
 *
 * Purpose:     Release an info object for the connector.
 *
 * Note:	Take care to preserve the current HDF5 error stack
 *		when calling HDF5 API calls.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_info_free(void *_info)
{
    H5VL_stream_info_t *info = (H5VL_stream_info_t *)_info;
    hid_t                     err_id;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM INFO Free\n");
#endif

    err_id = H5Eget_current_stack();

    /* Release underlying VOL ID and info */
    if (info->under_vol_info)
        H5VLfree_connector_info(info->under_vol_id, info->under_vol_info);
    H5Idec_ref(info->under_vol_id);

    H5Eset_current_stack(err_id);

    /* Free pass through info object itself */
    free(info);

    return 0;
} /* end H5VL_stream_info_free() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_info_to_str
 *
 * Purpose:     Serialize an info object for this connector into a string
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_info_to_str(const void *_info, char **str)
{
    const H5VL_stream_info_t *info              = (const H5VL_stream_info_t *)_info;
    H5VL_class_value_t              under_value       = (H5VL_class_value_t)-1;
    char                           *under_vol_string  = NULL;
    size_t                          under_vol_str_len = 0;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM INFO To String\n");
#endif

    /* Get value and string for underlying VOL connector */
    H5VLget_value(info->under_vol_id, &under_value);
    H5VLconnector_info_to_str(info->under_vol_info, info->under_vol_id, &under_vol_string);

    /* Determine length of underlying VOL info string */
    if (under_vol_string)
        under_vol_str_len = strlen(under_vol_string);

    /* Allocate space for our info */
    size_t strSize = 32 + under_vol_str_len;
    *str           = (char *)H5allocate_memory(strSize, (bool)0);
    assert(*str);

    /* Encode our info */
    snprintf(*str, strSize, "under_vol=%u;under_info={%s}", (unsigned)under_value,
             (under_vol_string ? under_vol_string : ""));

    return 0;
} /* end H5VL_stream_info_to_str() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_str_to_info
 *
 * Purpose:     Deserialize a string into an info object for this connector.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_str_to_info(const char *str, void **_info)
{
    H5VL_stream_info_t *info;
    unsigned                  under_vol_value;
    const char               *under_vol_info_start, *under_vol_info_end;
    hid_t                     under_vol_id;
    void                     *under_vol_info = NULL;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM INFO String To Info\n");
#endif

    /* Retrieve the underlying VOL connector value and info */
    if (sscanf(str, "under_vol=%u;", &under_vol_value) != 1)
        return -1;
    under_vol_id         = H5VLregister_connector_by_value((H5VL_class_value_t)under_vol_value, H5P_DEFAULT);
    under_vol_info_start = strchr(str, '{');
    under_vol_info_end   = strrchr(str, '}');
    assert(under_vol_info_end > under_vol_info_start);
    if (under_vol_info_end != (under_vol_info_start + 1)) {
        char *under_vol_info_str;

        under_vol_info_str = (char *)malloc((size_t)(under_vol_info_end - under_vol_info_start));
        memcpy(under_vol_info_str, under_vol_info_start + 1,
               (size_t)((under_vol_info_end - under_vol_info_start) - 1));
        *(under_vol_info_str + (under_vol_info_end - under_vol_info_start)) = '\0';

        H5VLconnector_str_to_info(under_vol_info_str, under_vol_id, &under_vol_info);

        free(under_vol_info_str);
    } /* end else */

    /* Allocate new vol-stream connector info and set its fields */
    info                 = (H5VL_stream_info_t *)calloc(1, sizeof(H5VL_stream_info_t));
    info->under_vol_id   = under_vol_id;
    info->under_vol_info = under_vol_info;

    /* Set return value */
    *_info = info;

    return 0;
} /* end H5VL_stream_str_to_info() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_get_object
 *
 * Purpose:     Retrieve the 'data' for a VOL object.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static void *
H5VL_stream_get_object(const void *obj)
{
    const H5VL_stream_t *o = (const H5VL_stream_t *)obj;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM Get object\n");
#endif

    return H5VLget_object(o->under_object, o->under_vol_id);
} /* end H5VL_stream_get_object() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_get_wrap_ctx
 *
 * Purpose:     Retrieve a "wrapper context" for an object
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_get_wrap_ctx(const void *obj, void **wrap_ctx)
{
    const H5VL_stream_t    *o = (const H5VL_stream_t *)obj;
    H5VL_stream_wrap_ctx_t *new_wrap_ctx;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM WRAP CTX Get\n");
#endif

    /* Allocate new VOL object wrapping context for the pass through connector */
    new_wrap_ctx = (H5VL_stream_wrap_ctx_t *)calloc(1, sizeof(H5VL_stream_wrap_ctx_t));

    /* M4: a deferred write/attr-write request has no under_object/
     * under_vol_id (deliberately -- see H5VL__stream_make_deferred_request()'s
     * comment). H5VL_set_vol_wrapper() calls get_wrap_ctx() on every object
     * core VOL dispatch touches, including a request handed to H5ESwait(),
     * so this needs a defined answer rather than an invalid-ID error from
     * incref'ing under_vol_id == 0. Return an empty context instead (both
     * fields left at their calloc() default): nothing legal to do with a
     * deferred request ever calls wrap_object() on it, so it is never
     * actually used -- it only needs to exist and free cleanly, which
     * H5VL_stream_free_wrap_ctx()'s under_vol_id > 0 guard arranges. */
    if (o->obj_state == H5VL_STREAM_OBJ_DEFERRED_REQUEST) {
        *wrap_ctx = new_wrap_ctx;
        return 0;
    }

    /* Increment reference count on underlying VOL ID, and copy the VOL info */
    new_wrap_ctx->under_vol_id = o->under_vol_id;

    H5Iinc_ref(new_wrap_ctx->under_vol_id);

    H5VLget_wrap_ctx(o->under_object, o->under_vol_id, &new_wrap_ctx->under_wrap_ctx);

    /* Set wrap context to return */
    *wrap_ctx = new_wrap_ctx;

    return 0;
} /* end H5VL_stream_get_wrap_ctx() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_wrap_object
 *
 * Purpose:     Use a "wrapper context" to wrap a data object
 *
 * Return:      Success:    Pointer to wrapped object
 *              Failure:    NULL
 *
 *---------------------------------------------------------------------------
 */
static void *
H5VL_stream_wrap_object(void *obj, H5I_type_t obj_type, void *_wrap_ctx)
{
    H5VL_stream_wrap_ctx_t *wrap_ctx = (H5VL_stream_wrap_ctx_t *)_wrap_ctx;
    H5VL_stream_t          *new_obj;
    void                         *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM WRAP Object\n");
#endif

    /* Wrap the object with the underlying VOL */
    under = H5VLwrap_object(obj, obj_type, wrap_ctx->under_vol_id, wrap_ctx->under_wrap_ctx);
    if (under)
        new_obj = H5VL_stream_new_obj(under, wrap_ctx->under_vol_id);
    else
        new_obj = NULL;

    return new_obj;
} /* end H5VL_stream_wrap_object() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_unwrap_object
 *
 * Purpose:     Unwrap a wrapped object, discarding the wrapper, but returning
 *		underlying object.
 *
 * Return:      Success:    Pointer to unwrapped object
 *              Failure:    NULL
 *
 *---------------------------------------------------------------------------
 */
static void *
H5VL_stream_unwrap_object(void *obj)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    void                *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM UNWRAP Object\n");
#endif

    /* Unrap the object with the underlying VOL */
    under = H5VLunwrap_object(o->under_object, o->under_vol_id);

    if (under)
        H5VL_stream_free_obj(o);

    return under;
} /* end H5VL_stream_unwrap_object() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_free_wrap_ctx
 *
 * Purpose:     Release a "wrapper context" for an object
 *
 * Note:	Take care to preserve the current HDF5 error stack
 *		when calling HDF5 API calls.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_free_wrap_ctx(void *_wrap_ctx)
{
    H5VL_stream_wrap_ctx_t *wrap_ctx = (H5VL_stream_wrap_ctx_t *)_wrap_ctx;
    hid_t                         err_id;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM WRAP CTX Free\n");
#endif

    err_id = H5Eget_current_stack();

    /* Release underlying VOL ID and wrap context. under_vol_id stays 0 (its
     * calloc() default, never a real registered ID) only for the empty
     * context H5VL_stream_get_wrap_ctx() returns for a deferred write/
     * attr-write request (see that function's M4 comment) -- skip the
     * matching decref there, since no incref was ever taken for it. */
    if (wrap_ctx->under_wrap_ctx)
        H5VLfree_wrap_ctx(wrap_ctx->under_wrap_ctx, wrap_ctx->under_vol_id);
    if (wrap_ctx->under_vol_id > 0)
        H5Idec_ref(wrap_ctx->under_vol_id);

    H5Eset_current_stack(err_id);

    /* Free pass through wrap context object itself */
    free(wrap_ctx);

    return 0;
} /* end H5VL_stream_free_wrap_ctx() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_attr_create
 *
 * Purpose:     Creates an attribute on an object.
 *
 * Return:      Success:    Pointer to attribute object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *
H5VL_stream_attr_create(void *obj, const H5VL_loc_params_t *loc_params, const char *name, hid_t type_id,
                              hid_t space_id, hid_t acpl_id, hid_t aapl_id, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *attr;
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    void                *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM ATTRIBUTE Create\n");
#endif

    /* M2: while a step is open, defer onto the pending manifest instead of
     * touching the real file -- mirrors H5VL_stream_dataset_create(). The
     * manifest path uses '@' to separate an attribute from the object
     * namespace (e.g. "/data@units"); since H5Awrite always overwrites an
     * attribute atomically, there is exactly one Attr entry per attribute,
     * mutated in place by attr_write() rather than appended to.
     *
     * Capture is allowed whenever the parent's path is resolvable (BY_SELF),
     * whether that parent is itself a placeholder created this step, the
     * file root, or a plain live group. Attaching a *new* attribute to a
     * dataset that was replayed in an *earlier* step has no clean answer
     * without a versioning scheme this schema does not have yet, and is left
     * for M3.
     */
    if (o->file_state && o->file_state->step_state == H5F_STEP_IN_STEP && o->path &&
        loc_params->type == H5VL_OBJECT_BY_SELF) {
        H5VL_stream_pending_entry_t entry;
        char                             *path;
        size_t                             idx;

        if (NULL == (path = H5VL__stream_attr_path(o->path, name)))
            return NULL;

        memset(&entry, 0, sizeof(entry));
        entry.kind     = vs_Kind_Attr;
        entry.path     = path;
        entry.type_id  = H5Tcopy(type_id);
        entry.space_id = H5Scopy(space_id);
        entry.dcpl_id  = H5Pcopy(acpl_id);
        entry.dapl_id  = H5I_INVALID_HID;
        if (entry.type_id < 0 || entry.space_id < 0 || entry.dcpl_id < 0) {
            H5VL__stream_pending_entry_clear(&entry);
            return NULL;
        }

        if ((idx = H5VL__stream_pending_append(o->file_state, &entry)) == (size_t)-1) {
            H5VL__stream_pending_entry_clear(&entry);
            return NULL;
        }

        if (NULL == (attr = H5VL_stream_new_obj(NULL, o->under_vol_id)))
            return NULL;
        attr->file_state = o->file_state;
        H5VL__stream_file_state_incref(o->file_state);
        attr->path                                = strdup(entry.path);
        attr->obj_state                           = H5VL_STREAM_OBJ_PLACEHOLDER;
        attr->pending_index                       = idx;
        o->file_state->pending[idx].owner_wrapper = attr;

        return (void *)attr;
    }

    under = H5VLattr_create(o->under_object, loc_params, o->under_vol_id, name, type_id, space_id, acpl_id,
                            aapl_id, dxpl_id, req);
    if (under) {
        attr = H5VL__stream_new_child_obj(under, o->under_vol_id, o->file_state, NULL, NULL);

        /* Check for async request */
        if (req && *req)
            *req = H5VL_stream_new_obj(*req, o->under_vol_id);
    } /* end if */
    else
        attr = NULL;

    return (void *)attr;
} /* end H5VL_stream_attr_create() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_attr_open
 *
 * Purpose:     Opens an attribute on an object.
 *
 * Return:      Success:    Pointer to attribute object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *
H5VL_stream_attr_open(void *obj, const H5VL_loc_params_t *loc_params, const char *name, hid_t aapl_id,
                            hid_t dxpl_id, void **req)
{
    H5VL_stream_t *attr;
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    void                *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM ATTRIBUTE Open\n");
#endif

    /* M3: same resolve-to-the-right-step logic as dataset_open(), via the
     * "@"-joined attribute path convention -- see
     * H5VL__stream_reader_open_attr(). */
    if (o->file_state && o->file_state->step_state == H5F_STEP_READING && o->path &&
        loc_params->type == H5VL_OBJECT_BY_SELF)
        return H5VL__stream_reader_open_attr(o, name, aapl_id, dxpl_id, req);

    under = H5VLattr_open(o->under_object, loc_params, o->under_vol_id, name, aapl_id, dxpl_id, req);
    if (under) {
        attr = H5VL__stream_new_child_obj(under, o->under_vol_id, o->file_state, NULL, NULL);

        /* Check for async request */
        if (req && *req)
            *req = H5VL_stream_new_obj(*req, o->under_vol_id);
    } /* end if */
    else
        attr = NULL;

    return (void *)attr;
} /* end H5VL_stream_attr_open() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_attr_read
 *
 * Purpose:     Reads data from attribute.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_attr_read(void *attr, hid_t mem_type_id, void *buf, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)attr;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM ATTRIBUTE Read\n");
#endif

    /* M2: reads are not legal during IN_STEP for a writer -- see the same
     * note in H5VL_stream_dataset_read(). */
    if (o->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER)
        return -1;

    ret_value = H5VLattr_read(o->under_object, o->under_vol_id, mem_type_id, buf, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_attr_read() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_attr_write
 *
 * Purpose:     Writes data to attribute.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_attr_write(void *attr, hid_t mem_type_id, const void *buf, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)attr;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM ATTRIBUTE Write\n");
#endif

    /* M2: H5Awrite always overwrites an attribute atomically, so a
     * placeholder attribute has exactly one Attr entry -- fill in (or
     * replace) its payload in place rather than appending a new entry.
     *
     * The payload is sized and copied against the entry's already-captured
     * file type, not mem_type_id: an attribute write whose memory type needs
     * real conversion to the file type is not captured correctly in M2 (the
     * exit-gate matrix's attribute scenarios use matching native types, so
     * this does not affect them; byte-order conversion is exercised on
     * datasets instead, via DsetWrite's own mem-type capture).
     */
    if (o->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER) {
        H5VL_stream_pending_entry_t *e = &o->file_state->pending[o->pending_index];
        hssize_t                      n_elem;
        size_t                        nbytes;

        if ((n_elem = H5Sget_select_npoints(e->space_id)) < 0)
            return -1;

        /* See H5VL__stream_type_unsafe_to_capture()'s comment: reject a
         * VL/reference-typed attribute rather than memcpy-ing pointers that
         * are only valid in this process. */
        if (H5VL__stream_type_unsafe_to_capture(e->type_id) != 0)
            return -1;

        nbytes = (size_t)n_elem * H5Tget_size(e->type_id);

        free(e->payload);
        e->payload     = NULL;
        e->payload_len = 0;

        if (nbytes > 0) {
            if (NULL == (e->payload = (uint8_t *)malloc(nbytes)))
                return -1;
            memcpy(e->payload, buf, nbytes);
            e->payload_len = nbytes;
        }

        /* M4: same durability-tracking request as the dataset-write deferred
         * path above -- see H5VL__stream_make_deferred_request()'s comment. */
        if (H5VL__stream_make_deferred_request(o->file_state, req) < 0)
            return -1;

        return 0;
    }

    ret_value = H5VLattr_write(o->under_object, o->under_vol_id, mem_type_id, buf, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_attr_write() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_attr_get
 *
 * Purpose:     Gets information about an attribute
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_attr_get(void *obj, H5VL_attr_get_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM ATTRIBUTE Get\n");
#endif

    /* M2: service directly from the pending entry -- see the same note in
     * H5VL_stream_dataset_get(). */
    if (o->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER) {
        H5VL_stream_pending_entry_t *e = &o->file_state->pending[o->pending_index];

        switch (args->op_type) {
            case H5VL_ATTR_GET_SPACE:
                return (args->args.get_space.space_id = H5Scopy(e->space_id)) < 0 ? -1 : 0;
            case H5VL_ATTR_GET_TYPE:
                return (args->args.get_type.type_id = H5Tcopy(e->type_id)) < 0 ? -1 : 0;
            case H5VL_ATTR_GET_ACPL:
                return (args->args.get_acpl.acpl_id = H5Pcopy(e->dcpl_id)) < 0 ? -1 : 0;
            case H5VL_ATTR_GET_STORAGE_SIZE:
                *args->args.get_storage_size.data_size = e->payload_len;
                return 0;
            default:
                return -1;
        }
    }

    ret_value = H5VLattr_get(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_attr_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_attr_specific
 *
 * Purpose:     Specific operation on attribute
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_attr_specific(void *obj, const H5VL_loc_params_t *loc_params,
                                H5VL_attr_specific_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM ATTRIBUTE Specific\n");
#endif

    /* M2: not supported against a placeholder -- a documented gap, not part
     * of the M2 exit-gate matrix. */
    if (o->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER)
        return -1;

    ret_value = H5VLattr_specific(o->under_object, loc_params, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_attr_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_attr_optional
 *
 * Purpose:     Perform a connector-specific operation on an attribute
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_attr_optional(void *obj, H5VL_optional_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM ATTRIBUTE Optional\n");
#endif

    /* M2: not supported against a placeholder -- a documented gap, not part
     * of the M2 exit-gate matrix. */
    if (o->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER)
        return -1;

    ret_value = H5VLattr_optional(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_attr_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_attr_close
 *
 * Purpose:     Closes an attribute.
 *
 * Return:      Success:    0
 *              Failure:    -1, attr not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_attr_close(void *attr, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)attr;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM ATTRIBUTE Close\n");
#endif

    /* M2: nothing underlying exists yet for a placeholder -- see the same
     * note in H5VL_stream_dataset_close(). */
    if (o->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER) {
        o->file_state->pending[o->pending_index].owner_wrapper = NULL;
        H5VL_stream_free_obj(o);
        return 0;
    }

    ret_value = H5VLattr_close(o->under_object, o->under_vol_id, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    /* Release our wrapper, if underlying attribute was closed */
    if (ret_value >= 0)
        H5VL_stream_free_obj(o);

    return ret_value;
} /* end H5VL_stream_attr_close() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_dataset_create
 *
 * Purpose:     Creates a dataset in a container
 *
 * Return:      Success:    Pointer to a dataset object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *
H5VL_stream_dataset_create(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                                 hid_t lcpl_id, hid_t type_id, hid_t space_id, hid_t dcpl_id, hid_t dapl_id,
                                 hid_t dxpl_id, void **req)
{
    H5VL_stream_t *dset;
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    void                *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATASET Create\n");
#endif

    /* M2: while a step is open, defer the create into the pending manifest
     * instead of touching the real file at all -- see the architecture note
     * above H5VL_stream_file_optional(). Only the common BY_SELF entry point
     * (H5Dcreate2(fid_or_gid, name, ...)) is capturable; anything else falls
     * through to the live pass-through below, uncaptured but still correct.
     */
    if (o->file_state && o->file_state->step_state == H5F_STEP_IN_STEP &&
        loc_params->type == H5VL_OBJECT_BY_SELF) {
        H5VL_stream_pending_entry_t entry;
        char                             *path = H5VL__stream_child_path(o->path, name);
        size_t                             idx;

        if (!path)
            return NULL;

        memset(&entry, 0, sizeof(entry));
        entry.kind     = vs_Kind_DsetCreate;
        entry.path     = path;
        entry.type_id  = H5Tcopy(type_id);
        entry.space_id = H5Scopy(space_id);
        entry.dcpl_id  = H5Pcopy(dcpl_id);
        entry.dapl_id  = H5Pcopy(dapl_id);
        if (entry.type_id < 0 || entry.space_id < 0 || entry.dcpl_id < 0 || entry.dapl_id < 0) {
            H5VL__stream_pending_entry_clear(&entry);
            return NULL;
        }

        if ((idx = H5VL__stream_pending_append(o->file_state, &entry)) == (size_t)-1) {
            H5VL__stream_pending_entry_clear(&entry);
            return NULL;
        }

        if (NULL == (dset = H5VL_stream_new_obj(NULL, o->under_vol_id)))
            return NULL;
        dset->file_state = o->file_state;
        H5VL__stream_file_state_incref(o->file_state);
        dset->path                                = strdup(entry.path);
        dset->obj_state                           = H5VL_STREAM_OBJ_PLACEHOLDER;
        dset->pending_index                       = idx;
        o->file_state->pending[idx].owner_wrapper = dset;

        return (void *)dset;
    }

    under = H5VLdataset_create(o->under_object, loc_params, o->under_vol_id, name, lcpl_id, type_id, space_id,
                               dcpl_id, dapl_id, dxpl_id, req);
    if (under) {
        dset = H5VL__stream_new_child_obj(under, o->under_vol_id, o->file_state,
                                           loc_params->type == H5VL_OBJECT_BY_SELF ? o->path : NULL, name);

        /* Check for async request */
        if (req && *req)
            *req = H5VL_stream_new_obj(*req, o->under_vol_id);
    } /* end if */
    else
        dset = NULL;

    return (void *)dset;
} /* end H5VL_stream_dataset_create() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_dataset_open
 *
 * Purpose:     Opens a dataset in a container
 *
 * Return:      Success:    Pointer to a dataset object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *
H5VL_stream_dataset_open(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                               hid_t dapl_id, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *dset;
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    void                *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATASET Open\n");
#endif

    /* M3: a reader positioned at a step resolves a bare path to whichever
     * physical step actually has it, rather than opening name literally --
     * see H5VL__stream_reader_open_dataset() and the M3 plan's "Critical
     * finding #1"/"Critical finding #3". */
    if (o->file_state && o->file_state->step_state == H5F_STEP_READING && o->path &&
        loc_params->type == H5VL_OBJECT_BY_SELF)
        return H5VL__stream_reader_open_dataset(o, name, dapl_id, dxpl_id, req);

    under = H5VLdataset_open(o->under_object, loc_params, o->under_vol_id, name, dapl_id, dxpl_id, req);
    if (under) {
        dset = H5VL__stream_new_child_obj(under, o->under_vol_id, o->file_state,
                                           loc_params->type == H5VL_OBJECT_BY_SELF ? o->path : NULL, name);

        /* Check for async request */
        if (req && *req)
            *req = H5VL_stream_new_obj(*req, o->under_vol_id);
    } /* end if */
    else
        dset = NULL;

    return (void *)dset;
} /* end H5VL_stream_dataset_open() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_dataset_read
 *
 * Purpose:     Reads data elements from a dataset into a buffer.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_dataset_read(size_t count, void *dset[], hid_t mem_type_id[], hid_t mem_space_id[],
                               hid_t file_space_id[], hid_t plist_id, void *buf[], void **req)
{
    void  *obj_local;        /* Local buffer for obj */
    void **obj = &obj_local; /* Array of object pointers */
    size_t i;                /* Local index variable */
    herr_t ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATASET Read\n");
#endif

    /* M2: a dataset still IN_STEP is a bookkeeping placeholder with nothing
     * underlying to read yet -- the connector state machine never lists
     * reads as legal during IN_STEP for a writer, so reject rather than
     * dereference a NULL under_object. */
    for (i = 0; i < count; i++)
        if (((H5VL_stream_t *)dset[i])->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER)
            return -1;

    /* Allocate obj array if necessary */
    if (count > 1)
        if (NULL == (obj = (void **)malloc(count * sizeof(void *))))
            return -1;

    /* Build obj array */
    for (i = 0; i < count; i++) {
        /* Get the object */
        obj[i] = ((H5VL_stream_t *)dset[i])->under_object;

        /* Make sure the class matches */
        if (((H5VL_stream_t *)dset[i])->under_vol_id != ((H5VL_stream_t *)dset[0])->under_vol_id)
            return -1;
    }

    ret_value = H5VLdataset_read(count, obj, ((H5VL_stream_t *)dset[0])->under_vol_id, mem_type_id,
                                 mem_space_id, file_space_id, plist_id, buf, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, ((H5VL_stream_t *)dset[0])->under_vol_id);

    /* Free memory */
    if (obj != &obj_local)
        free(obj);

    return ret_value;
} /* end H5VL_stream_dataset_read() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_dataset_write
 *
 * Purpose:     Writes data elements from a buffer into a dataset.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_dataset_write(size_t count, void *dset[], hid_t mem_type_id[], hid_t mem_space_id[],
                                hid_t file_space_id[], hid_t plist_id, const void *buf[], void **req)
{
    void  *obj_local;        /* Local buffer for obj */
    void **obj = &obj_local; /* Array of object pointers */
    size_t i;                /* Local index variable */
    size_t n_placeholder = 0;
    herr_t ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATASET Write\n");
#endif

    for (i = 0; i < count; i++)
        if (((H5VL_stream_t *)dset[i])->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER)
            n_placeholder++;

    /* M2: an H5Dwrite_multi() batch mixing placeholder (deferred-into-step)
     * and live datasets is not supported -- a combination the exit-gate
     * matrix does not exercise. Either every dataset in the batch is
     * captured, or none are. */
    if (n_placeholder > 0 && n_placeholder < count)
        return -1;

    if (n_placeholder == count) {
        /* Every write in this call targets a dataset deferred into the
         * current step: capture each as its own DsetWrite entry (one entry
         * per H5Dwrite call, carrying its own selection -- what makes
         * multiple partial-hyperslab writes to the same dataset in one step
         * work independently) instead of touching the real file.
         *
         * The captured buffer is a flat copy of exactly npoints(file
         * selection) elements, read starting at buf[i] -- correct for a
         * *simple* mem_space_id sized to match the file selection's npoints
         * (the standard partial-write idiom, and what this connector's own
         * tests use), or for H5S_ALL/H5S_ALL on a whole-dataset write.
         *
         * It is NOT correct for mem_space_id == H5S_ALL paired with a
         * *partial* file_space_id: per H5VLnative_dataset.c, H5S_ALL for the
         * memory space reuses the file space's selection object outright,
         * so the effective memory selection is the same hyperslab applied
         * to buf[i] as if it pointed at a same-extent buffer -- not "the
         * first npoints elements". Capturing that correctly would need to
         * honor the selection's offset/stride when copying out of buf[i],
         * which M2 does not implement; it assumes the simple-mem-space
         * idiom above instead.
         */
        for (i = 0; i < count; i++) {
            H5VL_stream_t                *o             = (H5VL_stream_t *)dset[i];
            H5VL_stream_pending_entry_t  *create_entry  = &o->file_state->pending[o->pending_index];
            hid_t                         resolved_fspace = H5VL__stream_resolve_space(file_space_id[i], create_entry->space_id);
            hssize_t                      n_elem;
            H5VL_stream_pending_entry_t   write_entry;

            if ((n_elem = H5Sget_select_npoints(resolved_fspace)) < 0)
                return -1;

            memset(&write_entry, 0, sizeof(write_entry));
            write_entry.kind     = vs_Kind_DsetWrite;
            write_entry.path     = strdup(o->path);
            write_entry.type_id  = H5Tcopy(mem_type_id[i]);
            write_entry.space_id = H5Scopy(resolved_fspace);
            write_entry.dcpl_id  = H5I_INVALID_HID;
            write_entry.dapl_id  = H5I_INVALID_HID;
            if (!write_entry.path || write_entry.type_id < 0 || write_entry.space_id < 0) {
                H5VL__stream_pending_entry_clear(&write_entry);
                return -1;
            }

            /* See H5VL__stream_type_unsafe_to_capture()'s comment: a raw
             * memcpy of a VL/reference buffer captures pointers that are
             * only valid in this process, not portable bytes -- reject
             * rather than silently corrupt on replay. */
            if (H5VL__stream_type_unsafe_to_capture(write_entry.type_id) != 0) {
                H5VL__stream_pending_entry_clear(&write_entry);
                return -1;
            }

            if (n_elem > 0) {
                size_t elem_size = H5Tget_size(write_entry.type_id);
                size_t nbytes    = (size_t)n_elem * elem_size;

                if (NULL == (write_entry.payload = (uint8_t *)malloc(nbytes))) {
                    H5VL__stream_pending_entry_clear(&write_entry);
                    return -1;
                }
                memcpy(write_entry.payload, buf[i], nbytes);
                write_entry.payload_len = nbytes;
            }

            if (H5VL__stream_pending_append(o->file_state, &write_entry) == (size_t)-1) {
                H5VL__stream_pending_entry_clear(&write_entry);
                return -1;
            }
        }

        /* M4: the buffer is already safely copied above; what a caller that
         * passed a non-NULL req is waiting on is durability -- end_step()'s
         * replay landing this batch in the underlying file. One request
         * covers the whole batch, matching count == 1 request per H5VL
         * dataset-write call regardless of how many datasets it targets. */
        if (H5VL__stream_make_deferred_request(((H5VL_stream_t *)dset[0])->file_state, req) < 0)
            return -1;

        return 0;
    }

    /* Allocate obj array if necessary */
    if (count > 1)
        if (NULL == (obj = (void **)malloc(count * sizeof(void *))))
            return -1;

    /* Build obj array */
    for (i = 0; i < count; i++) {
        /* Get the object */
        obj[i] = ((H5VL_stream_t *)dset[i])->under_object;

        /* Make sure the class matches */
        if (((H5VL_stream_t *)dset[i])->under_vol_id != ((H5VL_stream_t *)dset[0])->under_vol_id)
            return -1;
    }

    ret_value = H5VLdataset_write(count, obj, ((H5VL_stream_t *)dset[0])->under_vol_id, mem_type_id,
                                  mem_space_id, file_space_id, plist_id, buf, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, ((H5VL_stream_t *)dset[0])->under_vol_id);

    /* Free memory */
    if (obj != &obj_local)
        free(obj);

    return ret_value;
} /* end H5VL_stream_dataset_write() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_dataset_get
 *
 * Purpose:     Gets information about a dataset
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_dataset_get(void *dset, H5VL_dataset_get_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)dset;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATASET Get\n");
#endif

    /* M2: a placeholder has no underlying object to ask -- service directly
     * from the pending entry's still-live type/space/dcpl/dapl copies
     * instead, since apps commonly call e.g. H5Dget_space() right after
     * H5Dcreate2() and before end_step(). */
    if (o->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER) {
        H5VL_stream_pending_entry_t *e = &o->file_state->pending[o->pending_index];

        switch (args->op_type) {
            case H5VL_DATASET_GET_SPACE:
                return (args->args.get_space.space_id = H5Scopy(e->space_id)) < 0 ? -1 : 0;
            case H5VL_DATASET_GET_TYPE:
                return (args->args.get_type.type_id = H5Tcopy(e->type_id)) < 0 ? -1 : 0;
            case H5VL_DATASET_GET_DCPL:
                return (args->args.get_dcpl.dcpl_id = H5Pcopy(e->dcpl_id)) < 0 ? -1 : 0;
            case H5VL_DATASET_GET_DAPL:
                return (args->args.get_dapl.dapl_id = H5Pcopy(e->dapl_id)) < 0 ? -1 : 0;
            case H5VL_DATASET_GET_STORAGE_SIZE:
                *args->args.get_storage_size.storage_size = 0;
                return 0;
            case H5VL_DATASET_GET_SPACE_STATUS:
                *args->args.get_space_status.status = H5D_SPACE_STATUS_NOT_ALLOCATED;
                return 0;
            default:
                return -1;
        }
    }

    ret_value = H5VLdataset_get(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_dataset_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_dataset_specific
 *
 * Purpose:     Specific operation on a dataset
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_dataset_specific(void *obj, H5VL_dataset_specific_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    hid_t                under_vol_id;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM H5Dspecific\n");
#endif

    /* M2: not supported against a placeholder (H5Dset_extent, H5Dflush,
     * H5Drefresh on a not-yet-real object) -- a documented gap, not part of
     * the M2 exit-gate matrix. */
    if (o->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER)
        return -1;

    /* Save copy of underlying VOL connector ID, in case of
     * 'refresh' operation destroying the current object
     */
    under_vol_id = o->under_vol_id;

    ret_value = H5VLdataset_specific(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, under_vol_id);

    return ret_value;
} /* end H5VL_stream_dataset_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_dataset_optional
 *
 * Purpose:     Perform a connector-specific operation on a dataset
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_dataset_optional(void *obj, H5VL_optional_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATASET Optional\n");
#endif

    /* M2: not supported against a placeholder -- a documented gap, not part
     * of the M2 exit-gate matrix (this is also where filtered-chunk
     * passthrough, H5Dread_chunk2/H5Dwrite_chunk, would eventually hook in
     * for a live object). */
    if (o->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER)
        return -1;

    ret_value = H5VLdataset_optional(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_dataset_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_dataset_close
 *
 * Purpose:     Closes a dataset.
 *
 * Return:      Success:    0
 *              Failure:    -1, dataset not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_dataset_close(void *dset, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)dset;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATASET Close\n");
#endif

    /* M2: nothing underlying exists yet for a placeholder -- just clear the
     * pending entry's back-pointer (so end_step()'s replay doesn't try to
     * patch a wrapper that's gone) and free the wrapper. This is the
     * t_step.c-style "H5Dcreate2, H5Dwrite, H5Dclose, all inside the step"
     * pattern; closing before end_step is perfectly normal. */
    if (o->obj_state == H5VL_STREAM_OBJ_PLACEHOLDER) {
        o->file_state->pending[o->pending_index].owner_wrapper = NULL;
        H5VL_stream_free_obj(o);
        return 0;
    }

    ret_value = H5VLdataset_close(o->under_object, o->under_vol_id, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    /* Release our wrapper, if underlying dataset was closed */
    if (ret_value >= 0)
        H5VL_stream_free_obj(o);

    return ret_value;
} /* end H5VL_stream_dataset_close() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_datatype_commit
 *
 * Purpose:     Commits a datatype inside a container.
 *
 * Return:      Success:    Pointer to datatype object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *
H5VL_stream_datatype_commit(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                                  hid_t type_id, hid_t lcpl_id, hid_t tcpl_id, hid_t tapl_id, hid_t dxpl_id,
                                  void **req)
{
    H5VL_stream_t *dt;
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    void                *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATATYPE Commit\n");
#endif

    /* Committed datatypes stay live/pass-through always, even inside a step:
     * the fixed manifest Kind enum has no value for a standalone commit, and
     * H5Tencode() serializes a type's structure regardless of committedness
     * -- a dataset's own type_enc blob already captures a committed type's
     * shape correctly with no separate entry needed for the named type. */
    under = H5VLdatatype_commit(o->under_object, loc_params, o->under_vol_id, name, type_id, lcpl_id, tcpl_id,
                                tapl_id, dxpl_id, req);
    if (under) {
        dt = H5VL__stream_new_child_obj(under, o->under_vol_id, o->file_state,
                                         loc_params->type == H5VL_OBJECT_BY_SELF ? o->path : NULL, name);

        /* Check for async request */
        if (req && *req)
            *req = H5VL_stream_new_obj(*req, o->under_vol_id);
    } /* end if */
    else
        dt = NULL;

    return (void *)dt;
} /* end H5VL_stream_datatype_commit() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_datatype_open
 *
 * Purpose:     Opens a named datatype inside a container.
 *
 * Return:      Success:    Pointer to datatype object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *
H5VL_stream_datatype_open(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                                hid_t tapl_id, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *dt;
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    void                *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATATYPE Open\n");
#endif

    under = H5VLdatatype_open(o->under_object, loc_params, o->under_vol_id, name, tapl_id, dxpl_id, req);
    if (under) {
        dt = H5VL__stream_new_child_obj(under, o->under_vol_id, o->file_state,
                                         loc_params->type == H5VL_OBJECT_BY_SELF ? o->path : NULL, name);

        /* Check for async request */
        if (req && *req)
            *req = H5VL_stream_new_obj(*req, o->under_vol_id);
    } /* end if */
    else
        dt = NULL;

    return (void *)dt;
} /* end H5VL_stream_datatype_open() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_datatype_get
 *
 * Purpose:     Get information about a datatype
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_datatype_get(void *dt, H5VL_datatype_get_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)dt;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATATYPE Get\n");
#endif

    ret_value = H5VLdatatype_get(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_datatype_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_datatype_specific
 *
 * Purpose:     Specific operations for datatypes
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_datatype_specific(void *obj, H5VL_datatype_specific_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    hid_t                under_vol_id;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATATYPE Specific\n");
#endif

    /* Save copy of underlying VOL connector ID, in case of
     * 'refresh' operation destroying the current object
     */
    under_vol_id = o->under_vol_id;

    ret_value = H5VLdatatype_specific(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, under_vol_id);

    return ret_value;
} /* end H5VL_stream_datatype_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_datatype_optional
 *
 * Purpose:     Perform a connector-specific operation on a datatype
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_datatype_optional(void *obj, H5VL_optional_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATATYPE Optional\n");
#endif

    ret_value = H5VLdatatype_optional(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_datatype_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_datatype_close
 *
 * Purpose:     Closes a datatype.
 *
 * Return:      Success:    0
 *              Failure:    -1, datatype not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_datatype_close(void *dt, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)dt;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM DATATYPE Close\n");
#endif

    assert(o->under_object);

    ret_value = H5VLdatatype_close(o->under_object, o->under_vol_id, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    /* Release our wrapper, if underlying datatype was closed */
    if (ret_value >= 0)
        H5VL_stream_free_obj(o);

    return ret_value;
} /* end H5VL_stream_datatype_close() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_file_create
 *
 * Purpose:     Creates a container using this connector
 *
 * Return:      Success:    Pointer to a file object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *
H5VL_stream_file_create(const char *name, unsigned flags, hid_t fcpl_id, hid_t fapl_id, hid_t dxpl_id,
                              void **req)
{
    H5VL_stream_info_t *info;
    H5VL_stream_t      *file;
    hid_t                     under_fapl_id;
    void                     *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM FILE Create\n");
#endif

    /* Get copy of our VOL info from FAPL */
    H5Pget_vol_info(fapl_id, (void **)&info);

    /* Default the under-VOL to native when the FAPL carries no info */
    if (!info && NULL == (info = H5VL__stream_default_info()))
        return NULL;

    /* Copy the FAPL */
    under_fapl_id = H5Pcopy(fapl_id);

    /* Set the VOL ID and info for the underlying FAPL */
    H5Pset_vol(under_fapl_id, info->under_vol_id, info->under_vol_info);

    /* Open the file with the underlying VOL connector */
    under = H5VLfile_create(name, flags, fcpl_id, under_fapl_id, dxpl_id, req);
    if (under) {
        file = H5VL_stream_new_obj(under, info->under_vol_id);

        /* M2: every file gets fresh step state (refcount 1, the file
         * wrapper's own reference) and the empty root path. M3: a created
         * file is always opened for writing, never a reader -- is_reader
         * stays 0 (calloc default). file_under_object/vol_id let a wrapper
         * several levels deep route an absolute path through the file root;
         * see the M3 plan's "Critical finding #1". */
        if (file) {
            file->file_state                      = H5VL__stream_file_state_new();
            file->path                             = strdup("");
            file->file_state->file_under_object   = under;
            file->file_state->file_under_vol_id   = info->under_vol_id;
#ifdef H5_HAVE_PARALLEL
            H5VL__stream_detect_mpi_comm(file->file_state, fapl_id);
#endif
#ifdef VOL_STREAM_HAVE_MERCURY
            H5VL__stream_transport_start_writer(file->file_state, name);
#endif
        }

        /* Check for async request */
        if (req && *req)
            *req = H5VL_stream_new_obj(*req, info->under_vol_id);
    } /* end if */
    else
        file = NULL;

    /* Close underlying FAPL */
    H5Pclose(under_fapl_id);

    /* Release copy of our VOL info */
    H5VL_stream_info_free(info);

    return (void *)file;
} /* end H5VL_stream_file_create() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_file_open
 *
 * Purpose:     Opens a container created with this connector
 *
 * Return:      Success:    Pointer to a file object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *
H5VL_stream_file_open(const char *name, unsigned flags, hid_t fapl_id, hid_t dxpl_id, void **req)
{
    H5VL_stream_info_t *info;
    H5VL_stream_t      *file;
    hid_t                     under_fapl_id;
    void                     *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM FILE Open\n");
#endif

    /* Get copy of our VOL info from FAPL */
    H5Pget_vol_info(fapl_id, (void **)&info);

    /* Default the under-VOL to native when the FAPL carries no info */
    if (!info && NULL == (info = H5VL__stream_default_info()))
        return NULL;

    /* Copy the FAPL */
    under_fapl_id = H5Pcopy(fapl_id);

    /* Set the VOL ID and info for the underlying FAPL */
    H5Pset_vol(under_fapl_id, info->under_vol_id, info->under_vol_info);

    /* Open the file with the underlying VOL connector */
    under = H5VLfile_open(name, flags, under_fapl_id, dxpl_id, req);
    if (under) {
        file = H5VL_stream_new_obj(under, info->under_vol_id);

        /* M2: every file gets fresh step state (refcount 1, the file
         * wrapper's own reference) and the empty root path. M3: a file
         * opened without write access is a reader -- H5Fbegin_step() on it
         * advances a read cursor instead of starting write capture. See the
         * M3 plan's "Reader vs. writer" note. */
        if (file) {
            file->file_state                    = H5VL__stream_file_state_new();
            file->path                           = strdup("");
            file->file_state->file_under_object = under;
            file->file_state->file_under_vol_id = info->under_vol_id;
            file->file_state->is_reader         = ((flags & H5F_ACC_RDWR) == 0) ? 1 : 0;
#ifdef H5_HAVE_PARALLEL
            H5VL__stream_detect_mpi_comm(file->file_state, fapl_id);
#endif
#ifdef VOL_STREAM_HAVE_MERCURY
            if (file->file_state->is_reader)
                H5VL__stream_transport_start_reader(file->file_state, name);
            else
                H5VL__stream_transport_start_writer(file->file_state, name);
#endif
        }

        /* Check for async request */
        if (req && *req)
            *req = H5VL_stream_new_obj(*req, info->under_vol_id);
    } /* end if */
    else
        file = NULL;

    /* Close underlying FAPL */
    H5Pclose(under_fapl_id);

    /* Release copy of our VOL info */
    H5VL_stream_info_free(info);

    return (void *)file;
} /* end H5VL_stream_file_open() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_file_get
 *
 * Purpose:     Get info about a file
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_file_get(void *file, H5VL_file_get_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)file;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM FILE Get\n");
#endif

    ret_value = H5VLfile_get(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_file_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_file_specific
 *
 * Purpose:     Specific operation on file
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_file_specific(void *file, H5VL_file_specific_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t       *o = (H5VL_stream_t *)file;
    H5VL_stream_t       *new_o;
    H5VL_file_specific_args_t  my_args;
    H5VL_file_specific_args_t *new_args;
    H5VL_stream_info_t  *info         = NULL;
    hid_t                      under_vol_id = -1;
    herr_t                     ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM FILE Specific\n");
#endif

    if (args->op_type == H5VL_FILE_IS_ACCESSIBLE) {
        /* Shallow copy the args */
        memcpy(&my_args, args, sizeof(my_args));

        /* Get copy of our VOL info from FAPL */
        H5Pget_vol_info(args->args.is_accessible.fapl_id, (void **)&info);

        /* Default the under-VOL to native when the FAPL carries no info */
        if (!info && NULL == (info = H5VL__stream_default_info()))
            return (-1);

        /* Keep the correct underlying VOL ID for later */
        under_vol_id = info->under_vol_id;

        /* Copy the FAPL */
        my_args.args.is_accessible.fapl_id = H5Pcopy(args->args.is_accessible.fapl_id);

        /* Set the VOL ID and info for the underlying FAPL */
        H5Pset_vol(my_args.args.is_accessible.fapl_id, info->under_vol_id, info->under_vol_info);

        /* Set argument pointer to new arguments */
        new_args = &my_args;

        /* Set object pointer for operation */
        new_o = NULL;
    } /* end else-if */
    else if (args->op_type == H5VL_FILE_DELETE) {
        /* Shallow copy the args */
        memcpy(&my_args, args, sizeof(my_args));

        /* Get copy of our VOL info from FAPL */
        H5Pget_vol_info(args->args.del.fapl_id, (void **)&info);

        /* Default the under-VOL to native when the FAPL carries no info */
        if (!info && NULL == (info = H5VL__stream_default_info()))
            return (-1);

        /* Keep the correct underlying VOL ID for later */
        under_vol_id = info->under_vol_id;

        /* Copy the FAPL */
        my_args.args.del.fapl_id = H5Pcopy(args->args.del.fapl_id);

        /* Set the VOL ID and info for the underlying FAPL */
        H5Pset_vol(my_args.args.del.fapl_id, info->under_vol_id, info->under_vol_info);

        /* Set argument pointer to new arguments */
        new_args = &my_args;

        /* Set object pointer for operation */
        new_o = NULL;
    } /* end else-if */
    else {
        /* Keep the correct underlying VOL ID for later */
        under_vol_id = o->under_vol_id;

        /* Set argument pointer to current arguments */
        new_args = args;

        /* Set object pointer for operation */
        new_o = o->under_object;
    } /* end else */

    ret_value = H5VLfile_specific(new_o, under_vol_id, new_args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, under_vol_id);

    if (args->op_type == H5VL_FILE_IS_ACCESSIBLE) {
        /* Close underlying FAPL */
        H5Pclose(my_args.args.is_accessible.fapl_id);

        /* Release copy of our VOL info */
        H5VL_stream_info_free(info);
    } /* end else-if */
    else if (args->op_type == H5VL_FILE_DELETE) {
        /* Close underlying FAPL */
        H5Pclose(my_args.args.del.fapl_id);

        /* Release copy of our VOL info */
        H5VL_stream_info_free(info);
    } /* end else-if */
    else if (args->op_type == H5VL_FILE_REOPEN) {
        /* Wrap file struct pointer for 'reopen' operation, if we reopened one */
        if (ret_value >= 0 && *args->args.reopen.file)
            *args->args.reopen.file = H5VL_stream_new_obj(*args->args.reopen.file, under_vol_id);
    } /* end else */

    return ret_value;
} /* end H5VL_stream_file_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_file_optional
 *
 * Purpose:     Perform a connector-specific operation on a file
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_file_optional(void *file, H5VL_optional_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)file;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM File Optional\n");
#endif

    /* Intercept the step operations; everything else forwards untouched.
     *
     * M2: begin_step defers subsequent dataset/attribute creates and writes
     * into file_state's pending-entry buffer (see H5VL_stream_dataset_create()
     * and friends); end_step encodes that buffer into a flatcc manifest and
     * replays it, decoded, group-based under /step/<n>/ -- see
     * H5VL__stream_replay_step() below.
     */
    if (args->op_type == H5VL_stream_op_begin_step) {
        H5VL_stream_args_begin_step_t *sargs = (H5VL_stream_args_begin_step_t *)args->args;

        if (!sargs || !o->file_state)
            return -1;

        /* M3: a reader-mode file takes an entirely different path -- advance
         * the read cursor instead of starting write capture.
         * n_logical/logical_ids/wall_time_ns are write-only and ignored here;
         * the call is reused unmodified for both roles, per dev-plan.md's
         * "borrow by default" rule -- see H5Fbegin_step()'s doc comment. */
        if (o->file_state->is_reader) {
            herr_t adv_ret = H5VL__stream_reader_advance(o->file_state, 0, 0);

#ifdef VOL_STREAM_HAVE_MERCURY
            /* M7: report progress to the writer -- only for this sequential
             * advance, never for H5Fbegin_logical_step()'s jump (see that
             * handler below), which is exactly what keeps a reader using
             * only the jump path exempt from the writer's queue-policy
             * pressure (vs_tr_writer_min_acked_step()'s comment). Best-
             * effort: a failed ack just means the writer's view of this
             * reader goes stale until the next one, not a read failure. */
            if (adv_ret == 0 && o->file_state->transport)
                vs_tr_reader_ack_step(o->file_state->transport, o->file_state->current_step);
#endif
            return adv_ret;
        }

#ifdef H5_HAVE_PARALLEL
        /* M6: a writer's step boundaries are collective (opt_query() already
         * declares this via H5VL_OPT_QUERY_COLLECTIVE) -- synchronize here so
         * no rank starts capturing entries for this step before every rank
         * has finished whatever it was doing after the previous one (a
         * matching barrier sits at the end of end_step() too). */
        if (o->file_state->has_comm)
            MPI_Barrier(o->file_state->comm);
#endif

        /* Nested steps are not a thing: a step is the unit of atomicity, so an
         * unclosed one is a bug in the caller rather than something to
         * silently absorb. */
        if (o->file_state->step_state != H5F_STEP_NOT_IN_STEP)
            return -1;

        /* Take our own copy of the logical ids -- the caller's array is only
         * required to live for the duration of the call. */
        free(o->file_state->logical_ids);
        o->file_state->logical_ids = NULL;
        o->file_state->n_logical   = 0;

        if (sargs->n_logical > 0) {
            if (NULL ==
                (o->file_state->logical_ids = (uint64_t *)malloc(sargs->n_logical * sizeof(uint64_t))))
                return -1;
            memcpy(o->file_state->logical_ids, sargs->logical_ids, sargs->n_logical * sizeof(uint64_t));
            o->file_state->n_logical = sargs->n_logical;
        }

        o->file_state->wall_time_ns = sargs->wall_time_ns;
        o->file_state->step_state   = H5F_STEP_IN_STEP;
        return 0;
    }
    else if (args->op_type == H5VL_stream_op_end_step) {
        herr_t   replay_ret;
        uint64_t committed_step, committed_wall_time_ns;

        if (!o->file_state || o->file_state->step_state != H5F_STEP_IN_STEP)
            return -1;

        o->file_state->step_state = H5F_STEP_COMMITTING;

#ifdef H5_HAVE_PARALLEL
        /* M6: every rank has finished capturing its own pending entries for
         * this step by the time it calls end_step() -- but replay below
         * makes real, collective HDF5 calls (H5Dcreate2() and friends;
         * see H5VL__stream_replay_step()'s M6 comment) that every rank in
         * the communicator must reach together, so synchronize first. */
        if (o->file_state->has_comm)
            MPI_Barrier(o->file_state->comm);
#endif

#ifdef H5_HAVE_PARALLEL
        /* M6.5: a parallel writer supporting heterogeneous per-rank object
         * sets needs cross-rank aggregation before it can replay
         * collectively -- see H5VL__stream_replay_step_parallel()'s
         * comment. M7's queue policy is not yet integrated with this path
         * (out of M6.5's own scope, a real gap, not an oversight): a
         * parallel writer with a policy set still gets this collective
         * replay unconditionally, the same as if no policy were set at
         * all. */
        if (o->file_state->has_comm)
            replay_ret = H5VL__stream_replay_step_parallel(o);
        else
            replay_ret = H5VL__stream_apply_queue_policy(o);
#else
        /* M7: a no-op wrapper around H5VL__stream_replay_step() unless a
         * queue policy was set (H5Fset_stream_queue_policy()) -- see its
         * comment. */
        replay_ret = H5VL__stream_apply_queue_policy(o);
#endif

        /* Discard the pending buffer either way: a failed replay may have
         * landed some entries and not others, and there is no partial-step
         * state worth preserving -- see the same reasoning for an unclosed
         * step at file_close(). */
        H5VL__stream_pending_discard_all(o->file_state);
        free(o->file_state->logical_ids);
        o->file_state->logical_ids = NULL;
        o->file_state->n_logical   = 0;

        /* M4: resolve this step's deferred-request completion cell (see
         * H5VL_stream_step_completion_t's comment), invoke and drain any
         * notify() callbacks queued on it, and drop file_state's own
         * reference -- any request object the application is still holding
         * onto keeps the cell alive until it waits/frees it. A fresh cell is
         * created lazily by the next step's first deferred write. */
        if (o->file_state->current_completion) {
            H5VL_stream_step_completion_t *c = o->file_state->current_completion;
            H5VL_stream_step_notify_t     *n;

            c->status = (replay_ret < 0) ? -1 : 1;

            n              = c->notify_list;
            c->notify_list = NULL;
            while (n) {
                H5VL_stream_step_notify_t *next = n->next;

                n->cb(n->ctx, (c->status > 0) ? H5VL_REQUEST_STATUS_SUCCEED : H5VL_REQUEST_STATUS_FAIL);
                free(n);
                n = next;
            }

            H5VL__stream_step_completion_decref(c);
            o->file_state->current_completion = NULL;
        }

        if (replay_ret < 0) {
            o->file_state->step_state = H5F_STEP_NOT_IN_STEP;
            return -1;
        }

        committed_step          = o->file_state->physical_step;
        committed_wall_time_ns  = o->file_state->wall_time_ns;

        o->file_state->physical_step++;
        o->file_state->step_state = H5F_STEP_NOT_IN_STEP;

#ifdef VOL_STREAM_HAVE_MERCURY
        /* Best-effort: a stalled or absent reader must not fail (or stall)
         * the writer's end_step(). Data is already durable in the file at
         * this point regardless of whether any reader is listening. */
        if (o->file_state->transport)
            vs_tr_writer_broadcast_step_ready(o->file_state->transport, committed_step,
                                               committed_wall_time_ns);
#else
        (void)committed_step;
        (void)committed_wall_time_ns;
#endif

#ifdef H5_HAVE_PARALLEL
        /* Matches the barrier at the top of begin_step(): keeps a rank that
         * finishes end_step() from racing ahead into a collective call
         * (another begin_step(), or H5Fclose()) while a slower rank is
         * still inside replay above. */
        if (o->file_state->has_comm)
            MPI_Barrier(o->file_state->comm);
#endif

        return 0;
    }
    else if (args->op_type == H5VL_stream_op_step_status) {
        H5VL_stream_args_step_status_t *sargs = (H5VL_stream_args_step_status_t *)args->args;

        if (!sargs || !sargs->status || !o->file_state)
            return -1;

        *sargs->status = o->file_state->step_state;
        return 0;
    }
    else if (args->op_type == H5VL_stream_op_subscribe) {
        H5VL_stream_args_subscribe_t *sargs = (H5VL_stream_args_subscribe_t *)args->args;

        if (!sargs)
            return -1;

        /* Validated regardless of transport: a subscription naming a path
         * that cannot be expressed should fail where the caller made the
         * mistake. spaces[i]'s selection bounds ARE acted on (M8.5):
         * H5Sget_select_bounds()'s dimension-0 low/high become the 1-D
         * element range this subscription routes on, the general-N-D-
         * selection/H5Sselect_intersect_block case dev-plan.md's M8.5 text
         * describes remains out of scope. plists[i] (M8.5 precision), when
         * given, must be a real DCPL -- a subscription cannot request a
         * filter pipeline through anything else. */
        for (size_t i = 0; i < sargs->count; i++) {
            hid_t plist_id = sargs->plists ? sargs->plists[i] : H5P_DEFAULT;

            if (!sargs->paths[i] || sargs->paths[i][0] == '\0')
                return -1;
            if (H5Sget_simple_extent_ndims(sargs->spaces[i]) < 0)
                return -1;
            if (plist_id != H5P_DEFAULT) {
                hid_t cls = H5Pget_class(plist_id);

                if (cls < 0 || H5Pequal(cls, H5P_DATASET_CREATE) <= 0)
                    return -1;
            }
        }

#ifdef VOL_STREAM_HAVE_MERCURY
        if (o->file_state && o->file_state->transport) {
            for (size_t i = 0; i < sargs->count; i++) {
                uint64_t sel_start = 0, sel_count = UINT64_MAX;
                hid_t    plist_id = sargs->plists ? sargs->plists[i] : H5P_DEFAULT;
                uint8_t *dcpl_enc = NULL;
                size_t   dcpl_enc_len = 0;

                H5VL__stream_space_1d_bounds(sargs->spaces[i], &sel_start, &sel_count);
                if (plist_id != H5P_DEFAULT)
                    H5VL__stream_encode_dcpl(plist_id, &dcpl_enc, &dcpl_enc_len);
                vs_tr_reader_subscribe(o->file_state->transport, sargs->paths[i], sel_start, sel_count,
                                         dcpl_enc, (uint64_t)dcpl_enc_len);
                free(dcpl_enc);
            }
        }
#endif
        return 0;
    }
    else if (args->op_type == H5VL_stream_op_begin_logical_step) {
        H5VL_stream_args_begin_logical_step_t *sargs = (H5VL_stream_args_begin_logical_step_t *)args->args;
        uint64_t                                phys;
        size_t                                  i;
        int                                      found = -1;

        if (!sargs || !o->file_state || !o->file_state->is_reader)
            return -1;
        if (!o->file_state->index_built && H5VL__stream_reader_build_index(o->file_state) < 0)
            return -1;

        for (i = 0; i < o->file_state->n_logical_map; i++)
            if (o->file_state->logical_map[i].logical_id == sargs->logical_id) {
                phys  = o->file_state->logical_map[i].physical_step;
                found = 0;
                break;
            }
        if (found < 0)
            return -1;

        return H5VL__stream_reader_advance(o->file_state, 1, phys);
    }
    else if (args->op_type == H5VL_stream_op_get_logical_steps) {
        H5VL_stream_args_get_logical_steps_t *sargs = (H5VL_stream_args_get_logical_steps_t *)args->args;

        if (!sargs || !sargs->n_logical || !o->file_state || !o->file_state->is_reader)
            return -1;
        if (!o->file_state->index_built && H5VL__stream_reader_build_index(o->file_state) < 0)
            return -1;

        if (!sargs->logical_ids) {
            *sargs->n_logical = o->file_state->n_logical_map;
            return 0;
        }
        {
            size_t n = *sargs->n_logical < o->file_state->n_logical_map ? *sargs->n_logical
                                                                          : o->file_state->n_logical_map;
            size_t i;

            for (i = 0; i < n; i++)
                sargs->logical_ids[i] = o->file_state->logical_map[i].logical_id;
            *sargs->n_logical = o->file_state->n_logical_map;
        }
        return 0;
    }
    else if (args->op_type == H5VL_stream_op_wait_step_ready) {
#ifdef VOL_STREAM_HAVE_MERCURY
        H5VL_stream_args_wait_step_ready_t *sargs = (H5VL_stream_args_wait_step_ready_t *)args->args;

        if (!sargs || !sargs->physical_step || !o->file_state || !o->file_state->is_reader ||
            !o->file_state->transport)
            return -1;

        return (herr_t)vs_tr_reader_wait_step_ready(o->file_state->transport, sargs->timeout_ms,
                                                     sargs->physical_step, sargs->wall_time_ns);
#else
        return -1;
#endif
    }
    else if (args->op_type == H5VL_stream_op_set_queue_policy) {
        H5VL_stream_args_set_queue_policy_t *sargs = (H5VL_stream_args_set_queue_policy_t *)args->args;

        if (!sargs || !o->file_state || o->file_state->is_reader)
            return -1;

        o->file_state->queue_policy_set = 1;
        o->file_state->queue_policy      = sargs->policy;
        o->file_state->reserve_slots     = sargs->reserve_slots;
        return 0;
    }
    else if (args->op_type == H5VL_stream_op_get_subscribed_data) {
#ifdef VOL_STREAM_HAVE_MERCURY
        H5VL_stream_args_get_subscribed_data_t *sargs =
            (H5VL_stream_args_get_subscribed_data_t *)args->args;

        if (!sargs || !sargs->physical_step || !sargs->path || !sargs->buf || !sargs->size ||
            !sargs->elem_start || !sargs->elem_count || !o->file_state || !o->file_state->is_reader ||
            !o->file_state->transport)
            return -1;

        {
            uint64_t size64 = 0;
            uint8_t *dcpl_enc = NULL, *type_enc = NULL;
            uint64_t dcpl_enc_len = 0, type_enc_len = 0;
            uint32_t filter_mask = 0;
            int      r = vs_tr_reader_wait_data(o->file_state->transport, sargs->timeout_ms,
                                                  sargs->physical_step, sargs->path, sargs->buf, &size64,
                                                  sargs->elem_start, sargs->elem_count, &dcpl_enc,
                                                  &dcpl_enc_len, &type_enc, &type_enc_len, &filter_mask);

            *sargs->size = (size_t)size64;

            /* M8.5 precision: this push was re-filtered (see
             * H5VL__stream_refilter_for_subscriber()'s comment) -- *buf is
             * the *filtered* bytes as received, not decoded values. Reverse
             * it before handing anything back, so a caller of
             * H5Fget_subscribed_data() always gets real values regardless
             * of whether this particular push happened to be re-filtered.
             * A failed reconstruction is not fatal to an otherwise-
             * successful wait_data() -- falls back to the raw filtered
             * bytes rather than failing the call. */
            if (r == 0 && dcpl_enc_len > 0 && type_enc_len > 0 && sargs->elem_count) {
                void  *decoded     = NULL;
                size_t decoded_len = 0;

                if (H5VL__stream_unfilter_pushed_data(*sargs->buf, size64, dcpl_enc, dcpl_enc_len, type_enc,
                                                         type_enc_len, *sargs->elem_count, filter_mask,
                                                         &decoded, &decoded_len) == 0) {
                    free(*sargs->buf);
                    *sargs->buf  = decoded;
                    *sargs->size = decoded_len;
                }
            }
            free(dcpl_enc);
            free(type_enc);

            return (herr_t)r;
        }
#else
        return -1;
#endif
    }

    ret_value = H5VLfile_optional(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_file_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_file_close
 *
 * Purpose:     Closes a file.
 *
 * Return:      Success:    0
 *              Failure:    -1, file not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_file_close(void *file, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)file;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM FILE Close\n");
#endif

    /* M2: a step left open across a file close was never durably committed
     * -- discard it rather than leak it or replay a step nobody ended. M3:
     * a reader's H5F_STEP_READING is not an open step -- there is nothing
     * pending to discard, so it must not hit this branch. */
    if (o->file_state &&
        (o->file_state->step_state == H5F_STEP_IN_STEP || o->file_state->step_state == H5F_STEP_COMMITTING)) {
        H5VL__stream_pending_discard_all(o->file_state);
        free(o->file_state->logical_ids);
        o->file_state->logical_ids = NULL;
        o->file_state->n_logical   = 0;
        o->file_state->step_state  = H5F_STEP_NOT_IN_STEP;
    }

    ret_value = H5VLfile_close(o->under_object, o->under_vol_id, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    /* Release our wrapper, if underlying file was closed */
    if (ret_value >= 0)
        H5VL_stream_free_obj(o);

    return ret_value;
} /* end H5VL_stream_file_close() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_group_create
 *
 * Purpose:     Creates a group inside a container
 *
 * Return:      Success:    Pointer to a group object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *
H5VL_stream_group_create(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                               hid_t lcpl_id, hid_t gcpl_id, hid_t gapl_id, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *group;
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    void                *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM GROUP Create\n");
#endif

    under = H5VLgroup_create(o->under_object, loc_params, o->under_vol_id, name, lcpl_id, gcpl_id, gapl_id,
                             dxpl_id, req);
    if (under) {
        /* Groups stay live/pass-through always, even inside a step: they
         * exist only to supply path bookkeeping for their children's
         * manifest entries (H5Pset_create_intermediate_group() rebuilds any
         * ancestor groups a replayed dataset needs). */
        group = H5VL__stream_new_child_obj(under, o->under_vol_id, o->file_state,
                                            loc_params->type == H5VL_OBJECT_BY_SELF ? o->path : NULL, name);

        /* Check for async request */
        if (req && *req)
            *req = H5VL_stream_new_obj(*req, o->under_vol_id);
    } /* end if */
    else
        group = NULL;

    return (void *)group;
} /* end H5VL_stream_group_create() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_group_open
 *
 * Purpose:     Opens a group inside a container
 *
 * Return:      Success:    Pointer to a group object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *
H5VL_stream_group_open(void *obj, const H5VL_loc_params_t *loc_params, const char *name, hid_t gapl_id,
                             hid_t dxpl_id, void **req)
{
    H5VL_stream_t *group;
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    void                *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM GROUP Open\n");
#endif

    /* M3: /step/5/grp and /step/12/grp are entirely unrelated objects that
     * happen to share a name -- H5VL__stream_replay_ensure_group() never
     * shares nodes across steps -- so there is no single underlying group a
     * "logical group history" can map to. A reader-mode group open is
     * therefore virtual: path/file_state bookkeeping only, no under_object.
     * Every child open recomputes its own absolute path from this path and
     * re-resolves independently through the file (H5VL__stream_reader_open_
     * dataset()/_attr()), never through this wrapper's (nonexistent)
     * under_object. See the M3 plan's "Critical finding #1". Never fails
     * here -- a path prefix with no entry of its own (e.g. "/mesh" when only
     * "/mesh/coords" is ever an entry) is normal; resolution is deferred to
     * the eventual child open. */
    if (o->file_state && o->file_state->step_state == H5F_STEP_READING && o->path &&
        loc_params->type == H5VL_OBJECT_BY_SELF) {
        char *abs_path;

        if (NULL == (abs_path = H5VL__stream_child_path(o->path, name)))
            return NULL;
        if (NULL == (group = H5VL_stream_new_obj(NULL, o->under_vol_id))) {
            free(abs_path);
            return NULL;
        }
        group->file_state = o->file_state;
        H5VL__stream_file_state_incref(o->file_state);
        group->path       = abs_path;
        group->obj_state  = H5VL_STREAM_OBJ_READER_VIRTUAL;

        return (void *)group;
    }

    under = H5VLgroup_open(o->under_object, loc_params, o->under_vol_id, name, gapl_id, dxpl_id, req);
    if (under) {
        group = H5VL__stream_new_child_obj(under, o->under_vol_id, o->file_state,
                                            loc_params->type == H5VL_OBJECT_BY_SELF ? o->path : NULL, name);

        /* Check for async request */
        if (req && *req)
            *req = H5VL_stream_new_obj(*req, o->under_vol_id);
    } /* end if */
    else
        group = NULL;

    return (void *)group;
} /* end H5VL_stream_group_open() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_group_get
 *
 * Purpose:     Get info about a group
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_group_get(void *obj, H5VL_group_get_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM GROUP Get\n");
#endif

    /* M3: a reader-mode virtual group has no single underlying object to
     * report on -- see the M3 plan's "Critical finding #1". A documented
     * scope boundary, not a crash. */
    if (o->obj_state == H5VL_STREAM_OBJ_READER_VIRTUAL)
        return -1;

    ret_value = H5VLgroup_get(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_group_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_group_specific
 *
 * Purpose:     Specific operation on a group
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_group_specific(void *obj, H5VL_group_specific_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    hid_t                under_vol_id;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM GROUP Specific\n");
#endif

    /* M3: see H5VL_stream_group_get() -- no single underlying object for a
     * reader-mode virtual group. */
    if (o->obj_state == H5VL_STREAM_OBJ_READER_VIRTUAL)
        return -1;

    /* Save copy of underlying VOL connector ID, in case of
     * 'refresh' operation destroying the current object
     */
    under_vol_id = o->under_vol_id;

    /* Unpack arguments to get at the child file pointer when mounting a file */
    if (args->op_type == H5VL_GROUP_MOUNT) {
        H5VL_group_specific_args_t vol_cb_args; /* New group specific arg struct */

        /* Set up new VOL callback arguments */
        vol_cb_args.op_type         = H5VL_GROUP_MOUNT;
        vol_cb_args.args.mount.name = args->args.mount.name;
        vol_cb_args.args.mount.child_file =
            ((H5VL_stream_t *)args->args.mount.child_file)->under_object;
        vol_cb_args.args.mount.fmpl_id = args->args.mount.fmpl_id;

        /* Re-issue 'group specific' call, using the unwrapped pieces */
        ret_value = H5VLgroup_specific(o->under_object, under_vol_id, &vol_cb_args, dxpl_id, req);
    } /* end if */
    else
        ret_value = H5VLgroup_specific(o->under_object, under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, under_vol_id);

    return ret_value;
} /* end H5VL_stream_group_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_group_optional
 *
 * Purpose:     Perform a connector-specific operation on a group
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_group_optional(void *obj, H5VL_optional_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM GROUP Optional\n");
#endif

    /* M3: see H5VL_stream_group_get() -- no single underlying object for a
     * reader-mode virtual group. */
    if (o->obj_state == H5VL_STREAM_OBJ_READER_VIRTUAL)
        return -1;

    ret_value = H5VLgroup_optional(o->under_object, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_group_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_group_close
 *
 * Purpose:     Closes a group.
 *
 * Return:      Success:    0
 *              Failure:    -1, group not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_group_close(void *grp, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)grp;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM H5Gclose\n");
#endif

    /* M3: a reader-mode virtual group has no underlying object to close --
     * just release the wrapper. */
    if (o->obj_state == H5VL_STREAM_OBJ_READER_VIRTUAL) {
        H5VL_stream_free_obj(o);
        return 0;
    }

    ret_value = H5VLgroup_close(o->under_object, o->under_vol_id, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    /* Release our wrapper, if underlying file was closed */
    if (ret_value >= 0)
        H5VL_stream_free_obj(o);

    return ret_value;
} /* end H5VL_stream_group_close() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_link_create
 *
 * Purpose:     Creates a hard / soft / UD / external link.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_link_create(H5VL_link_create_args_t *args, void *obj, const H5VL_loc_params_t *loc_params,
                              hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o            = (H5VL_stream_t *)obj;
    hid_t                under_vol_id = -1;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM LINK Create\n");
#endif

    /* Try to retrieve the "under" VOL id */
    if (o)
        under_vol_id = o->under_vol_id;

    /* Fix up the link target object for hard link creation */
    if (H5VL_LINK_CREATE_HARD == args->op_type) {
        void *cur_obj = args->args.hard.curr_obj;

        /* If cur_obj is a non-NULL pointer, find its 'under object' and update the pointer */
        if (cur_obj) {
            /* Check if we still haven't set the "under" VOL ID */
            if (under_vol_id < 0)
                under_vol_id = ((H5VL_stream_t *)cur_obj)->under_vol_id;

            /* Update the object for the link target */
            args->args.hard.curr_obj = ((H5VL_stream_t *)cur_obj)->under_object;
        } /* end if */
    }     /* end if */

    ret_value = H5VLlink_create(args, (o ? o->under_object : NULL), loc_params, under_vol_id, lcpl_id,
                                lapl_id, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, under_vol_id);

    return ret_value;
} /* end H5VL_stream_link_create() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_link_copy
 *
 * Purpose:     Renames an object within an HDF5 container and copies it to a new
 *              group.  The original name SRC is unlinked from the group graph
 *              and then inserted with the new name DST (which can specify a
 *              new path for the object) as an atomic operation. The names
 *              are interpreted relative to SRC_LOC_ID and
 *              DST_LOC_ID, which are either file IDs or group ID.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_link_copy(void *src_obj, const H5VL_loc_params_t *loc_params1, void *dst_obj,
                            const H5VL_loc_params_t *loc_params2, hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id,
                            void **req)
{
    H5VL_stream_t *o_src        = (H5VL_stream_t *)src_obj;
    H5VL_stream_t *o_dst        = (H5VL_stream_t *)dst_obj;
    hid_t                under_vol_id = -1;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM LINK Copy\n");
#endif

    /* Retrieve the "under" VOL id */
    if (o_src)
        under_vol_id = o_src->under_vol_id;
    else if (o_dst)
        under_vol_id = o_dst->under_vol_id;
    assert(under_vol_id > 0);

    ret_value =
        H5VLlink_copy((o_src ? o_src->under_object : NULL), loc_params1, (o_dst ? o_dst->under_object : NULL),
                      loc_params2, under_vol_id, lcpl_id, lapl_id, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, under_vol_id);

    return ret_value;
} /* end H5VL_stream_link_copy() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_link_move
 *
 * Purpose:     Moves a link within an HDF5 file to a new group.  The original
 *              name SRC is unlinked from the group graph
 *              and then inserted with the new name DST (which can specify a
 *              new path for the object) as an atomic operation. The names
 *              are interpreted relative to SRC_LOC_ID and
 *              DST_LOC_ID, which are either file IDs or group ID.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_link_move(void *src_obj, const H5VL_loc_params_t *loc_params1, void *dst_obj,
                            const H5VL_loc_params_t *loc_params2, hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id,
                            void **req)
{
    H5VL_stream_t *o_src        = (H5VL_stream_t *)src_obj;
    H5VL_stream_t *o_dst        = (H5VL_stream_t *)dst_obj;
    hid_t                under_vol_id = -1;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM LINK Move\n");
#endif

    /* Retrieve the "under" VOL id */
    if (o_src)
        under_vol_id = o_src->under_vol_id;
    else if (o_dst)
        under_vol_id = o_dst->under_vol_id;
    assert(under_vol_id > 0);

    ret_value =
        H5VLlink_move((o_src ? o_src->under_object : NULL), loc_params1, (o_dst ? o_dst->under_object : NULL),
                      loc_params2, under_vol_id, lcpl_id, lapl_id, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, under_vol_id);

    return ret_value;
} /* end H5VL_stream_link_move() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_link_get
 *
 * Purpose:     Get info about a link
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_link_get(void *obj, const H5VL_loc_params_t *loc_params, H5VL_link_get_args_t *args,
                           hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM LINK Get\n");
#endif

    ret_value = H5VLlink_get(o->under_object, loc_params, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_link_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_link_specific
 *
 * Purpose:     Specific operation on a link
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_link_specific(void *obj, const H5VL_loc_params_t *loc_params,
                                H5VL_link_specific_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM LINK Specific\n");
#endif

    ret_value = H5VLlink_specific(o->under_object, loc_params, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_link_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_link_optional
 *
 * Purpose:     Perform a connector-specific operation on a link
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_link_optional(void *obj, const H5VL_loc_params_t *loc_params, H5VL_optional_args_t *args,
                                hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM LINK Optional\n");
#endif

    ret_value = H5VLlink_optional(o->under_object, loc_params, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_link_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_object_open
 *
 * Purpose:     Opens an object inside a container.
 *
 * Return:      Success:    Pointer to object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *
H5VL_stream_object_open(void *obj, const H5VL_loc_params_t *loc_params, H5I_type_t *opened_type,
                              hid_t dxpl_id, void **req)
{
    H5VL_stream_t *new_obj;
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    void                *under;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM OBJECT Open\n");
#endif

    under = H5VLobject_open(o->under_object, loc_params, o->under_vol_id, opened_type, dxpl_id, req);
    if (under) {
        /* object_open() is inherently a BY_NAME/BY_IDX/BY_TOKEN entry point,
         * so the result's path is never resolvable here -- it still borrows
         * file_state (NULL parent_path yields path == NULL, per
         * H5VL__stream_new_child_obj()). */
        new_obj = H5VL__stream_new_child_obj(under, o->under_vol_id, o->file_state, NULL, NULL);

        /* Check for async request */
        if (req && *req)
            *req = H5VL_stream_new_obj(*req, o->under_vol_id);
    } /* end if */
    else
        new_obj = NULL;

    return (void *)new_obj;
} /* end H5VL_stream_object_open() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_object_copy
 *
 * Purpose:     Copies an object inside a container.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_object_copy(void *src_obj, const H5VL_loc_params_t *src_loc_params, const char *src_name,
                              void *dst_obj, const H5VL_loc_params_t *dst_loc_params, const char *dst_name,
                              hid_t ocpypl_id, hid_t lcpl_id, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o_src = (H5VL_stream_t *)src_obj;
    H5VL_stream_t *o_dst = (H5VL_stream_t *)dst_obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM OBJECT Copy\n");
#endif

    ret_value =
        H5VLobject_copy(o_src->under_object, src_loc_params, src_name, o_dst->under_object, dst_loc_params,
                        dst_name, o_src->under_vol_id, ocpypl_id, lcpl_id, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o_src->under_vol_id);

    return ret_value;
} /* end H5VL_stream_object_copy() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_object_get
 *
 * Purpose:     Get info about an object
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_object_get(void *obj, const H5VL_loc_params_t *loc_params, H5VL_object_get_args_t *args,
                             hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM OBJECT Get\n");
#endif

    ret_value = H5VLobject_get(o->under_object, loc_params, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_object_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_object_specific
 *
 * Purpose:     Specific operation on an object
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_object_specific(void *obj, const H5VL_loc_params_t *loc_params,
                                  H5VL_object_specific_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    hid_t                under_vol_id;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM OBJECT Specific\n");
#endif

    /* Save copy of underlying VOL connector ID, in case of
     * 'refresh' operation destroying the current object
     */
    under_vol_id = o->under_vol_id;

    /* H5Rcreate_object()/H5Rcreate_region() reach the connector as a LOOKUP
     * by name. The name is the application's logical path, which is not
     * where the object physically lives (steps land under "/step/<k>/"), so
     * passing it straight through makes the under connector report "object
     * doesn't exist". Translate, then look up against the file root -- the
     * resolved path is absolute, so it must not be resolved relative to o.
     * Falls through untranslated whenever translation does not apply, which
     * keeps every non-H5R caller of object_specific byte-identical. */
    if (args && args->op_type == H5VL_OBJECT_LOOKUP && loc_params &&
        loc_params->type == H5VL_OBJECT_BY_NAME && loc_params->loc_data.loc_by_name.name &&
        o->file_state) {
        char *resolved =
            H5VL__stream_resolve_physical_path(o->file_state, loc_params->loc_data.loc_by_name.name);

        if (resolved) {
            H5VL_loc_params_t resolved_params = *loc_params;

            resolved_params.obj_type                   = H5I_FILE;
            resolved_params.loc_data.loc_by_name.name  = resolved;

            ret_value = H5VLobject_specific(o->file_state->file_under_object, &resolved_params,
                                            o->file_state->file_under_vol_id, args, dxpl_id, req);
            free(resolved);

            if (req && *req)
                *req = H5VL_stream_new_obj(*req, under_vol_id);
            return ret_value;
        }
    }

    ret_value = H5VLobject_specific(o->under_object, loc_params, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, under_vol_id);

    return ret_value;
} /* end H5VL_stream_object_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_object_optional
 *
 * Purpose:     Perform a connector-specific operation for an object
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_object_optional(void *obj, const H5VL_loc_params_t *loc_params, H5VL_optional_args_t *args,
                                  hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM OBJECT Optional\n");
#endif

    ret_value = H5VLobject_optional(o->under_object, loc_params, o->under_vol_id, args, dxpl_id, req);

    /* Check for async request */
    if (req && *req)
        *req = H5VL_stream_new_obj(*req, o->under_vol_id);

    return ret_value;
} /* end H5VL_stream_object_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_introspect_get_conn_cls
 *
 * Purpose:     Query the connector class.
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_stream_introspect_get_conn_cls(void *obj, H5VL_get_conn_lvl_t lvl, const H5VL_class_t **conn_cls)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM INTROSPECT GetConnCls\n");
#endif

    /* Check for querying this connector's class */
    if (H5VL_GET_CONN_LVL_CURR == lvl) {
        *conn_cls = &H5VL_stream_g;
        ret_value = 0;
    } /* end if */
    else
        ret_value = H5VLintrospect_get_conn_cls(o->under_object, o->under_vol_id, lvl, conn_cls);

    return ret_value;
} /* end H5VL_stream_introspect_get_conn_cls() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_introspect_get_cap_flags
 *
 * Purpose:     Query the capability flags for this connector and any
 *              underlying connector(s).
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_stream_introspect_get_cap_flags(const void *_info, uint64_t *cap_flags)
{
    const H5VL_stream_info_t *info = (const H5VL_stream_info_t *)_info;
    H5VL_stream_info_t       *default_info = NULL;
    herr_t                          ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM INTROSPECT GetCapFlags\n");
#endif

    /* H5Pget_vol_cap_flags() can reach here with no connector info at all --
     * e.g. after H5Pset_vol(fapl, vol_id, NULL), which is one of the two
     * natural no-info ways to use a connector documented at
     * H5VL__stream_default_info(). Default to native there, same as
     * file_create/file_open/file_specific, instead of failing a call that
     * should be harmless.
     */
    if (!info) {
        if (NULL == (default_info = H5VL__stream_default_info()))
            return -1;
        info = default_info;
    }

    if (H5Iis_valid(info->under_vol_id) <= 0) {
        printf("\nH5VLstream.c line %d in %s: not a valid underneath VOL ID for vol-stream\n",
               __LINE__, __func__);
        if (default_info)
            H5VL_stream_info_free(default_info);
        return -1;
    }

    /* Invoke the query on the underlying VOL connector */
    ret_value = H5VLintrospect_get_cap_flags(info->under_vol_info, info->under_vol_id, cap_flags);

    /* Bitwise OR our capability flags in */
    if (ret_value >= 0)
        *cap_flags |= H5VL_stream_g.cap_flags;

    if (default_info)
        H5VL_stream_info_free(default_info);

    return ret_value;
} /* end H5VL_stream_introspect_get_cap_flags() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_introspect_opt_query
 *
 * Purpose:     Query if an optional operation is supported by this connector
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_stream_introspect_opt_query(void *obj, H5VL_subclass_t cls, int opt_type, uint64_t *flags)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM INTROSPECT OptQuery\n");
#endif

    /* Answer for our own operations.  This is the substitute for a dedicated
     * streaming capability flag, which would require an HDF5 library change:
     * applications and tools can discover the step API through the existing
     * H5VLquery_optional() mechanism.
     */
    if (cls == H5VL_SUBCLS_FILE &&
        (opt_type == H5VL_stream_op_begin_step || opt_type == H5VL_stream_op_end_step)) {
        /* Step boundaries must be called by every rank, so say so here rather
         * than only in documentation. */
        *flags = H5VL_OPT_QUERY_SUPPORTED | H5VL_OPT_QUERY_COLLECTIVE | H5VL_OPT_QUERY_MODIFY_METADATA;
        return 0;
    }
    else if (cls == H5VL_SUBCLS_FILE && opt_type == H5VL_stream_op_step_status) {
        *flags = H5VL_OPT_QUERY_SUPPORTED | H5VL_OPT_QUERY_QUERY_METADATA;
        return 0;
    }
    else if (cls == H5VL_SUBCLS_FILE && opt_type == H5VL_stream_op_subscribe) {
        *flags = H5VL_OPT_QUERY_SUPPORTED | H5VL_OPT_QUERY_MODIFY_METADATA;
        return 0;
    }
    else if (cls == H5VL_SUBCLS_FILE && opt_type == H5VL_stream_op_begin_logical_step) {
        /* Collective for forward-consistency with begin_step/end_step, in
         * anticipation of a parallel reader -- not itself exercised by M3's
         * single-process scope. */
        *flags = H5VL_OPT_QUERY_SUPPORTED | H5VL_OPT_QUERY_COLLECTIVE | H5VL_OPT_QUERY_MODIFY_METADATA;
        return 0;
    }
    else if (cls == H5VL_SUBCLS_FILE && opt_type == H5VL_stream_op_get_logical_steps) {
        *flags = H5VL_OPT_QUERY_SUPPORTED | H5VL_OPT_QUERY_QUERY_METADATA;
        return 0;
    }
    else if (cls == H5VL_SUBCLS_FILE && opt_type == H5VL_stream_op_wait_step_ready) {
        /* Registered either way (so H5VLquery_optional() always resolves the
         * op string), but only meaningfully usable when the connector was
         * built with Mercury and the file's transport came up -- the
         * file_optional() handler above is what actually enforces that,
         * returning -1 rather than blocking forever when it is not. */
        *flags = H5VL_OPT_QUERY_SUPPORTED | H5VL_OPT_QUERY_QUERY_METADATA;
        return 0;
    }
    else if (cls == H5VL_SUBCLS_FILE && opt_type == H5VL_stream_op_set_queue_policy) {
        *flags = H5VL_OPT_QUERY_SUPPORTED | H5VL_OPT_QUERY_MODIFY_METADATA;
        return 0;
    }
    else if (cls == H5VL_SUBCLS_FILE && opt_type == H5VL_stream_op_get_subscribed_data) {
        *flags = H5VL_OPT_QUERY_SUPPORTED | H5VL_OPT_QUERY_QUERY_METADATA;
        return 0;
    }

    ret_value = H5VLintrospect_opt_query(o->under_object, o->under_vol_id, cls, opt_type, flags);

    return ret_value;
} /* end H5VL_stream_introspect_opt_query() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_request_wait
 *
 * Purpose:     Wait (with a timeout) for an async operation to complete
 *
 * Note:        Does NOT release the request on completion. H5ES's own
 *              H5ES__event_free() unconditionally calls the connector's
 *              request_free() on every completed event regardless of what
 *              wait() reported, via H5ES__event_completed() ->
 *              H5ES__event_free() -> H5VL_request_free() in the same
 *              H5ESwait() call. A version of this function that freed the
 *              request as soon as *status left IN_PROGRESS double-freed it
 *              here. free() is the sole place a request is released.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_request_wait(void *obj, uint64_t timeout, H5VL_request_status_t *status)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM REQUEST Wait\n");
#endif

    /* M4: a deferred write/attr-write request -- see
     * H5VL_stream_step_completion_t's comment. There is no background
     * progress to wait on: resolution only happens inside end_step(), called
     * explicitly by the application, so this is a non-blocking status check
     * regardless of timeout, exactly like the under-connector case above
     * when the underlying operation already finished. */
    if (o->obj_state == H5VL_STREAM_OBJ_DEFERRED_REQUEST) {
        (void)timeout;
        if (o->deferred_completion->status == 0)
            *status = H5VL_REQUEST_STATUS_IN_PROGRESS;
        else
            *status = (o->deferred_completion->status > 0) ? H5VL_REQUEST_STATUS_SUCCEED
                                                             : H5VL_REQUEST_STATUS_FAIL;
        return 0;
    }

    ret_value = H5VLrequest_wait(o->under_object, o->under_vol_id, timeout, status);

    return ret_value;
} /* end H5VL_stream_request_wait() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_request_notify
 *
 * Purpose:     Registers a user callback to be invoked when an asynchronous
 *              operation completes
 *
 * Note:        Releases the request, if connector callback succeeds
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_request_notify(void *obj, H5VL_request_notify_t cb, void *ctx)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM REQUEST Notify\n");
#endif

    /* M4: a deferred write/attr-write request. If the step already resolved,
     * invoke cb immediately; otherwise queue it on the completion cell --
     * end_step() drains and invokes the whole list when it resolves the
     * cell. Either way this request's own reference is dropped now: the
     * callback registration, once handed to the cell, no longer needs this
     * particular request object to survive. */
    if (o->obj_state == H5VL_STREAM_OBJ_DEFERRED_REQUEST) {
        H5VL_stream_step_completion_t *c = o->deferred_completion;

        if (c->status == 0) {
            H5VL_stream_step_notify_t *n = (H5VL_stream_step_notify_t *)malloc(sizeof(*n));

            if (!n)
                return -1;
            n->cb   = cb;
            n->ctx  = ctx;
            n->next = c->notify_list;
            c->notify_list = n;
        }
        else
            cb(ctx, (c->status > 0) ? H5VL_REQUEST_STATUS_SUCCEED : H5VL_REQUEST_STATUS_FAIL);

        H5VL__stream_deferred_request_free(o);
        return 0;
    }

    ret_value = H5VLrequest_notify(o->under_object, o->under_vol_id, cb, ctx);

    if (ret_value >= 0)
        H5VL_stream_free_obj(o);

    return ret_value;
} /* end H5VL_stream_request_notify() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_request_cancel
 *
 * Purpose:     Cancels an asynchronous operation
 *
 * Note:        Releases the request, if connector callback succeeds
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_request_cancel(void *obj, H5VL_request_status_t *status)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM REQUEST Cancel\n");
#endif

    /* M4: the payload is already captured; canceling a deferred write would
     * mean unwinding it out of the pending buffer, which nothing in M4's
     * scope needs. Report CANT_CANCEL -- a legal, honest answer -- rather
     * than pretending to support it. */
    if (o->obj_state == H5VL_STREAM_OBJ_DEFERRED_REQUEST) {
        *status = H5VL_REQUEST_STATUS_CANT_CANCEL;
        H5VL__stream_deferred_request_free(o);
        return 0;
    }

    ret_value = H5VLrequest_cancel(o->under_object, o->under_vol_id, status);

    if (ret_value >= 0)
        H5VL_stream_free_obj(o);

    return ret_value;
} /* end H5VL_stream_request_cancel() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_request_specific
 *
 * Purpose:     Specific operation on a request
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_request_specific(void *obj, H5VL_request_specific_args_t *args)
{
    H5VL_stream_t *o         = (H5VL_stream_t *)obj;
    herr_t               ret_value = -1;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM REQUEST Specific\n");
#endif

    /* M4: not supported on a deferred write/attr-write request -- wait(),
     * notify() and free() cover this request kind's whole surface. */
    if (o->obj_state == H5VL_STREAM_OBJ_DEFERRED_REQUEST)
        return -1;

    ret_value = H5VLrequest_specific(o->under_object, o->under_vol_id, args);

    return ret_value;
} /* end H5VL_stream_request_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_request_optional
 *
 * Purpose:     Perform a connector-specific operation for a request
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_request_optional(void *obj, H5VL_optional_args_t *args)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM REQUEST Optional\n");
#endif

    /* M4: not supported on a deferred write/attr-write request. */
    if (o->obj_state == H5VL_STREAM_OBJ_DEFERRED_REQUEST)
        return -1;

    ret_value = H5VLrequest_optional(o->under_object, o->under_vol_id, args);

    return ret_value;
} /* end H5VL_stream_request_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_request_free
 *
 * Purpose:     Releases a request, allowing the operation to complete without
 *              application tracking
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_request_free(void *obj)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM REQUEST Free\n");
#endif

    /* M4: releases this request's reference on the step's completion cell.
     * The cell itself lives on via file_state's reference (if the step
     * hasn't ended yet) or any other request still holding it. */
    if (o->obj_state == H5VL_STREAM_OBJ_DEFERRED_REQUEST) {
        H5VL__stream_deferred_request_free(o);
        return 0;
    }

    ret_value = H5VLrequest_free(o->under_object, o->under_vol_id);

    if (ret_value >= 0)
        H5VL_stream_free_obj(o);

    return ret_value;
} /* end H5VL_stream_request_free() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_blob_put
 *
 * Purpose:     Handles the blob 'put' callback
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_stream_blob_put(void *obj, const void *buf, size_t size, void *blob_id, void *ctx)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM BLOB Put\n");
#endif

    ret_value = H5VLblob_put(o->under_object, o->under_vol_id, buf, size, blob_id, ctx);

    return ret_value;
} /* end H5VL_stream_blob_put() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_blob_get
 *
 * Purpose:     Handles the blob 'get' callback
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_stream_blob_get(void *obj, const void *blob_id, void *buf, size_t size, void *ctx)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM BLOB Get\n");
#endif

    ret_value = H5VLblob_get(o->under_object, o->under_vol_id, blob_id, buf, size, ctx);

    return ret_value;
} /* end H5VL_stream_blob_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_blob_specific
 *
 * Purpose:     Handles the blob 'specific' callback
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_stream_blob_specific(void *obj, void *blob_id, H5VL_blob_specific_args_t *args)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM BLOB Specific\n");
#endif

    ret_value = H5VLblob_specific(o->under_object, o->under_vol_id, blob_id, args);

    return ret_value;
} /* end H5VL_stream_blob_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_blob_optional
 *
 * Purpose:     Handles the blob 'optional' callback
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_stream_blob_optional(void *obj, void *blob_id, H5VL_optional_args_t *args)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM BLOB Optional\n");
#endif

    ret_value = H5VLblob_optional(o->under_object, o->under_vol_id, blob_id, args);

    return ret_value;
} /* end H5VL_stream_blob_optional() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_token_cmp
 *
 * Purpose:     Compare two of the connector's object tokens, setting
 *              *cmp_value, following the same rules as strcmp().
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_token_cmp(void *obj, const H5O_token_t *token1, const H5O_token_t *token2, int *cmp_value)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM TOKEN Compare\n");
#endif

    /* Sanity checks */
    assert(obj);
    assert(token1);
    assert(token2);
    assert(cmp_value);

    ret_value = H5VLtoken_cmp(o->under_object, o->under_vol_id, token1, token2, cmp_value);

    return ret_value;
} /* end H5VL_stream_token_cmp() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_token_to_str
 *
 * Purpose:     Serialize the connector's object token into a string.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_token_to_str(void *obj, H5I_type_t obj_type, const H5O_token_t *token, char **token_str)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM TOKEN To string\n");
#endif

    /* Sanity checks */
    assert(obj);
    assert(token);
    assert(token_str);

    ret_value = H5VLtoken_to_str(o->under_object, obj_type, o->under_vol_id, token, token_str);

    return ret_value;
} /* end H5VL_stream_token_to_str() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_stream_token_from_str
 *
 * Purpose:     Deserialize the connector's object token from a string.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t
H5VL_stream_token_from_str(void *obj, H5I_type_t obj_type, const char *token_str, H5O_token_t *token)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM TOKEN From string\n");
#endif

    /* Sanity checks */
    assert(obj);
    assert(token);
    assert(token_str);

    ret_value = H5VLtoken_from_str(o->under_object, obj_type, o->under_vol_id, token_str, token);

    return ret_value;
} /* end H5VL_stream_token_from_str() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_optional
 *
 * Purpose:     Handles the generic 'optional' callback
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_stream_optional(void *obj, H5VL_optional_args_t *args, hid_t dxpl_id, void **req)
{
    H5VL_stream_t *o = (H5VL_stream_t *)obj;
    herr_t               ret_value;

#ifdef ENABLE_STREAM_LOGGING
    printf("------- VOL-STREAM generic Optional\n");
#endif

    ret_value = H5VLoptional(o->under_object, o->under_vol_id, args, dxpl_id, req);

    return ret_value;
} /* end H5VL_stream_optional() */

/*-------------------------------------------------------------------------
 * Registration, plugin entry points, and the public step API.
 *
 * Everything below is vol-stream's own; the callbacks above are the adapted
 * pass-through skeleton.
 *-------------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_register
 *
 * Purpose:     Register the connector and cache its ID.  Only needed when the
 *              connector is linked directly; the plugin loader path used by
 *              HDF5_VOL_CONNECTOR does not call this.
 *
 * Return:      Connector ID / H5I_INVALID_HID
 *
 *-------------------------------------------------------------------------
 */
hid_t
H5VL_stream_register(void)
{
    /* Idempotent: return the cached ID if the connector is already registered. */
    if (H5VL_STREAM_g > 0 && H5Iget_type(H5VL_STREAM_g) == H5I_VOL)
        return H5VL_STREAM_g;

    if ((H5VL_STREAM_g = H5VLregister_connector(&H5VL_stream_g, H5P_DEFAULT)) < 0)
        return H5I_INVALID_HID;

    return H5VL_STREAM_g;
} /* end H5VL_stream_register() */

/* Plugin loader entry points.  These are what make HDF5_VOL_CONNECTOR=vol-stream
 * work, and are the one piece the in-tree pass-through template does not have,
 * since it is compiled into the library rather than dlopen'd.
 */
H5PL_type_t
H5PLget_plugin_type(void)
{
    return H5PL_TYPE_VOL;
}

const void *
H5PLget_plugin_info(void)
{
    return &H5VL_stream_g;
}

/*-------------------------------------------------------------------------
 * Helper: resolve the connector ID for a file and invoke one of our
 *         registered optional operations on it.
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_file_op(hid_t file_id, int op_type, void *op_args)
{
    H5VL_optional_args_t args;
    hid_t                connector_id = H5I_INVALID_HID;
    herr_t               ret_value;

    if (op_type < 0)
        return -1; /* connector never initialized, so the op is unregistered */

    /* Ask the file which connector it is using, rather than assuming ours: this
     * gives a clear failure when the call is made on a file opened natively.
     */
    if ((connector_id = H5VLget_connector_id(file_id)) < 0)
        return -1;

    args.op_type = op_type;
    args.args    = op_args;

    ret_value = H5VLfile_optional_op(file_id, &args, H5P_DEFAULT, H5ES_NONE);

    H5VLclose(connector_id);

    return ret_value;
} /* end H5VL__stream_file_op() */

herr_t
H5Fbegin_step(hid_t file_id, size_t n_logical, const uint64_t *logical_ids, uint64_t wall_time_ns)
{
    H5VL_stream_args_begin_step_t op_args;

    if (n_logical > 0 && !logical_ids)
        return -1;

    op_args.n_logical    = n_logical;
    op_args.logical_ids  = logical_ids;
    op_args.wall_time_ns = wall_time_ns;

    return H5VL__stream_file_op(file_id, H5VL_stream_op_begin_step, &op_args);
} /* end H5Fbegin_step() */

herr_t
H5Fend_step(hid_t file_id)
{
    return H5VL__stream_file_op(file_id, H5VL_stream_op_end_step, NULL);
} /* end H5Fend_step() */

herr_t
H5Fstep_status(hid_t file_id, H5F_step_status_t *status)
{
    H5VL_stream_args_step_status_t op_args;

    if (!status)
        return -1;

    op_args.status = status;

    return H5VL__stream_file_op(file_id, H5VL_stream_op_step_status, &op_args);
} /* end H5Fstep_status() */

herr_t
H5Fbegin_logical_step(hid_t file_id, uint64_t logical_id)
{
    H5VL_stream_args_begin_logical_step_t op_args;

    op_args.logical_id = logical_id;

    return H5VL__stream_file_op(file_id, H5VL_stream_op_begin_logical_step, &op_args);
} /* end H5Fbegin_logical_step() */

herr_t
H5Fget_logical_steps(hid_t file_id, size_t *n_logical, uint64_t *logical_ids)
{
    H5VL_stream_args_get_logical_steps_t op_args;

    if (!n_logical)
        return -1;

    op_args.n_logical   = n_logical;
    op_args.logical_ids = logical_ids;

    return H5VL__stream_file_op(file_id, H5VL_stream_op_get_logical_steps, &op_args);
} /* end H5Fget_logical_steps() */

herr_t
H5Fsubscribe(hid_t file_id, size_t count, const char *const *paths, const hid_t *spaces,
             const hid_t *plists)
{
    H5VL_stream_args_subscribe_t op_args;

    if (count > 0 && (!paths || !spaces))
        return -1;

    op_args.count  = count;
    op_args.paths  = paths;
    op_args.spaces = spaces;
    op_args.plists = plists;

    return H5VL__stream_file_op(file_id, H5VL_stream_op_subscribe, &op_args);
} /* end H5Fsubscribe() */

herr_t
H5Fwait_step_ready(hid_t file_id, uint64_t timeout_ms, uint64_t *physical_step, uint64_t *wall_time_ns)
{
    H5VL_stream_args_wait_step_ready_t op_args;

    if (!physical_step)
        return -1;

    op_args.timeout_ms    = timeout_ms;
    op_args.physical_step = physical_step;
    op_args.wall_time_ns  = wall_time_ns;

    return H5VL__stream_file_op(file_id, H5VL_stream_op_wait_step_ready, &op_args);
} /* end H5Fwait_step_ready() */

herr_t
H5Fset_stream_queue_policy(hid_t file_id, H5VL_stream_queue_policy_t policy, uint64_t reserve_slots)
{
    H5VL_stream_args_set_queue_policy_t op_args;

    op_args.policy        = policy;
    op_args.reserve_slots = reserve_slots;

    return H5VL__stream_file_op(file_id, H5VL_stream_op_set_queue_policy, &op_args);
} /* end H5Fset_stream_queue_policy() */

herr_t
H5Fget_subscribed_data(hid_t file_id, uint64_t timeout_ms, uint64_t *physical_step, char **path, void **buf,
                         size_t *size, uint64_t *elem_start, uint64_t *elem_count)
{
    H5VL_stream_args_get_subscribed_data_t op_args;

    if (!physical_step || !path || !buf || !size || !elem_start || !elem_count)
        return -1;

    op_args.timeout_ms    = timeout_ms;
    op_args.physical_step = physical_step;
    op_args.path            = path;
    op_args.buf             = buf;
    op_args.size            = size;
    op_args.elem_start     = elem_start;
    op_args.elem_count     = elem_count;

    return H5VL__stream_file_op(file_id, H5VL_stream_op_get_subscribed_data, &op_args);
} /* end H5Fget_subscribed_data() */
