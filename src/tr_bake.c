/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Purpose: Implementation of tr_bake.h. See that header for the design note.
 */

#include "tr_bake.h"

#include <abt-io.h>
#include <bake-client.h>
#include <bake-server.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Arbitrary but fixed: exactly one BAKE provider per writer process in this
 * increment, so there is no discovery problem to solve -- a future reader-
 * side fetch (see tr_bake.h's file comment) would just need this constant
 * and the writer's already-known transport address. */
#define VS_BAKE_PROVIDER_ID 42

/* Ignored by the file backend's own extend-on-write behavior (confirmed
 * against BAKE 0.6.4's bake-mkpool --help: "-s may be omitted if backend
 * supports extending space"), but bake_provider_create_target() still wants
 * a starting size. */
#define VS_BAKE_INITIAL_TARGET_SIZE (64ULL * 1024 * 1024)

/* BAKE's own string round trip for a region id (bake_region_id_to_string())
 * does not document a max length; 20 raw bytes (BAKE_REGION_ID_DATA_SIZE +
 * the type field) comfortably fits any reasonable textual encoding. */
#define VS_BAKE_DESC_BUF_LEN 128

struct vs_bake_t {
    margo_instance_id       mid;
    abt_io_instance_id      aid;
    bake_provider_t         provider;
    bake_client_t            client;
    bake_provider_handle_t   bph;
    bake_target_id_t         target;
    char                    *target_path;
};

vs_bake_t *
vs_bake_start(margo_instance_id mid, const char *spill_dir)
{
    vs_bake_t                      *b;
    struct bake_provider_init_info  info;
    hg_addr_t                       self_addr = HG_ADDR_NULL;
    size_t                          path_len;

    if (mid == MARGO_INSTANCE_NULL || !spill_dir)
        return NULL;

    if (NULL == (b = (vs_bake_t *)calloc(1, sizeof(*b))))
        return NULL;
    b->mid = mid;

    if (ABT_IO_INSTANCE_NULL == (b->aid = abt_io_init(1))) {
        free(b);
        return NULL;
    }

    /* "vol-stream-spill-<pid>.dat" under spill_dir -- one target file per
     * writer process, never shared, so the pid is enough to avoid collision
     * with a previous or concurrent run using the same directory. */
    path_len = strlen(spill_dir) + 32;
    if (NULL == (b->target_path = (char *)malloc(path_len))) {
        abt_io_finalize(b->aid);
        free(b);
        return NULL;
    }
    snprintf(b->target_path, path_len, "%s/vol-stream-spill-%ld.dat", spill_dir, (long)getpid());

    memset(&info, 0, sizeof(info));
    info.json_config = "{\"pipeline_enable\":true}"; /* required for the file backend, see M7 research */
    info.aid          = b->aid;

    if (BAKE_SUCCESS != bake_provider_register(mid, VS_BAKE_PROVIDER_ID, &info, &b->provider)) {
        free(b->target_path);
        abt_io_finalize(b->aid);
        free(b);
        return NULL;
    }

    {
        char target_spec[600];

        snprintf(target_spec, sizeof(target_spec), "file:%s", b->target_path);
        if (BAKE_SUCCESS != bake_provider_create_target(b->provider, target_spec,
                                                          VS_BAKE_INITIAL_TARGET_SIZE, &b->target)) {
            bake_provider_deregister(b->provider);
            free(b->target_path);
            abt_io_finalize(b->aid);
            free(b);
            return NULL;
        }
    }

    if (BAKE_SUCCESS != bake_client_init(mid, &b->client)) {
        bake_provider_deregister(b->provider);
        free(b->target_path);
        abt_io_finalize(b->aid);
        free(b);
        return NULL;
    }

    /* The writer is also its own BAKE client -- the standard Mochi pattern
     * for a service that is a client of itself, see the file comment. */
    if (HG_SUCCESS != margo_addr_self(mid, &self_addr)) {
        bake_client_finalize(b->client);
        bake_provider_deregister(b->provider);
        free(b->target_path);
        abt_io_finalize(b->aid);
        free(b);
        return NULL;
    }

    if (BAKE_SUCCESS != bake_provider_handle_create(b->client, self_addr, VS_BAKE_PROVIDER_ID, &b->bph)) {
        margo_addr_free(mid, self_addr);
        bake_client_finalize(b->client);
        bake_provider_deregister(b->provider);
        free(b->target_path);
        abt_io_finalize(b->aid);
        free(b);
        return NULL;
    }
    margo_addr_free(mid, self_addr); /* bake_provider_handle_create() keeps its own reference */

    return b;
}

void
vs_bake_stop(vs_bake_t *b)
{
    if (!b)
        return;

    if (b->bph)
        bake_provider_handle_release(b->bph);
    if (b->client)
        bake_client_finalize(b->client);
    if (b->provider) {
        bake_provider_detach_target(b->provider, b->target);
        bake_provider_deregister(b->provider);
    }
    if (b->aid != ABT_IO_INSTANCE_NULL)
        abt_io_finalize(b->aid);

    free(b->target_path);
    free(b);
}

int
vs_bake_spill_write(vs_bake_t *b, const void *buf, uint64_t size, char **out_desc)
{
    bake_region_id_t rid;
    char              desc_buf[VS_BAKE_DESC_BUF_LEN];

    if (!b || !buf || size == 0 || !out_desc)
        return -1;

    if (BAKE_SUCCESS != bake_create_write_persist(b->bph, b->target, buf, size, &rid))
        return -1;

    if (BAKE_SUCCESS != bake_region_id_to_string(rid, desc_buf, sizeof(desc_buf)))
        return -1;

    if (NULL == (*out_desc = strdup(desc_buf)))
        return -1;

    return 0;
}

int
vs_bake_spill_read(vs_bake_t *b, const char *desc, uint64_t size, void **out_buf)
{
    bake_region_id_t rid;
    uint64_t          bytes_read = 0;
    void             *buf;

    if (!b || !desc || size == 0 || !out_buf)
        return -1;

    if (BAKE_SUCCESS != bake_region_id_from_string(desc, &rid))
        return -1;
    if (NULL == (buf = malloc(size)))
        return -1;
    if (BAKE_SUCCESS != bake_read(b->bph, b->target, rid, 0, buf, size, &bytes_read) || bytes_read != size) {
        free(buf);
        return -1;
    }

    *out_buf = buf;
    return 0;
}

int
vs_bake_spill_remove(vs_bake_t *b, const char *desc)
{
    bake_region_id_t rid;

    if (!b || !desc)
        return -1;
    if (BAKE_SUCCESS != bake_region_id_from_string(desc, &rid))
        return -1;
    return (BAKE_SUCCESS == bake_remove(b->bph, b->target, rid)) ? 0 : -1;
}
