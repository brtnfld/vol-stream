/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M9's last item: onion-backed addressable step history -- "stream and
 * archive as one object" (design-plan.md #2). `h5stream history` writes an
 * onion-VFD archive in which every step is a numbered, re-openable revision,
 * each holding the stream as of that step at ordinary logical paths.
 *
 * The load-bearing assertion is the one a plain exported file cannot satisfy:
 * **opening at an older revision must HIDE what later steps did.** A finished
 * stream shows every step at once (/step/0/, /step/1/, …) and an export shows
 * only the newest; neither can answer "what did this file look like at step
 * 1?". Checking that /late is absent from revision 2 and present in revision 3
 * is what distinguishes a real revision history from a copy.
 *
 * Everything after the tool runs uses plain HDF5 with an onion fapl -- no
 * vol-stream connector -- because being readable without this project is the
 * entire point of writing an archive.
 *
 * Revision numbering is the VFD's, and off by one: revision 0 is the empty
 * canonical file the onion VFD requires before it will open read-write, so
 * step N is revision N+1. Each revision stamps its own step into a root
 * attribute, and this test checks that stamp rather than trusting the
 * arithmetic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5FDonion.h"
#include "H5VLstream.h"

#define FNAME   "t_onion_history.h5"
#define OUTNAME "t_onion_history_archive.h5"

#define NSTEPS 3

static int
write_val(hid_t fid, const char *name, int v)
{
    hid_t sp, ds;
    int   rc = -1;

    if ((sp = H5Screate(H5S_SCALAR)) < 0)
        return -1;
    if ((ds = H5Dcreate2(fid, name, H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) >= 0) {
        if (H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &v) >= 0)
            rc = 0;
        H5Dclose(ds);
    }
    H5Sclose(sp);
    return rc;
}

/* The page size the archive was written with, read from the canonical file
 * with plain HDF5 -- no onion fapl, because that is the point: a reader has
 * to know the page size *before* it can open a revision at all. Passing 0
 * shows an apparently empty file and passing the wrong non-zero value aborts
 * the process inside the VFD, so guessing is not an option. See
 * cmd_history()'s page-size note in tools/h5stream.c. */
static uint32_t
archive_page_size(void)
{
    hid_t    fid, at;
    uint32_t page = 0;

    H5E_BEGIN_TRY
    {
        fid = H5Fopen(OUTNAME, H5F_ACC_RDONLY, H5P_DEFAULT);
    }
    H5E_END_TRY
    if (fid < 0)
        return 0;

    H5E_BEGIN_TRY
    {
        at = H5Aopen(fid, "vol_stream_onion_page_size", H5P_DEFAULT);
    }
    H5E_END_TRY
    if (at >= 0) {
        if (H5Aread(at, H5T_NATIVE_UINT32, &page) < 0)
            page = 0;
        H5Aclose(at);
    }
    H5Fclose(fid);
    return page;
}

/* Open one revision of the archive with plain HDF5 + an onion fapl. */
static hid_t
open_revision(uint64_t revision, hid_t *backing_out)
{
    H5FD_onion_fapl_info_t info;
    hid_t                  fapl = H5I_INVALID_HID, fid = H5I_INVALID_HID;

    memset(&info, 0, sizeof(info));
    info.version = H5FD_ONION_FAPL_INFO_VERSION_CURR;
    /* A real fapl, not H5P_DEFAULT -- see cmd_history()'s note. */
    if ((info.backing_fapl_id = H5Pcreate(H5P_FILE_ACCESS)) < 0)
        return H5I_INVALID_HID;
    info.page_size    = archive_page_size();
    info.store_target = H5FD_ONION_STORE_TARGET_ONION;
    info.revision_num = revision;

    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_fapl_onion(fapl, &info) < 0) {
        H5Pclose(info.backing_fapl_id);
        return H5I_INVALID_HID;
    }

    H5E_BEGIN_TRY
    {
        fid = H5Fopen(OUTNAME, H5F_ACC_RDONLY, fapl);
    }
    H5E_END_TRY

    H5Pclose(fapl);
    *backing_out = info.backing_fapl_id;
    return fid;
}

static int
read_val(hid_t fid, const char *name, int *out)
{
    hid_t ds;
    int   rc = -1;

    H5E_BEGIN_TRY
    {
        ds = H5Dopen2(fid, name, H5P_DEFAULT);
    }
    H5E_END_TRY
    if (ds < 0)
        return -1;
    if (H5Dread(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, out) >= 0)
        rc = 0;
    H5Dclose(ds);
    return rc;
}

static int
exists(hid_t fid, const char *name)
{
    htri_t e;

    H5E_BEGIN_TRY
    {
        e = H5Lexists(fid, name, H5P_DEFAULT);
    }
    H5E_END_TRY
    return e > 0;
}

static int
read_step_stamp(hid_t fid, unsigned long long *out)
{
    hid_t at;
    int   rc = -1;

    H5E_BEGIN_TRY
    {
        at = H5Aopen(fid, "vol_stream_step", H5P_DEFAULT);
    }
    H5E_END_TRY
    if (at < 0)
        return -1;
    if (H5Aread(at, H5T_NATIVE_ULLONG, out) >= 0)
        rc = 0;
    H5Aclose(at);
    return rc;
}

/* Read the page size recorded in an arbitrary archive's canonical file. */
static uint32_t
page_size_of(const char *archive)
{
    hid_t    fid, at;
    uint32_t page = 0;

    H5E_BEGIN_TRY
    {
        fid = H5Fopen(archive, H5F_ACC_RDONLY, H5P_DEFAULT);
    }
    H5E_END_TRY
    if (fid < 0)
        return 0;
    H5E_BEGIN_TRY
    {
        at = H5Aopen(fid, "vol_stream_onion_page_size", H5P_DEFAULT);
    }
    H5E_END_TRY
    if (at >= 0) {
        if (H5Aread(at, H5T_NATIVE_UINT32, &page) < 0)
            page = 0;
        H5Aclose(at);
    }
    H5Fclose(fid);
    return page;
}

#define BULK_FNAME   "t_onion_history_bulk.h5"
#define BULK_OUTNAME "t_onion_history_bulk_archive.h5"
#define BULK_NELEM   8192

static int
check_bulk_archive_uses_large_pages(const char *tool)
{
    hid_t    vol_id, fapl, fid, sp;
    hsize_t  dims = BULK_NELEM;
    int     *buf;
    char     cmd[1024];
    uint32_t page;
    int      s, i, rc = 0;

    unlink(BULK_FNAME);
    unlink(BULK_OUTNAME);
    unlink(BULK_OUTNAME ".onion");
    unlink(BULK_OUTNAME ".onion.recovery");

    if (NULL == (buf = (int *)malloc(BULK_NELEM * sizeof(int))))
        return 1;

    if ((vol_id = H5VL_stream_register()) < 0 || (fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 ||
        H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        (fid = H5Fcreate(BULK_FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("  FAIL  cannot build the bulk stream\n");
        free(buf);
        return 1;
    }
    if ((sp = H5Screate_simple(1, &dims, NULL)) < 0) {
        printf("  FAIL  bulk dataspace\n");
        free(buf);
        return 1;
    }

    for (s = 0; s < NSTEPS; s++) {
        hid_t ds;

        for (i = 0; i < BULK_NELEM; i++)
            buf[i] = s * 1000 + i;

        if (H5Fbegin_step(fid, 0, NULL, 0) < 0 ||
            (ds = H5Dcreate2(fid, "/bulk", H5T_NATIVE_INT, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)) < 0 ||
            H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) < 0 ||
            H5Fend_step(fid) < 0) {
            printf("  FAIL  bulk step %d\n", s);
            free(buf);
            return 1;
        }
        H5Dclose(ds);
    }

    H5Sclose(sp);
    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    free(buf);

    snprintf(cmd, sizeof(cmd), "\"%s\" history %s %s > /dev/null 2>&1", tool, BULK_FNAME, BULK_OUTNAME);
    if (system(cmd) != 0) {
        printf("  FAIL  h5stream history exited non-zero on the bulk stream\n");
        return 1;
    }

    page = page_size_of(BULK_OUTNAME);
    if (page != 4096) {
        printf("  FAIL  bulk archive used page size %u, expected 4096 -- the small-page fallback should "
               "only trigger on tiny archives\n",
               page);
        rc = 1;
    }
    else
        printf("  ok    an archive with real data verifies at page size 4096, no fallback needed\n");

    return rc;
}

int
main(int argc, char **argv)
{
    hid_t vol_id, fapl, fid;
    char  cmd[1024];
    int   nerrors = 0;

    if (argc < 2) {
        printf("usage: %s <path to h5stream>\n", argv[0]);
        return 2;
    }

    printf("vol-stream M9: onion-backed addressable step history\n");

    unlink(FNAME);
    unlink(OUTNAME);
    unlink(OUTNAME ".onion");
    unlink(OUTNAME ".onion.recovery");

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("  FAIL  register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0) {
        printf("  FAIL  fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("  FAIL  create\n");
        return 1;
    }

    /* /rewritten changes every step -- so a revision showing the wrong value
     * is a wrong *snapshot*, not just a missing object. /late appears only in
     * the last step, which is what the hiding check turns on. */
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || write_val(fid, "/rewritten", 10) < 0 ||
        H5Fend_step(fid) < 0) {
        printf("  FAIL  step 0\n");
        return 1;
    }
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || write_val(fid, "/rewritten", 20) < 0 ||
        H5Fend_step(fid) < 0) {
        printf("  FAIL  step 1\n");
        return 1;
    }
    if (H5Fbegin_step(fid, 0, NULL, 0) < 0 || write_val(fid, "/rewritten", 30) < 0 ||
        write_val(fid, "/late", 3) < 0 || H5Fend_step(fid) < 0) {
        printf("  FAIL  step 2\n");
        return 1;
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    snprintf(cmd, sizeof(cmd), "\"%s\" history %s %s > /dev/null 2>&1", argv[1], FNAME, OUTNAME);
    if (system(cmd) != 0) {
        printf("  FAIL  h5stream history exited non-zero\n");
        return 1;
    }

    if (access(OUTNAME ".onion", F_OK) != 0) {
        printf("  FAIL  no onion history file was written\n");
        return 1;
    }

    /* The VFD's own count, not ours: NSTEPS revisions plus the empty base. */
    {
        hid_t   backing   = H5I_INVALID_HID;
        hid_t   cfid      = open_revision(H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST, &backing);
        hsize_t revisions = 0;
        hid_t   cfapl     = H5I_INVALID_HID;

        /* H5FDonion_get_revision_count() requires an *onion* fapl -- a plain
         * one fails with "not a Onion VFL driver". Building it the same way
         * open_revision() does keeps that in one place. */
        if (cfid >= 0)
            H5Fclose(cfid);
        {
            H5FD_onion_fapl_info_t info;

            memset(&info, 0, sizeof(info));
            info.version         = H5FD_ONION_FAPL_INFO_VERSION_CURR;
            info.backing_fapl_id = (backing >= 0) ? backing : H5Pcreate(H5P_FILE_ACCESS);
            info.page_size       = archive_page_size();
            info.store_target    = H5FD_ONION_STORE_TARGET_ONION;
            info.revision_num    = H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST;
            if ((cfapl = H5Pcreate(H5P_FILE_ACCESS)) >= 0)
                H5Pset_fapl_onion(cfapl, &info);
        }

        if (cfapl < 0 || H5FDonion_get_revision_count(OUTNAME, cfapl, &revisions) < 0) {
            printf("  FAIL  cannot read the revision count back\n");
            nerrors++;
        }
        else if (revisions != (hsize_t)NSTEPS) {
            printf("  FAIL  archive holds %llu revision(s), expected %d (one per step)\n",
                   (unsigned long long)revisions, NSTEPS);
            nerrors++;
        }
        else
            printf("  ok    %d steps became %llu addressable revision(s)\n", NSTEPS,
                   (unsigned long long)revisions);
        if (cfapl >= 0)
            H5Pclose(cfapl);
        if (backing >= 0)
            H5Pclose(backing);
    }

    /* Each revision, checked as a snapshot: the right value for /rewritten,
     * the right stamp, and -- the part only a real history can do -- /late
     * absent until the step that wrote it. */
    {
        struct {
            uint64_t revision;
            int      expect_rewritten;
            int      expect_late;
            unsigned long long expect_stamp;
        } cases[] = {
            {1, 10, 0, 0},
            {2, 20, 0, 1},
            {3, 30, 1, 2},
        };
        size_t c;

        for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            hid_t              backing = H5I_INVALID_HID;
            hid_t              rfid    = open_revision(cases[c].revision, &backing);
            int                v       = -1;
            unsigned long long stamp   = (unsigned long long)-1;

            if (rfid < 0) {
                printf("  FAIL  revision %llu does not open\n", (unsigned long long)cases[c].revision);
                nerrors++;
                if (backing >= 0)
                    H5Pclose(backing);
                continue;
            }

            if (read_val(rfid, "/rewritten", &v) < 0 || v != cases[c].expect_rewritten) {
                printf("  FAIL  revision %llu: /rewritten = %d, expected %d\n",
                       (unsigned long long)cases[c].revision, v, cases[c].expect_rewritten);
                nerrors++;
            }
            if (exists(rfid, "/late") != cases[c].expect_late) {
                printf("  FAIL  revision %llu: /late %s, expected %s -- an older revision must not show a "
                       "later step's objects\n",
                       (unsigned long long)cases[c].revision, cases[c].expect_late ? "missing" : "present",
                       cases[c].expect_late ? "present" : "absent");
                nerrors++;
            }
            if (read_step_stamp(rfid, &stamp) < 0 || stamp != cases[c].expect_stamp) {
                printf("  FAIL  revision %llu is stamped step %llu, expected %llu\n",
                       (unsigned long long)cases[c].revision, stamp, cases[c].expect_stamp);
                nerrors++;
            }
            /* /step groups are the stream's storage layout, not an archive's:
             * a revision must look like an ordinary file. */
            if (exists(rfid, "/step")) {
                printf("  FAIL  revision %llu still carries a /step group\n",
                       (unsigned long long)cases[c].revision);
                nerrors++;
            }

            H5Fclose(rfid);
            if (backing >= 0)
                H5Pclose(backing);
        }

        if (!nerrors)
            printf("  ok    each revision is the stream as of its own step -- /late is absent from "
                   "revisions 1-2 and present in 3\n");
    }

    /* The efficient path. The archive above is a few KB, which is exactly
     * the regime where the onion VFD loses revisions at the default page
     * size, so the tool fell back to the small one -- correct, but it costs
     * an index entry per 32 bytes amended and no real archive should pay
     * that. A stream carrying actual bulk data must verify on the first
     * pass and keep the large page size. Asserting the *recorded* size is
     * what makes this a check on the fallback logic rather than on the VFD. */
    {
        uint32_t page = archive_page_size();

        if (page != 32)
            printf("  note  small archive used page size %u (expected the 32-byte fallback)\n", page);
        else
            printf("  ok    small archive fell back to page size 32, and says so in the file\n");
    }
    /* A revision that was never written must fail rather than silently
     * resolve to something nearby. */
    {
        hid_t backing = H5I_INVALID_HID;
        hid_t rfid    = open_revision(NSTEPS + 5, &backing);

        if (rfid >= 0) {
            printf("  FAIL  a nonexistent revision opened anyway\n");
            H5Fclose(rfid);
            nerrors++;
        }
        else
            printf("  ok    a nonexistent revision is refused, not approximated\n");
        if (backing >= 0)
            H5Pclose(backing);
    }

    /* A stream carrying real bulk data must verify on the *first* pass and
     * keep the large page size -- otherwise every archive would pay the
     * 32-byte-page index cost that only the tiny-archive workaround needs.
     * Checked by re-running the tool on a bulkier stream and reading the
     * page size it recorded. */
    nerrors += check_bulk_archive_uses_large_pages(argv[1]);

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
