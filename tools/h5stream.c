/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * h5stream -- inspect a vol-stream file.
 *
 * M9's tool. The milestone's premise (see docs/dev-plan.md's "existing tools
 * on a stream") is that `h5dump` assumes random access and fails unhelpfully
 * on a live stream, and that the honest answer is to ship something that does
 * understand steps rather than to pretend otherwise.
 *
 * Subcommands, as they land:
 *
 *   list FILE    Steps in the file: physical step number, the logical ids
 *                each carries, and the objects it wrote. Implemented.
 *   tail FILE    Follow a live stream, reporting steps as they commit.
 *   export FILE  Reorganize a stream into a plain static file.
 *
 * `list` deliberately reads two ways, because they answer different questions
 * and disagreeing is itself informative:
 *
 *   - Logical history comes from the *connector* (H5Fget_logical_steps()),
 *     which is the only thing that knows a restart superseded an id -- a
 *     logical id rewritten by a later step appears once, resolved to its
 *     authoritative occurrence, not once per physical write.
 *   - Per-step contents come from the *native* connector walking
 *     "/step/<n>/", which is what actually landed on disk. Reading this
 *     through the stream connector instead would report what the connector
 *     believes rather than what a plain HDF5 tool would find, and the point
 *     of a tool is to show the latter.
 *
 * ".manifest" and ".payload" are the connector's own bookkeeping and are
 * skipped: they are how the step is recorded, not what the application wrote.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"
#include "H5VLstream.h"

static void
usage(void)
{
    fprintf(stderr, "usage: h5stream list FILE\n"
                    "       h5stream export FILE OUT\n"
                    "\n"
                    "  list     Steps in FILE: physical step, logical ids, objects written.\n"
                    "  export   Write OUT: each object at its logical path, newest version.\n");
}

/* H5Literate2 callback: print one object inside a step group, skipping the
 * connector's own .manifest/.payload bookkeeping. */
static herr_t
print_member(hid_t group, const char *name, const H5L_info2_t *info, void *op_data)
{
    H5O_info2_t oinfo;
    int        *count = (int *)op_data;
    const char *kind  = "object";

    (void)info;

    if (name[0] == '.') /* .manifest / .payload -- bookkeeping, not content */
        return 0;

    if (H5Oget_info_by_name3(group, name, &oinfo, H5O_INFO_BASIC, H5P_DEFAULT) >= 0) {
        switch (oinfo.type) {
            case H5O_TYPE_DATASET:
                kind = "dataset";
                break;
            case H5O_TYPE_GROUP:
                kind = "group";
                break;
            case H5O_TYPE_NAMED_DATATYPE:
                kind = "datatype";
                break;
            default:
                break;
        }
    }

    printf("      %-9s %s\n", kind, name);
    (*count)++;
    return 0;
}

static int
cmd_list(const char *fname)
{
    hid_t     vol_id = H5I_INVALID_HID, fapl = H5I_INVALID_HID, sfid = H5I_INVALID_HID;
    hid_t     nfid = H5I_INVALID_HID, steps_grp = H5I_INVALID_HID;
    uint64_t *logical = NULL;
    size_t    n_logical = 0;
    hsize_t   n_steps = 0, i;
    int       rc = 1;

    /* --- The connector's view: logical history. --- */
    if ((vol_id = H5VL_stream_register()) < 0) {
        fprintf(stderr, "h5stream: cannot register the vol-stream connector\n");
        goto done;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        fprintf(stderr, "h5stream: cannot build a file-access property list\n");
        goto done;
    }
    H5E_BEGIN_TRY
    {
        sfid = H5Fopen(fname, H5F_ACC_RDONLY, fapl);
    }
    H5E_END_TRY

    if (sfid >= 0) {
        if (H5Fget_logical_steps(sfid, &n_logical, NULL) >= 0 && n_logical > 0) {
            if (NULL == (logical = (uint64_t *)malloc(n_logical * sizeof(uint64_t)))) {
                fprintf(stderr, "h5stream: out of memory\n");
                goto done;
            }
            if (H5Fget_logical_steps(sfid, &n_logical, logical) < 0) {
                free(logical);
                logical   = NULL;
                n_logical = 0;
            }
        }
    }

    /* Done with the connector's view. Close it before opening natively:
     * HDF5 refuses a second open of the same file whose locking flag differs
     * from the first ("file locking flag values don't match"), and the two
     * opens below deliberately differ in VOL. */
    if (sfid >= 0) {
        H5Fclose(sfid);
        sfid = H5I_INVALID_HID;
    }

    /* --- What actually landed: walk "/step/<n>/" natively. ---
     *
     * Locking stays off here too. A stream is typically still being written
     * while someone inspects it, and taking a lock against a live writer is
     * both unnecessary for a read-only walk and a good way to interfere with
     * the thing being inspected. */
    {
        hid_t nfapl = H5Pcreate(H5P_FILE_ACCESS);

        if (nfapl >= 0)
            H5Pset_file_locking(nfapl, false, true);
        nfid = H5Fopen(fname, H5F_ACC_RDONLY, nfapl >= 0 ? nfapl : H5P_DEFAULT);
        if (nfapl >= 0)
            H5Pclose(nfapl);
    }
    if (nfid < 0) {
        fprintf(stderr, "h5stream: cannot open '%s'\n", fname);
        goto done;
    }
    H5E_BEGIN_TRY
    {
        steps_grp = H5Gopen2(nfid, "/step", H5P_DEFAULT);
    }
    H5E_END_TRY

    if (steps_grp < 0) {
        printf("%s: no steps -- not a vol-stream file, or nothing was ever committed\n", fname);
        rc = 0;
        goto done;
    }
    if (H5Gget_num_objs(steps_grp, &n_steps) < 0) {
        fprintf(stderr, "h5stream: cannot count steps\n");
        goto done;
    }

    printf("%s: %llu step(s)\n", fname, (unsigned long long)n_steps);
    if (n_logical > 0) {
        size_t k;

        printf("  logical ids (deduped, authoritative):");
        for (k = 0; k < n_logical; k++)
            printf(" %llu", (unsigned long long)logical[k]);
        printf("\n");
    }
    else
        printf("  logical ids: none recorded\n");

    /* Physical steps are 0..n-1 and contiguous, so index rather than sort. */
    for (i = 0; i < n_steps; i++) {
        char  path[64];
        hid_t g;
        int   count = 0;

        snprintf(path, sizeof(path), "/step/%llu", (unsigned long long)i);
        H5E_BEGIN_TRY
        {
            g = H5Gopen2(nfid, path, H5P_DEFAULT);
        }
        H5E_END_TRY
        if (g < 0) {
            printf("  step %llu: missing\n", (unsigned long long)i);
            continue;
        }

        printf("  step %llu:\n", (unsigned long long)i);
        H5Literate2(g, H5_INDEX_NAME, H5_ITER_INC, NULL, print_member, &count);
        if (count == 0)
            printf("      (no application objects)\n");
        H5Gclose(g);
    }

    rc = 0;

done:
    free(logical);
    if (steps_grp >= 0)
        H5Gclose(steps_grp);
    if (nfid >= 0)
        H5Fclose(nfid);
    if (sfid >= 0)
        H5Fclose(sfid);
    if (fapl >= 0)
        H5Pclose(fapl);
    if (vol_id >= 0)
        H5VLclose(vol_id);
    return rc;
}

/* One logical object and the newest physical step that wrote it. */
typedef struct {
    char *name;
    long  step;
} latest_t;

struct scan {
    latest_t *ents;
    size_t    n;
    size_t    cap;
    long      step;
};

/* Record name -> newest step. Later steps overwrite earlier ones, which is
 * exactly the "state as of now" semantics a reader sees: a step that does not
 * rewrite an object leaves the previous version standing. */
static herr_t
note_member(hid_t group, const char *name, const H5L_info2_t *info, void *op_data)
{
    struct scan *sc = (struct scan *)op_data;
    size_t       i;

    (void)group;
    (void)info;

    if (name[0] == '.')
        return 0;

    for (i = 0; i < sc->n; i++)
        if (strcmp(sc->ents[i].name, name) == 0) {
            sc->ents[i].step = sc->step; /* newer wins */
            return 0;
        }

    if (sc->n == sc->cap) {
        size_t    ncap  = sc->cap ? sc->cap * 2 : 16;
        latest_t *grown = (latest_t *)realloc(sc->ents, ncap * sizeof(*grown));

        if (!grown)
            return -1;
        sc->ents = grown;
        sc->cap  = ncap;
    }
    if (NULL == (sc->ents[sc->n].name = strdup(name)))
        return -1;
    sc->ents[sc->n].step = sc->step;
    sc->n++;
    return 0;
}

/*
 * export: collapse a stream into an ordinary HDF5 file.
 *
 * A vol-stream file stores each step under "/step/<n>/", which is portable
 * but not what anyone wants to hand to a plotting script -- the object a user
 * calls "/temperature" lives at "/step/7/temperature", and which step is the
 * current one is not obvious from the outside. export writes each object once,
 * at its logical path, taking the newest step that wrote it. That is the same
 * "state as of the latest step" a reader through the connector would see, and
 * it produces a file with no vol-stream concepts in it at all.
 *
 * H5Ocopy does the actual copying, so datatypes, dataspaces, filters and
 * attributes come across without this tool needing to understand any of them.
 */
static int
cmd_export(const char *fname, const char *outname)
{
    hid_t       nfid = H5I_INVALID_HID, ofid = H5I_INVALID_HID, steps_grp = H5I_INVALID_HID;
    hid_t       fapl = H5I_INVALID_HID;
    struct scan sc   = {NULL, 0, 0, 0};
    hsize_t     n_steps = 0, i;
    size_t      k;
    int         rc = 1;

    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) >= 0)
        H5Pset_file_locking(fapl, false, true);

    if ((nfid = H5Fopen(fname, H5F_ACC_RDONLY, fapl >= 0 ? fapl : H5P_DEFAULT)) < 0) {
        fprintf(stderr, "h5stream: cannot open '%s'\n", fname);
        goto done;
    }
    H5E_BEGIN_TRY
    {
        steps_grp = H5Gopen2(nfid, "/step", H5P_DEFAULT);
    }
    H5E_END_TRY
    if (steps_grp < 0) {
        fprintf(stderr, "h5stream: '%s' has no steps to export\n", fname);
        goto done;
    }
    if (H5Gget_num_objs(steps_grp, &n_steps) < 0)
        goto done;

    /* Ascending, so the last writer of a name wins. */
    for (i = 0; i < n_steps; i++) {
        char  path[64];
        hid_t g;

        snprintf(path, sizeof(path), "/step/%llu", (unsigned long long)i);
        H5E_BEGIN_TRY
        {
            g = H5Gopen2(nfid, path, H5P_DEFAULT);
        }
        H5E_END_TRY
        if (g < 0)
            continue;
        sc.step = (long)i;
        if (H5Literate2(g, H5_INDEX_NAME, H5_ITER_INC, NULL, note_member, &sc) < 0) {
            H5Gclose(g);
            fprintf(stderr, "h5stream: out of memory scanning step %llu\n", (unsigned long long)i);
            goto done;
        }
        H5Gclose(g);
    }

    if ((ofid = H5Fcreate(outname, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        fprintf(stderr, "h5stream: cannot create '%s'\n", outname);
        goto done;
    }

    for (k = 0; k < sc.n; k++) {
        char src[128];

        snprintf(src, sizeof(src), "/step/%ld/%s", sc.ents[k].step, sc.ents[k].name);
        if (H5Ocopy(nfid, src, ofid, sc.ents[k].name, H5P_DEFAULT, H5P_DEFAULT) < 0) {
            fprintf(stderr, "h5stream: cannot copy %s\n", src);
            goto done;
        }
    }

    printf("%s: exported %zu object(s) from %llu step(s) to %s\n", fname, sc.n,
           (unsigned long long)n_steps, outname);
    rc = 0;

done:
    for (k = 0; k < sc.n; k++)
        free(sc.ents[k].name);
    free(sc.ents);
    if (steps_grp >= 0)
        H5Gclose(steps_grp);
    if (ofid >= 0)
        H5Fclose(ofid);
    if (nfid >= 0)
        H5Fclose(nfid);
    if (fapl >= 0)
        H5Pclose(fapl);
    return rc;
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "list") == 0)
        return cmd_list(argv[2]);
    if (strcmp(argv[1], "export") == 0) {
        if (argc < 4) {
            usage();
            return 2;
        }
        return cmd_export(argv[2], argv[3]);
    }

    fprintf(stderr, "h5stream: unknown subcommand '%s'\n", argv[1]);
    usage();
    return 2;
}
