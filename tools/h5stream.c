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
#include <unistd.h>

#include "hdf5.h"
#include "H5FDonion.h" /* M9: addressable step history, see cmd_history() */
#include "H5VLstream.h"

static void
usage(void)
{
    fprintf(stderr, "usage: h5stream list FILE\n"
                    "       h5stream export FILE OUT\n"
                    "       h5stream history FILE OUT\n"
                    "       h5stream tail FILE [--max-steps N] [--timeout-ms T]   (needs VOL_STREAM_NA)\n"
                    "       h5stream schema FILE [--timeout-ms T]                 (needs VOL_STREAM_NA)\n"
                    "\n"
                    "  list     Steps in FILE: physical step, logical ids, objects written.\n"
                    "  export   Write OUT: each object at its logical path, newest version.\n"
                    "  history  Write OUT as an onion-VFD archive: one revision per step, each\n"
                    "           holding the stream as of that step at logical paths. Open a past\n"
                    "           step with H5Pset_fapl_onion(revision_num = step + 1).\n"
                    "  tail     Follow a live stream, reporting each step as it commits.\n"
                    "           Requires the transport (VOL_STREAM_NA) -- a live writer's file\n"
                    "           cannot be read by another process.\n"
                    "  schema   What a *live* writer says its stream carries: every path, its\n"
                    "           datatype and its current extent. This is `h5ls` for a stream that\n"
                    "           is still being written, which h5ls itself reports as NOT FOUND.\n");
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

/*
 * history: write an onion-backed archive in which every step is an
 * addressable revision.
 *
 * dev-plan.md/design-plan.md's "stream and archive as one object": ADIOS2
 * makes you choose an engine, BP5 to a file or SST to a stream, and the onion
 * VFD already models an append-only numbered revision history. A step that is
 * both a live stream element and a nameable, re-openable revision collapses
 * that choice.
 *
 * Why this is a *tool* and not something end_step() does live, measured
 * rather than assumed: the onion VFD commits a revision when the file is
 * closed, and there is no "commit a revision now" call. One revision per step
 * would therefore mean closing and reopening the underlying file inside every
 * end_step() -- which would invalidate every under-object the connector and
 * the application still hold, including the dataset handle an application
 * legitimately keeps open across steps (see test/t_step_rewrite.c). Paying
 * that in the live writer to gain something a post-hoc pass produces for free
 * is a bad trade, so the history is built from a finished stream.
 *
 * The layout inside each revision is `export`'s, not the stream's: objects
 * live at their logical paths, with no /step groups. Revision N is therefore
 * "the file as a user would have seen it, as of step N-1" -- readable by any
 * HDF5 program, no vol-stream connector involved.
 *
 * Revision numbering is inherited from the VFD and is off by one: revision 0
 * is the empty canonical file that must exist before any revision can be
 * written, so step N lands in revision N+1. Rather than leave a caller to
 * rediscover that, each revision records its own step in a root attribute
 * ("vol_stream_step"), which is self-describing and needs no onion metadata
 * API to read back.
 */
/* Page size for the history: why it is chosen at runtime, and why the
 * archive records which one was used.
 *
 * Three things about the onion VFD, all measured 2026-08-15 with standalone
 * HDF5 programs containing no vol-stream code at all:
 *
 *   1. With a small archive (a few KB) and page_size >= 512, revisions are
 *      silently lost -- a three-session history reads back with the first
 *      revision empty and the third identical to the second. At 32/64/128/
 *      256 the same program is correct, and once the file comfortably
 *      exceeds one page every size tested is correct, 4096 included.
 *   2. A reader MUST pass the page size the history was written with.
 *      H5FDonion.h documents 0 as "whatever the file already uses", but a
 *      revision opened with 0 comes back apparently empty -- the datasets
 *      are simply not found.
 *   3. A reader that passes the *wrong* non-zero page size does not get an
 *      error, it gets an abort: `H5FD__onion_read: Assertion
 *      '0 == bytes_to_read' failed`.
 *
 * H5Ocopy is not implicated in any of it: direct H5Dcreate/H5Dwrite and
 * H5Ocopy behave identically at every page size tried. And HDF5's own onion
 * tests only ever use page sizes 4 and 32 (test/onion.c), so the failing
 * regime sits outside their coverage rather than being a misuse of the API.
 *
 * Consequences for this tool. The large page size is the default, because
 * the small one costs an index entry per 32 bytes amended and a real archive
 * cannot afford that; the history is then read back and, if it does not
 * verify, rewritten at the small size, which is cheap precisely in the
 * small-archive case where the problem appears. Whichever size wins is
 * stored as an attribute in the *canonical* file -- which stays an ordinary,
 * plainly-openable HDF5 file, since every amendment lives in the .onion --
 * so a reader can discover it without having to guess, and guessing here
 * either shows an empty file or aborts the process. */
#define H5STREAM_ONION_PAGE_SIZE      4096 /* must be a power of two */
#define H5STREAM_ONION_PAGE_SIZE_SAFE 32
#define H5STREAM_ONION_PAGE_ATTR      "vol_stream_onion_page_size"

/* Build a fapl selecting one onion revision. backing_fapl is created once by
 * the caller and outlives every fapl made here -- H5Pset_fapl_onion() keeps
 * the id rather than a copy, which is why HDF5's own tests close it only
 * after the last open. page_size 0 means "whatever the file already uses",
 * which is what every read path here wants. */
static hid_t
onion_fapl(hid_t backing_fapl, uint64_t revision, uint32_t page_size, const char *comment)
{
    H5FD_onion_fapl_info_t info;
    hid_t                  fapl;

    memset(&info, 0, sizeof(info));
    info.version         = H5FD_ONION_FAPL_INFO_VERSION_CURR;
    info.backing_fapl_id = backing_fapl;
    info.page_size       = page_size;
    info.store_target    = H5FD_ONION_STORE_TARGET_ONION;
    info.revision_num    = revision;
    if (comment)
        snprintf(info.comment, sizeof(info.comment), "%s", comment);

    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0)
        return H5I_INVALID_HID;
    if (H5Pset_fapl_onion(fapl, &info) < 0) {
        H5Pclose(fapl);
        return H5I_INVALID_HID;
    }
    return fapl;
}

/* Names written by one step, in the order H5Literate2 reports them. */
struct step_names {
    char **names;
    size_t n;
    size_t cap;
};

static herr_t
collect_member(hid_t group, const char *name, const H5L_info2_t *info, void *op_data)
{
    struct step_names *sn = (struct step_names *)op_data;

    (void)group;
    (void)info;

    if (name[0] == '.') /* .manifest / .payload -- bookkeeping, not content */
        return 0;

    if (sn->n == sn->cap) {
        size_t ncap   = sn->cap ? sn->cap * 2 : 16;
        char **grown = (char **)realloc(sn->names, ncap * sizeof(*grown));

        if (!grown)
            return -1;
        sn->names = grown;
        sn->cap   = ncap;
    }
    if (NULL == (sn->names[sn->n] = strdup(name)))
        return -1;
    sn->n++;
    return 0;
}

/* Stamp this revision with the physical step it represents, so a reader can
 * confirm the off-by-one mapping from the file itself. */
static int
write_step_attr(hid_t fid, unsigned long long step)
{
    hid_t sp = H5I_INVALID_HID, at = H5I_INVALID_HID;
    int   rc = -1;

    H5E_BEGIN_TRY
    {
        H5Adelete(fid, "vol_stream_step");
    }
    H5E_END_TRY

    if ((sp = H5Screate(H5S_SCALAR)) < 0)
        goto done;
    if ((at = H5Acreate2(fid, "vol_stream_step", H5T_NATIVE_ULLONG, sp, H5P_DEFAULT, H5P_DEFAULT)) < 0)
        goto done;
    if (H5Awrite(at, H5T_NATIVE_ULLONG, &step) < 0)
        goto done;
    rc = 0;

done:
    if (at >= 0)
        H5Aclose(at);
    if (sp >= 0)
        H5Sclose(sp);
    return rc;
}

/* Read every revision back and check it is the step it claims to be. The
 * onion VFD can drop revisions silently (see the page-size note above), and
 * an archive that lost half its history looks exactly like one that never
 * had it -- so the tool checks rather than reports success on faith.
 * Revisions are opened with the same page size the pass wrote, since 0 does
 * not work on the read side (see the page-size note above). */
static int
history_verify(const char *outname, hsize_t n_steps, uint32_t page_size)
{
    hid_t   backing = H5I_INVALID_HID;
    hsize_t i;
    int     rc = -1;

    if ((backing = H5Pcreate(H5P_FILE_ACCESS)) < 0)
        return -1;

    for (i = 0; i < n_steps; i++) {
        hid_t              rfapl = onion_fapl(backing, (uint64_t)(i + 1), page_size, NULL);
        hid_t              rfid  = H5I_INVALID_HID;
        hid_t              at    = H5I_INVALID_HID;
        unsigned long long stamp = (unsigned long long)-1;

        if (rfapl < 0)
            goto done;

        H5E_BEGIN_TRY
        {
            rfid = H5Fopen(outname, H5F_ACC_RDONLY, rfapl);
            if (rfid >= 0) {
                if ((at = H5Aopen(rfid, "vol_stream_step", H5P_DEFAULT)) >= 0) {
                    if (H5Aread(at, H5T_NATIVE_ULLONG, &stamp) < 0)
                        stamp = (unsigned long long)-1;
                    H5Aclose(at);
                }
            }
        }
        H5E_END_TRY

        if (rfid >= 0)
            H5Fclose(rfid);
        H5Pclose(rfapl);

        if (stamp != (unsigned long long)i)
            goto done;
    }
    rc = 0;

done:
    H5Pclose(backing);
    return rc;
}

/* One full write pass at a given page size. Everything it touches is
 * recreated from scratch, so a failed pass can simply be repeated with a
 * different page size. */
static int
history_write_pass(const char *fname, hid_t nfid, hsize_t n_steps, const char *outname,
                   const char *onion_name, const char *recovery_name, uint32_t page_size,
                   size_t *n_written)
{
    hid_t             backing = H5I_INVALID_HID;
    hsize_t           i;
    size_t            k;
    struct step_names sn = {NULL, 0, 0};
    int               rc = -1;

    (void)fname;
    *n_written = 0;

    /* A stale history alongside a reused output name would be appended to,
     * silently mixing two streams' revisions into one numbering. */
    unlink(outname);
    unlink(onion_name);
    unlink(recovery_name);

    /* Revision 0: the canonical file, which must exist before the onion VFD
     * will open anything read-write. Created plainly, exactly as HDF5's own
     * H5F-level onion tests do -- and stamped, while it is still an ordinary
     * file, with the page size a reader has to pass back. */
    {
        hid_t ofid = H5Fcreate(outname, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        hid_t sp = H5I_INVALID_HID, at = H5I_INVALID_HID;
        int   ok = 0;

        if (ofid < 0) {
            fprintf(stderr, "h5stream: cannot create '%s'\n", outname);
            return -1;
        }
        if ((sp = H5Screate(H5S_SCALAR)) >= 0 &&
            (at = H5Acreate2(ofid, H5STREAM_ONION_PAGE_ATTR, H5T_NATIVE_UINT32, sp, H5P_DEFAULT,
                             H5P_DEFAULT)) >= 0 &&
            H5Awrite(at, H5T_NATIVE_UINT32, &page_size) >= 0)
            ok = 1;
        if (at >= 0)
            H5Aclose(at);
        if (sp >= 0)
            H5Sclose(sp);
        H5Fclose(ofid);

        if (!ok) {
            fprintf(stderr, "h5stream: cannot record the page size in '%s'\n", outname);
            return -1;
        }
    }

    /* A real fapl, not H5P_DEFAULT: the onion VFD hands this to its own
     * recovery-file open, which rejects H5P_DEFAULT with "not a file access
     * property list" -- and HDF5's own H5F-level tests pass a real one. */
    if ((backing = H5Pcreate(H5P_FILE_ACCESS)) < 0)
        return -1;

    for (i = 0; i < n_steps; i++) {
        char  step_path[64], comment[32];
        hid_t g = H5I_INVALID_HID, ofid = H5I_INVALID_HID, rev_fapl = H5I_INVALID_HID;
        int   step_rc = 1;

        snprintf(step_path, sizeof(step_path), "/step/%llu", (unsigned long long)i);
        H5E_BEGIN_TRY
        {
            g = H5Gopen2(nfid, step_path, H5P_DEFAULT);
        }
        H5E_END_TRY
        if (g < 0)
            continue;

        for (k = 0; k < sn.n; k++)
            free(sn.names[k]);
        sn.n = 0;
        if (H5Literate2(g, H5_INDEX_NAME, H5_ITER_INC, NULL, collect_member, &sn) < 0) {
            H5Gclose(g);
            fprintf(stderr, "h5stream: out of memory scanning step %llu\n", (unsigned long long)i);
            goto done;
        }
        H5Gclose(g);

        snprintf(comment, sizeof(comment), "step %llu", (unsigned long long)i);
        if ((rev_fapl = onion_fapl(backing, H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST, page_size, comment)) <
            0)
            goto done;

        /* One open/close cycle is one revision -- the whole mechanism. */
        if ((ofid = H5Fopen(outname, H5F_ACC_RDWR, rev_fapl)) < 0) {
            fprintf(stderr, "h5stream: cannot open '%s' for revision %llu\n", outname,
                    (unsigned long long)(i + 1));
            H5Pclose(rev_fapl);
            goto done;
        }

        for (k = 0; k < sn.n; k++) {
            /* A step that rewrites an object replaces the previous
             * revision's copy of it, which is what makes revision N the
             * state *as of* step N-1 rather than a pile of every version. */
            H5E_BEGIN_TRY
            {
                if (H5Lexists(ofid, sn.names[k], H5P_DEFAULT) > 0)
                    H5Ldelete(ofid, sn.names[k], H5P_DEFAULT);
            }
            H5E_END_TRY

            {
                char src[192];

                snprintf(src, sizeof(src), "%s/%s", step_path, sn.names[k]);
                if (H5Ocopy(nfid, src, ofid, sn.names[k], H5P_DEFAULT, H5P_DEFAULT) < 0) {
                    fprintf(stderr, "h5stream: cannot copy %s\n", src);
                    goto step_done;
                }
            }
        }

        if (write_step_attr(ofid, (unsigned long long)i) < 0) {
            fprintf(stderr, "h5stream: cannot stamp revision %llu\n", (unsigned long long)(i + 1));
            goto step_done;
        }
        step_rc = 0;

step_done:
        H5Fclose(ofid);
        H5Pclose(rev_fapl);
        if (step_rc != 0)
            goto done;
        (*n_written)++;
    }
    rc = 0;

done:
    for (k = 0; k < sn.n; k++)
        free(sn.names[k]);
    free(sn.names);
    if (backing >= 0)
        H5Pclose(backing);
    return rc;
}

static int
cmd_history(const char *fname, const char *outname)
{
    hid_t    nfid = H5I_INVALID_HID, steps_grp = H5I_INVALID_HID, fapl = H5I_INVALID_HID;
    hsize_t  n_steps = 0;
    size_t   n_written = 0;
    char    *onion_name = NULL, *recovery_name = NULL;
    uint32_t pages[2] = {H5STREAM_ONION_PAGE_SIZE, H5STREAM_ONION_PAGE_SIZE_SAFE};
    uint32_t used     = 0;
    size_t   p;
    int      rc = 1;

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
        fprintf(stderr, "h5stream: '%s' has no steps\n", fname);
        goto done;
    }
    if (H5Gget_num_objs(steps_grp, &n_steps) < 0)
        goto done;

    {
        size_t len = strlen(outname) + 20;

        if (NULL == (onion_name = (char *)malloc(len)) || NULL == (recovery_name = (char *)malloc(len)))
            goto done;
        snprintf(onion_name, len, "%s.onion", outname);
        snprintf(recovery_name, len, "%s.onion.recovery", outname);
    }

    for (p = 0; p < sizeof(pages) / sizeof(pages[0]); p++) {
        if (history_write_pass(fname, nfid, n_steps, outname, onion_name, recovery_name, pages[p],
                               &n_written) < 0)
            goto done;
        if (history_verify(outname, n_steps, pages[p]) == 0) {
            used = pages[p];
            break;
        }
        if (p + 1 < sizeof(pages) / sizeof(pages[0]))
            fprintf(stderr,
                    "h5stream: the history did not read back correctly at page size %u -- retrying at %u "
                    "(see cmd_history()'s page-size note)\n",
                    pages[p], pages[p + 1]);
    }

    if (used == 0) {
        fprintf(stderr, "h5stream: could not write a verifiable revision history for '%s'\n", fname);
        goto done;
    }

    /* Report the count the VFD itself reports, not the one the loop believes
     * it wrote -- the two disagreeing is exactly the failure verified above. */
    {
        hid_t   backing   = H5Pcreate(H5P_FILE_ACCESS);
        hid_t   cfapl     = (backing >= 0) ? onion_fapl(backing, H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST,
                                                        used, NULL)
                                            : H5I_INVALID_HID;
        hsize_t revisions = 0;

        /* H5FDonion_get_revision_count() insists on an onion fapl, not the
         * plain backing one -- "not a Onion VFL driver" otherwise. */
        if (cfapl < 0 || H5FDonion_get_revision_count(outname, cfapl, &revisions) < 0)
            revisions = 0;

        printf("%s: wrote %zu revision(s) to %s (VFD reports %llu), verified\n", fname, n_written,
               outname, (unsigned long long)revisions);
        printf("  step N is revision N+1; each revision carries its own step in the root attribute "
               "\"vol_stream_step\"\n");
        printf("  open one with H5Pset_fapl_onion(): revision_num = step + 1, page_size = %u\n", used);
        printf("  the page size is also in '%s' itself, attribute \"%s\" -- read it plainly, without the "
               "onion VFD\n",
               outname, H5STREAM_ONION_PAGE_ATTR);

        if (cfapl >= 0)
            H5Pclose(cfapl);
        if (backing >= 0)
            H5Pclose(backing);
    }
    rc = 0;

done:
    free(onion_name);
    free(recovery_name);
    if (steps_grp >= 0)
        H5Gclose(steps_grp);
    if (nfid >= 0)
        H5Fclose(nfid);
    if (fapl >= 0)
        H5Pclose(fapl);
    return rc;
}

/* A short, readable name for a datatype: what someone reading a variable
 * list wants, rather than H5Tencode()'s truth. Falls back to the class name
 * plus a byte count for anything composite, which is enough to tell two
 * variables apart and honest about not being the whole story. */
static void
type_name(hid_t type_id, char *out, size_t out_len)
{
    H5T_class_t cls  = H5Tget_class(type_id);
    size_t      size = H5Tget_size(type_id);

    switch (cls) {
        case H5T_INTEGER: {
            H5T_sign_t sign = H5Tget_sign(type_id);

            snprintf(out, out_len, "%sint%zu", sign == H5T_SGN_NONE ? "u" : "", size * 8);
            break;
        }
        case H5T_FLOAT:
            snprintf(out, out_len, "float%zu", size * 8);
            break;
        case H5T_STRING:
            snprintf(out, out_len, "string");
            break;
        case H5T_COMPOUND:
            snprintf(out, out_len, "compound(%zuB)", size);
            break;
        case H5T_ENUM:
            snprintf(out, out_len, "enum(%zuB)", size);
            break;
        case H5T_ARRAY:
            snprintf(out, out_len, "array(%zuB)", size);
            break;
        case H5T_VLEN:
            snprintf(out, out_len, "vlen");
            break;
        default:
            snprintf(out, out_len, "%zuB", size);
            break;
    }
}

static void
shape_name(hid_t space_id, char *out, size_t out_len)
{
    int     rank = H5Sget_simple_extent_ndims(space_id);
    hsize_t dims[H5S_MAX_RANK];
    size_t  used = 0;
    int     d;

    if (rank <= 0 || H5Sget_simple_extent_dims(space_id, dims, NULL) < 0) {
        snprintf(out, out_len, "scalar");
        return;
    }
    out[0] = '\0';
    for (d = 0; d < rank && used < out_len; d++)
        used += (size_t)snprintf(out + used, out_len - used, "%s%llu", d ? "x" : "",
                                  (unsigned long long)dims[d]);
}

/* Attach to a live writer as a reader. Both `tail` and `schema` need exactly
 * this, including the diagnostic: "cannot open" is the single most likely
 * thing to happen to someone trying either, and the reason is almost never
 * the file. */
static int
attach_reader(const char *fname, const char *subcmd, hid_t *vol_id, hid_t *fapl, hid_t *fid)
{
    *vol_id = H5I_INVALID_HID;
    *fapl   = H5I_INVALID_HID;
    *fid    = H5I_INVALID_HID;

    if ((*vol_id = H5VL_stream_register()) < 0) {
        fprintf(stderr, "h5stream: cannot register the vol-stream connector\n");
        return -1;
    }
    if ((*fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(*fapl, *vol_id, NULL) < 0 ||
        H5Pset_file_locking(*fapl, false, true) < 0) {
        fprintf(stderr, "h5stream: cannot build a file-access property list\n");
        return -1;
    }

    H5E_BEGIN_TRY
    {
        *fid = H5Fopen(fname, H5F_ACC_RDONLY, *fapl);
    }
    H5E_END_TRY
    if (*fid < 0) {
        fprintf(stderr,
                "h5stream: cannot attach to '%s' as a reader.\n"
                "  %s needs a *live* writer, which requires the transport: set VOL_STREAM_NA\n"
                "  (e.g. na+sm or ofi+tcp) and make sure the writer is running and has published\n"
                "  its group file. For a stream that is already finished, use `h5stream list`.\n",
                fname, subcmd);
        return -1;
    }
    return 0;
}

/*
 * schema: ask a live writer what its stream carries.
 *
 * The answer `h5ls` cannot give. A writer holds the file open, so no
 * file-reading tool can enumerate a running stream -- h5ls reports
 * **NOT FOUND** against a file plainly on disk with content in it (measured;
 * see docs/dev-plan.md's tool-compatibility matrix). The variable list
 * therefore has to come over the transport, from the writer's own published
 * schema, which is exactly what H5Fget_stream_schema() asks for.
 */
static int
print_schema(hid_t fid, long timeout_ms)
{
    H5F_stream_var_t *vars  = NULL;
    size_t            n_vars = 0;
    uint64_t          step   = 0;
    size_t            i;

    if (H5Fget_stream_schema(fid, (uint64_t)(timeout_ms > 0 ? timeout_ms : 5000), &step, &n_vars, &vars) <
        0) {
        fprintf(stderr, "h5stream: the writer published no schema within the timeout.\n"
                         "  A writer publishes one when it commits its first step, so this usually\n"
                         "  means no step has committed yet -- retry, or raise --timeout-ms.\n");
        return -1;
    }

    printf("schema as of step %llu -- %zu object(s)\n", (unsigned long long)step, n_vars);
    for (i = 0; i < n_vars; i++) {
        char tname[32], shape[128];

        type_name(vars[i].type_id, tname, sizeof(tname));
        shape_name(vars[i].space_id, shape, sizeof(shape));
        printf("  %-9s %-28s %-12s %s\n", vars[i].is_attr ? "attribute" : "dataset", vars[i].path, tname,
               shape);
    }
    fflush(stdout);

    H5Ffree_stream_schema(n_vars, vars);
    return 0;
}

static int
cmd_schema(const char *fname, long timeout_ms)
{
    hid_t vol_id, fapl, fid;
    int   rc;

    if (attach_reader(fname, "schema", &vol_id, &fapl, &fid) < 0) {
        rc = 1;
        goto done;
    }
    rc = print_schema(fid, timeout_ms) < 0 ? 1 : 0;

done:
    if (fid >= 0)
        H5Fclose(fid);
    if (fapl >= 0)
        H5Pclose(fapl);
    if (vol_id >= 0)
        H5VLclose(vol_id);
    return rc;
}

/*
 * tail: follow a live stream, reporting each step as it commits.
 *
 * Uses the connector's own reader path -- H5Fwait_step_ready() -- and not
 * polling the file, because polling cannot work. A writer holds the file
 * open, and HDF5 does not permit another process to read it meanwhile:
 * plain `h5ls` on a stream mid-write reports "NOT FOUND" even though the
 * file exists on disk with content. That is the constraint dev-plan.md names
 * in its opening section ("h5dump assumes random access and will fail
 * unhelpfully"), and the transport exists precisely to answer it. An earlier
 * cut of this subcommand did poll, worked fine against a finished stream,
 * and reported nothing at all against a live one.
 *
 * The consequence is a real requirement rather than a preference: tail needs
 * the transport enabled (VOL_STREAM_NA set, and the writer publishing its
 * group), because that is the only channel by which a second process learns
 * a step was committed. Without it the reader open fails, and saying so
 * plainly is more use than silently following nothing.
 *
 * Terminates on --max-steps, or --timeout-ms with no new step; otherwise
 * follows indefinitely, which is what an interactive user wants and what a
 * test must not do.
 */
static int
cmd_tail(const char *fname, long max_steps, long interval_ms, long timeout_ms)
{
    hid_t    vol_id = H5I_INVALID_HID, fapl = H5I_INVALID_HID, fid = H5I_INVALID_HID;
    long     reported = 0;
    int      rc       = 1;

    (void)interval_ms; /* the wait is driven by the transport, not a poll */

    if (attach_reader(fname, "tail", &vol_id, &fapl, &fid) < 0)
        goto done;

    /* M10: what the stream contains, once, before following it. Printed at
     * attach rather than re-queried per step deliberately -- a schema is
     * pulled precisely because it changes rarely, and one metadata RPC per
     * step against a running simulation is the cost this design exists to
     * avoid. Best-effort: a writer that has not committed anything yet has
     * nothing to describe, and following it is still perfectly useful. */
    {
        long schema_wait = timeout_ms > 0 ? timeout_ms : 2000;

        H5E_BEGIN_TRY
        {
            (void)print_schema(fid, schema_wait);
        }
        H5E_END_TRY
    }

    for (;;) {
        uint64_t phys = 0, wall = 0;

        if (H5Fwait_step_ready(fid, (uint64_t)(timeout_ms > 0 ? timeout_ms : 1000), &phys, &wall) < 0) {
            if (timeout_ms > 0) {
                rc = 0; /* quiet exit: nothing new within the timeout */
                goto done;
            }
            continue; /* follow indefinitely */
        }

        printf("step %llu\n", (unsigned long long)phys);
        fflush(stdout);

        if (max_steps > 0 && ++reported == max_steps) {
            rc = 0;
            goto done;
        }
    }

done:
    if (fid >= 0)
        H5Fclose(fid);
    if (fapl >= 0)
        H5Pclose(fapl);
    if (vol_id >= 0)
        H5VLclose(vol_id);
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
    if (strcmp(argv[1], "tail") == 0) {
        long max_steps = 0, interval_ms = 200, timeout_ms = 0;
        int  a;

        for (a = 3; a < argc; a++) {
            if (strcmp(argv[a], "--max-steps") == 0 && a + 1 < argc)
                max_steps = strtol(argv[++a], NULL, 10);
            else if (strcmp(argv[a], "--interval-ms") == 0 && a + 1 < argc)
                interval_ms = strtol(argv[++a], NULL, 10);
            else if (strcmp(argv[a], "--timeout-ms") == 0 && a + 1 < argc)
                timeout_ms = strtol(argv[++a], NULL, 10);
            else {
                usage();
                return 2;
            }
        }
        if (interval_ms <= 0)
            interval_ms = 200;
        return cmd_tail(argv[2], max_steps, interval_ms, timeout_ms);
    }
    if (strcmp(argv[1], "schema") == 0) {
        long timeout_ms = 0;
        int  a2;

        for (a2 = 3; a2 < argc; a2++) {
            if (strcmp(argv[a2], "--timeout-ms") == 0 && a2 + 1 < argc)
                timeout_ms = strtol(argv[++a2], NULL, 10);
            else {
                usage();
                return 2;
            }
        }
        return cmd_schema(argv[2], timeout_ms);
    }
    if (strcmp(argv[1], "export") == 0) {
        if (argc < 4) {
            usage();
            return 2;
        }
        return cmd_export(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "history") == 0) {
        if (argc < 4) {
            usage();
            return 2;
        }
        return cmd_history(argv[2], argv[3]);
    }

    fprintf(stderr, "h5stream: unknown subcommand '%s'\n", argv[1]);
    usage();
    return 2;
}
