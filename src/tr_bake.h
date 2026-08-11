/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Purpose: M7's Spill queue policy needs somewhere fast to put a step's
 *          bytes when the writer chooses not to wait for a lagging reader
 *          and not to lose the data either -- node-local storage via BAKE
 *          (a Mochi provider for byte-addressable regions; see
 *          docs/dev-plan.md's M7 section). This module embeds a BAKE
 *          provider directly on the writer's own margo instance (the same
 *          one tr_mercury.c already drives), following the pattern BAKE's
 *          own README documents for "integrating into a larger service"
 *          rather than standing up a separate bake-server-daemon process.
 *          The writer is also its own BAKE client (the standard Mochi
 *          pattern for a service that is a client of itself): draining a
 *          spilled step is a local RPC over shared memory, not a new
 *          process or a new address for anything else to discover.
 *
 *          Scope: this increment's spill target lives only as long as the
 *          writer process and is read back only by the writer itself, to
 *          complete a deferred H5VL__stream_replay_manifest() once reader
 *          pressure subsides (see H5VLstream.c's M7 comments). A reader
 *          fetching a still-spilled step's bytes directly from BAKE, before
 *          the writer drains it, is a real generalization this does not
 *          attempt yet.
 *
 *          Independent of HDF5 headers/types, like tr_mercury.c.
 */

#ifndef VOL_STREAM_TR_BAKE_H
#define VOL_STREAM_TR_BAKE_H

#include <margo.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vs_bake_t vs_bake_t;

/* Embeds a BAKE provider (file backend -- no real persistent-memory hardware
 * needed) on mid, managing a fresh node-local target created under
 * spill_dir (e.g. a scratch/tmp directory on this node; the caller picks it,
 * same as it picks na_str for vs_tr_start()). Returns NULL on failure --
 * that is not fatal to the connector as a whole, only to Spill: Block and
 * Discard need no transport beyond what tr_mercury.c already provides. */
vs_bake_t *vs_bake_start(margo_instance_id mid, const char *spill_dir);

/* Detaches the target and releases every resource vs_bake_start() allocated.
 * Does not delete the underlying target file -- the caller (H5VLstream.c)
 * owns spill_dir and cleans it up, same convention as the .vsgroup sidecar
 * file from M5. */
void vs_bake_stop(vs_bake_t *b);

/* Writes buf (one spilled step's manifest+payload bytes, see
 * H5VL__stream_spill_step()) to the local target in one RTT. *out_desc is a
 * newly allocated, NUL-terminated string identifying the region -- opaque to
 * the caller, pass it back to vs_bake_spill_read()/vs_bake_spill_remove().
 * Returns 0 on success, -1 on failure. */
int vs_bake_spill_write(vs_bake_t *b, const void *buf, uint64_t size, char **out_desc);

/* Reads back a region written by vs_bake_spill_write() on this same vs_bake_t
 * (this increment's spill target is drained only by the writer that created
 * it, never by a remote reader -- see the file comment). size must be
 * exactly what vs_bake_spill_write() was given for this region -- the file
 * backend's bake_get_size() returns BAKE_ERR_OP_UNSUPPORTED (confirmed
 * against BAKE 0.6.4 directly), so the caller must already know it rather
 * than query it back. *out_buf is newly malloc()'d, size bytes; caller
 * frees. Returns 0 on success, -1 on failure. */
int vs_bake_spill_read(vs_bake_t *b, const char *desc, uint64_t size, void **out_buf);

/* Reclaims a region's space once its step has been fully drained back into
 * the real file. Not fatal to leave undone (the region just sits there until
 * the target is torn down with the rest of spill_dir) so callers may ignore
 * a failure here. Returns 0 on success, -1 on failure. */
int vs_bake_spill_remove(vs_bake_t *b, const char *desc);

#ifdef __cplusplus
}
#endif

#endif /* VOL_STREAM_TR_BAKE_H */
