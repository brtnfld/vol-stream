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

/* This connector's header */
#include "H5VLnative.h"
#include "H5PLextern.h"
#include "H5VLstream.h"

/* Generated from src/vol_stream.fbs by flatcc (see CMakeLists.txt). Pulls in
 * vol_stream_reader.h and flatcc/flatcc_builder.h transitively -- the step
 * manifest schema, and the builder/reader API used to serialize and decode it.
 */
#include "vol_stream_builder.h"

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
};

/* Whether a wrapped object is a real, opened/created underlying object, or a
 * bookkeeping placeholder standing in for a create+write deferred into the
 * pending step manifest. H5VL_STREAM_OBJ_LIVE is 0, the calloc() default, so
 * every object not explicitly made a placeholder is correctly LIVE. */
typedef enum H5VL_stream_obj_state_t {
    H5VL_STREAM_OBJ_LIVE        = 0,
    H5VL_STREAM_OBJ_PLACEHOLDER = 1
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
static H5VL_stream_t *H5VL__stream_new_child_obj(void *under_obj, hid_t under_vol_id,
                             H5VL_stream_file_state_t *file_state, const char *parent_path,
                             const char *name);
static size_t H5VL__stream_pending_append(H5VL_stream_file_state_t *fs,
                             const H5VL_stream_pending_entry_t *entry);
static hid_t  H5VL__stream_resolve_space(hid_t space_id, hid_t fallback_space_id);
static herr_t H5VL__stream_replay_step(H5VL_stream_t *file_obj);

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
static int H5VL_stream_op_begin_step  = -1;
static int H5VL_stream_op_end_step    = -1;
static int H5VL_stream_op_step_status = -1;
static int H5VL_stream_op_subscribe   = -1;

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
 * Purpose:     Drop a reference; frees the pending-entry buffer and the
 *              struct itself once the last wrapper (file or child) referring
 *              to it closes.
 *-------------------------------------------------------------------------
 */
static void
H5VL__stream_file_state_decref(H5VL_stream_file_state_t *fs)
{
    if (--fs->refcount > 0)
        return;

    H5VL__stream_pending_discard_all(fs);
    free(fs->logical_ids);
    free(fs);
} /* end H5VL__stream_file_state_decref() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__stream_child_path
 *
 * Purpose:     Build an absolute path for a child of parent_path (the file
 *              root's own path is ""). Returns NULL (propagating
 *              "unresolvable") if parent_path is NULL.
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

    len  = strlen(parent_path) + 1 /* '/' */ + strlen(name) + 1 /* '\0' */;
    if (NULL == (path = (char *)malloc(len)))
        return NULL;

    snprintf(path, len, "%s/%s", parent_path, name);

    return path;
} /* end H5VL__stream_child_path() */

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
 * Function:    H5VL__stream_replay_step
 *
 * Purpose:     The M2 core. Encodes file_obj->file_state's pending entries
 *              into a flatcc Step manifest, then decodes that manifest back
 *              and replays it -- creating real objects group-based under
 *              /step/<physical_step>/ from the *decoded* ids, never the live
 *              ones captured at create/write time. That decode round trip is
 *              the point: it is what proves the manifest -- not just the
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
 * Return:      Success:    0
 *              Failure:    -1 (the caller discards the pending buffer either
 *                          way -- a partial replay is not salvaged)
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__stream_replay_step(H5VL_stream_t *file_obj)
{
    H5VL_stream_file_state_t *fs = file_obj->file_state;
    flatcc_builder_t           B;
    int                        builder_ready = 0;
    vs_Entry_ref_t            *entry_refs    = NULL;
    uint8_t                   *payload_buf   = NULL;
    size_t                     payload_cap   = 0;
    size_t                     payload_len   = 0;
    void                     **replay_under  = NULL;
    uint8_t                   *needs_close   = NULL;
    uint8_t                   *manifest_buf  = NULL;
    size_t                     manifest_len  = 0;
    char                      *step_root     = NULL;
    unsigned                   maj, minor, rel;
    size_t                     i;
    herr_t                     ret_value = 0;

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

    if (NULL == (step_root = (char *)malloc(32))) { /* "/step/" + up to 20 digits + '\0' */
        ret_value = -1;
        goto done;
    }
    snprintf(step_root, 32, "/step/%llu", (unsigned long long)fs->physical_step);

    /* Pass 2: decode the manifest just built and replay it entry by entry,
     * using only the decoded ids -- see the function comment above. */
    {
        vs_Step_table_t step      = vs_Step_as_root(manifest_buf);
        vs_Entry_vec_t  entries   = step ? vs_Step_entries(step) : NULL;
        size_t          n_entries = entries ? vs_Entry_vec_len(entries) : 0;

        if (!step || n_entries != fs->n_pending) {
            ret_value = -1;
            goto done;
        }

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

            if ((dtype = H5Tdecode2(type_enc, flatbuffers_uint8_vec_len(type_enc))) < 0 ||
                (dspace = H5Sdecode(space_enc)) < 0) {
                if (dtype >= 0)
                    H5Tclose(dtype);
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

                if (fs->pending[i].owner_wrapper) {
                    fs->pending[i].owner_wrapper->under_object = real;
                    fs->pending[i].owner_wrapper->obj_state    = H5VL_STREAM_OBJ_LIVE;
                }
                else
                    needs_close[i] = 1;
            }
            else if (kind == vs_Kind_DsetWrite) {
                void    *real = NULL;
                size_t   j;
                hid_t    mem_space = -1;
                hssize_t n_elem;

                for (j = i; j-- > 0;)
                    if (vs_Entry_kind(vs_Entry_vec_at(entries, j)) == vs_Kind_DsetCreate &&
                        strcmp(vs_Entry_path(vs_Entry_vec_at(entries, j)), path) == 0) {
                        real = replay_under[j];
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
                    (void)plen;
                    if (w < 0) {
                        H5Tclose(dtype);
                        H5Sclose(dspace);
                        ret_value = -1;
                        goto done;
                    }
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
                }

                if (fs->pending[i].owner_wrapper) {
                    fs->pending[i].owner_wrapper->under_object = attr;
                    fs->pending[i].owner_wrapper->obj_state    = H5VL_STREAM_OBJ_LIVE;
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
    if (manifest_buf)
        flatcc_builder_free(manifest_buf);
    if (builder_ready)
        flatcc_builder_clear(&B);
    free(entry_refs);
    free(payload_buf);
    free(replay_under);
    free(needs_close);
    free(step_root);

    return ret_value;
} /* end H5VL__stream_replay_step() */

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

    /* Release underlying VOL ID and wrap context */
    if (wrap_ctx->under_wrap_ctx)
        H5VLfree_wrap_ctx(wrap_ctx->under_wrap_ctx, wrap_ctx->under_vol_id);
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
        size_t                             path_len;
        size_t                             idx;

        path_len = strlen(o->path) + 1 /* '@' */ + strlen(name) + 1 /* '\0' */;
        if (NULL == (path = (char *)malloc(path_len)))
            return NULL;
        snprintf(path, path_len, "%s@%s", o->path, name);

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
         * wrapper's own reference) and the empty root path. */
        if (file) {
            file->file_state = H5VL__stream_file_state_new();
            file->path       = strdup("");
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
         * wrapper's own reference) and the empty root path. */
        if (file) {
            file->file_state = H5VL__stream_file_state_new();
            file->path       = strdup("");
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
        herr_t replay_ret;

        if (!o->file_state || o->file_state->step_state != H5F_STEP_IN_STEP)
            return -1;

        o->file_state->step_state = H5F_STEP_COMMITTING;

        replay_ret = H5VL__stream_replay_step(o);

        /* Discard the pending buffer either way: a failed replay may have
         * landed some entries and not others, and there is no partial-step
         * state worth preserving -- see the same reasoning for an unclosed
         * step at file_close(). */
        H5VL__stream_pending_discard_all(o->file_state);
        free(o->file_state->logical_ids);
        o->file_state->logical_ids = NULL;
        o->file_state->n_logical   = 0;

        if (replay_ret < 0) {
            o->file_state->step_state = H5F_STEP_NOT_IN_STEP;
            return -1;
        }

        o->file_state->physical_step++;
        o->file_state->step_state = H5F_STEP_NOT_IN_STEP;
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

        /* Validate now, act in M8: a subscription naming a path that cannot be
         * expressed should fail where the caller made the mistake. */
        for (size_t i = 0; i < sargs->count; i++) {
            if (!sargs->paths[i] || sargs->paths[i][0] == '\0')
                return -1;
            if (H5Sget_simple_extent_ndims(sargs->spaces[i]) < 0)
                return -1;
        }
        return 0;
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
     * -- discard it rather than leak it or replay a step nobody ended. */
    if (o->file_state && o->file_state->step_state != H5F_STEP_NOT_IN_STEP) {
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

    ret_value = H5VLintrospect_opt_query(o->under_object, o->under_vol_id, cls, opt_type, flags);

    return ret_value;
} /* end H5VL_stream_introspect_opt_query() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_stream_request_wait
 *
 * Purpose:     Wait (with a timeout) for an async operation to complete
 *
 * Note:        Releases the request if the operation has completed and the
 *              connector callback succeeds
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

    ret_value = H5VLrequest_wait(o->under_object, o->under_vol_id, timeout, status);

    if (ret_value >= 0 && *status != H5VL_REQUEST_STATUS_IN_PROGRESS)
        H5VL_stream_free_obj(o);

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
