/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * M9 predicate pushdown: a subscriber declares a value test, and the writer
 * evaluates it against the bytes it is about to marshal, sending only the
 * elements that satisfy it.
 *
 * What makes this different from everything before it: M8's subscription
 * routing and M8.5's precision both still put *something* on the wire for
 * every write overlapping a subscription. A predicate can take a write to
 * zero bytes, and step 1 below is exactly that case.
 *
 * Two OS processes, na+sm, same shape as test/t_subscribe.c. The reader
 * subscribes to the whole of /temp -- no subrange bound at all -- so nothing
 * but the predicate can narrow what arrives, and the narrowing is provably
 * the writer's doing: H5Fget_subscribed_data() only dequeues what was
 * received, so 100 elements out of a 1000-element write cannot have been
 * filtered on this side.
 *
 *   step 0  one contiguous hot region: /temp[HOT0_START .. +HOT0_COUNT) is
 *           above the threshold, everything else below -> exactly one push,
 *           covering exactly that region.
 *   step 1  nothing above the threshold -> no push at all, the case only a
 *           predicate can produce.
 *   step 2  two disjoint hot regions -> two pushes, one per maximal
 *           contiguous run, each truthfully labelled with its own
 *           elem_start/elem_count (no wire-format change was needed for
 *           this; see vs_tr_writer_push_data()).
 *   step 3  every other element hot over a 200-element window, ~100 runs
 *           against a cap of 64 -> one coalesced span covering them all.
 *           A superset by design: the alternative at the cap is either 100
 *           tiny RPCs or a truncated answer, and a truncated answer is data
 *           loss.
 *
 * /blob, written in step 0, is a compound-typed dataset carrying the same
 * predicate. A value test does not apply to a compound, so the writer cannot
 * evaluate it and sends the whole object -- pinning the deliberate rule that
 * an unevaluable predicate over-sends rather than under-sends
 * (H5VL__stream_eval_predicate() declines; over-sending is inefficiency,
 * under-sending is data loss). A regression that made a decline drop data
 * instead would fail here rather than silently losing science.
 *
 * Wire bytes are summed and printed rather than asserted less-than, the same
 * standard test/t_precision.c is held to.
 *
 * Only compiled/run when VOL_STREAM_HAVE_MERCURY is on; see
 * test/CMakeLists.txt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define NTEMP     1000 /* ints in /temp -- 4000 bytes if pushed whole */
#define THRESHOLD 1000 /* the predicate: /temp element > THRESHOLD    */
#define NSTEPS    4

/* step 0: one contiguous run above the threshold */
#define HOT0_START 700
#define HOT0_COUNT 100

/* step 2: two disjoint runs -> two pushes */
#define HOT2A_START 100
#define HOT2A_COUNT 10
#define HOT2B_START 900
#define HOT2B_COUNT 5

/* step 3: every other element in [FRAG_START, FRAG_END) matches, which is
 * ~100 runs -- more than VS_TR_MAX_PRED_RUNS (64), so the writer coalesces
 * to the single span containing them all rather than firing 100 tiny RPCs
 * or, far worse, truncating at the cap. The span is a superset: it carries
 * the non-matching elements between matches too. */
#define FRAG_START 200
#define FRAG_END   400
/* First and last matching index, so the expected coalesced span is exact. */
#define FRAG_SPAN_START FRAG_START
#define FRAG_SPAN_COUNT (FRAG_END - 2 - FRAG_START + 1)

#define NBLOB 4 /* compound elements in /blob -- predicate cannot apply */

#define FNAME "t_predicate.h5"

#define READY_SENTINEL       "t_predicate.reader_ready"
#define WRITES_DONE_SENTINEL "t_predicate.writes_done"
#define READER_DONE_SENTINEL "t_predicate.reader_done"

typedef struct blob_t {
    int    a;
    double b;
} blob_t;

/* One received push, as H5Fget_subscribed_data() handed it over. */
typedef struct got_t {
    char    *path;
    uint64_t elem_start;
    uint64_t elem_count;
    size_t   size;
    void    *buf;
} got_t;

#define MAX_GOT 16

static int
wait_for_sentinel(const char *path, int max_iters)
{
    int i;

    for (i = 0; i < max_iters; i++) {
        FILE *f = fopen(path, "r");

        if (f) {
            fclose(f);
            return 0;
        }
        usleep(100000);
    }
    return -1;
}

static void
touch_sentinel(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f)
        fclose(f);
}

/* The value /temp[i] carries in step s -- the single definition both sides
 * use, so "what the writer wrote" and "what the reader expected" cannot
 * drift apart. Above THRESHOLD only inside that step's hot region(s). */
static int
temp_value(int s, int i)
{
    int hot = 0;

    if (s == 0)
        hot = (i >= HOT0_START && i < HOT0_START + HOT0_COUNT);
    else if (s == 2)
        hot = (i >= HOT2A_START && i < HOT2A_START + HOT2A_COUNT) ||
              (i >= HOT2B_START && i < HOT2B_START + HOT2B_COUNT);
    else if (s == 3)
        hot = (i >= FRAG_START && i < FRAG_END && (i % 2) == 0);

    /* Cold values stay below THRESHOLD by construction (i < NTEMP ==
     * THRESHOLD), so step 1 has nothing to match anywhere. */
    return hot ? THRESHOLD + 1000 + i : i;
}

static int
run_reader(void)
{
    hid_t    vol_id, fapl, fid, temp_space, blob_space;
    int      i, rc = 0;
    int      threshold = THRESHOLD;
    got_t    got[MAX_GOT];
    int      n_got      = 0;
    size_t   temp_bytes = 0;
    hsize_t  temp_dims = NTEMP, blob_dims = NBLOB;

    memset(got, 0, sizeof(got));

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("reader: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("reader: FAIL fapl\n");
        return 1;
    }
    /* Wait for the group sidecar rather than the file, as every other
     * transport test here does -- it is only published after an explicit
     * flush, so its existence guarantees an openable superblock. */
    for (i = 0; i < 100; i++) {
        FILE *f = fopen(FNAME ".vsgroup", "r");

        if (f) {
            fclose(f);
            break;
        }
        usleep(100000);
    }
    if ((fid = H5Fopen(FNAME, H5F_ACC_RDONLY, fapl)) < 0) {
        printf("reader: FAIL open/join\n");
        return 1;
    }

    /* Whole-object subscriptions deliberately: with no subrange bound, the
     * predicate is the only thing that can narrow what arrives. */
    if ((temp_space = H5Screate_simple(1, &temp_dims, NULL)) < 0 ||
        (blob_space = H5Screate_simple(1, &blob_dims, NULL)) < 0) {
        printf("reader: FAIL create subscription dataspaces\n");
        return 1;
    }
    {
        const char *paths[2]  = {"/temp", "/blob"};
        const hid_t spaces[2] = {temp_space, blob_space};

        if (H5Fsubscribe(fid, 2, paths, spaces, NULL) < 0) {
            printf("reader: FAIL subscribe\n");
            return 1;
        }
    }
    H5Sclose(temp_space);
    H5Sclose(blob_space);

    /* The milestone's actual call. Both paths get the same test; /temp can
     * be evaluated, /blob cannot. */
    if (H5Fsubscribe_predicate(fid, "/temp", H5VL_STREAM_PRED_GT, H5T_NATIVE_INT, &threshold) < 0) {
        printf("  FAIL  H5Fsubscribe_predicate on /temp rejected\n");
        rc = 1;
    }
    if (H5Fsubscribe_predicate(fid, "/blob", H5VL_STREAM_PRED_GT, H5T_NATIVE_INT, &threshold) < 0) {
        printf("  FAIL  H5Fsubscribe_predicate on /blob rejected\n");
        rc = 1;
    }
    /* A predicate narrows an existing subscription, so one for a path never
     * subscribed to must fail rather than be silently dropped -- otherwise a
     * typo'd path would look like "the predicate matched nothing, ever". */
    if (H5Fsubscribe_predicate(fid, "/never-subscribed", H5VL_STREAM_PRED_GT, H5T_NATIVE_INT,
                               &threshold) == 0) {
        printf("  FAIL  predicate on an unsubscribed path was accepted\n");
        rc = 1;
    }
    else
        printf("  ok    predicate on an unsubscribed path is refused, not silently dropped\n");

    touch_sentinel(READY_SENTINEL);

    /* Wait until the writer is completely done before draining. Every push
     * is forwarded synchronously inside the writer's end_step(), so once
     * WRITES_DONE exists, everything that will ever arrive has arrived --
     * which is what lets step 1's "no push at all" be a real assertion
     * rather than a "not yet". */
    if (wait_for_sentinel(WRITES_DONE_SENTINEL, 200) < 0) {
        printf("reader: FAIL writer never signalled writes done\n");
        rc = 1;
    }

    while (n_got < MAX_GOT) {
        uint64_t phys = 0, es = 0, ec = 0;
        char    *path = NULL;
        void    *buf  = NULL;
        size_t   size = 0;

        if (H5Fget_subscribed_data(fid, 500, &phys, &path, &buf, &size, &es, &ec) < 0)
            break;

        got[n_got].path        = path;
        got[n_got].elem_start = es;
        got[n_got].elem_count = ec;
        got[n_got].size        = size;
        got[n_got].buf         = buf;
        n_got++;
    }

    /* Expected: /temp thrice (step 0's single run, step 2's two runs) and
     * /blob once. Step 1 contributes nothing -- the point of the milestone. */
    {
        int n_temp = 0, n_blob = 0;

        for (i = 0; i < n_got; i++) {
            if (got[i].path && strcmp(got[i].path, "/temp") == 0)
                n_temp++;
            else if (got[i].path && strcmp(got[i].path, "/blob") == 0)
                n_blob++;
        }
        if (n_temp != 4) {
            printf("  FAIL  got %d /temp pushes, expected 4 (step 0: 1 run, step 1: none, step 2: 2 runs, "
                   "step 3: 1 coalesced)\n",
                   n_temp);
            rc = 1;
        }
        if (n_blob != 1) {
            printf("  FAIL  got %d /blob pushes, expected 1\n", n_blob);
            rc = 1;
        }
    }

    /* Every /temp element that arrived must be one the writer actually
     * wrote above the threshold, at the flat index it claims. Checking the
     * count as well as the values: a routing bug that delivers correct
     * values for a fraction of the data passes a value-only check (the
     * lesson test/t_subvolume_nd.c was written for). */
    {
        int matched_total  = 0;
        int expected_total = HOT0_COUNT + HOT2A_COUNT + HOT2B_COUNT + FRAG_SPAN_COUNT;
        int saw_step0 = 0, saw_step2a = 0, saw_step2b = 0, saw_step3 = 0;

        for (i = 0; i < n_got; i++) {
            const int *vals;
            uint64_t   k;
            int        s;
            int        coalesced = 0;

            if (!got[i].path || strcmp(got[i].path, "/temp") != 0)
                continue;

            temp_bytes += got[i].size;
            matched_total += (int)got[i].elem_count;

            if (got[i].size != got[i].elem_count * sizeof(int)) {
                printf("  FAIL  /temp push claims %llu elements but carries %zu bytes\n",
                       (unsigned long long)got[i].elem_count, got[i].size);
                rc = 1;
                continue;
            }

            /* Which step's pattern this run belongs to is determined by
             * where it starts -- the three hot regions are disjoint. */
            if (got[i].elem_start == HOT0_START && got[i].elem_count == HOT0_COUNT) {
                s = 0;
                saw_step0 = 1;
            }
            else if (got[i].elem_start == HOT2A_START && got[i].elem_count == HOT2A_COUNT) {
                s = 2;
                saw_step2a = 1;
            }
            else if (got[i].elem_start == HOT2B_START && got[i].elem_count == HOT2B_COUNT) {
                s = 2;
                saw_step2b = 1;
            }
            else if (got[i].elem_start == FRAG_SPAN_START && got[i].elem_count == FRAG_SPAN_COUNT) {
                s          = 3;
                coalesced  = 1;
                saw_step3  = 1;
            }
            else {
                printf("  FAIL  unexpected /temp run [%llu, %llu)\n", (unsigned long long)got[i].elem_start,
                       (unsigned long long)(got[i].elem_start + got[i].elem_count));
                rc = 1;
                continue;
            }

            vals = (const int *)got[i].buf;
            for (k = 0; k < got[i].elem_count; k++) {
                int idx      = (int)(got[i].elem_start + k);
                int expected = temp_value(s, idx);

                if (vals[k] != expected) {
                    printf("  FAIL  /temp[%d] = %d, expected %d\n", idx, vals[k], expected);
                    rc = 1;
                    break;
                }
                /* A coalesced span is deliberately a superset, so only an
                 * exactly-described run may be held to "every element
                 * matches". */
                if (!coalesced && vals[k] <= THRESHOLD) {
                    printf("  FAIL  /temp[%d] = %d does not satisfy > %d, but was pushed\n", idx, vals[k],
                           THRESHOLD);
                    rc = 1;
                    break;
                }
            }
        }

        if (!saw_step0) {
            printf("  FAIL  step 0's single hot run [%d, %d) never arrived\n", HOT0_START,
                   HOT0_START + HOT0_COUNT);
            rc = 1;
        }
        if (!saw_step2a || !saw_step2b) {
            printf("  FAIL  step 2's two disjoint runs did not both arrive (a=%d b=%d)\n", saw_step2a,
                   saw_step2b);
            rc = 1;
        }
        if (!saw_step3) {
            printf("  FAIL  step 3's ~%d matching runs were not coalesced to [%d, %d) -- truncation at the "
                   "run cap would be data loss\n",
                   (FRAG_END - FRAG_START) / 2, FRAG_SPAN_START, FRAG_SPAN_START + FRAG_SPAN_COUNT);
            rc = 1;
        }
        else
            printf("  ok    %d matching runs exceeded the run cap and coalesced to one %d-element span, a "
                   "superset\n",
                   (FRAG_END - FRAG_START) / 2, FRAG_SPAN_COUNT);
        if (matched_total != expected_total) {
            printf("  FAIL  received %d /temp elements, expected %d\n", matched_total, expected_total);
            rc = 1;
        }
        else
            printf("  ok    %d of %d elements pushed across %d steps -- step 1 matched nothing and sent "
                   "nothing\n",
                   matched_total, NSTEPS * NTEMP, NSTEPS);
    }

    /* The measured number this milestone is worth. */
    {
        size_t whole = (size_t)NSTEPS * NTEMP * sizeof(int);

        printf("  ok    /temp wire bytes: %zu of %zu (%.1f%%) -- predicate evaluated writer-side\n",
               temp_bytes, whole, 100.0 * (double)temp_bytes / (double)whole);
    }

    /* The decline path: a predicate that cannot apply to compound data must
     * yield the whole object, not an empty push and not a dropped one. */
    for (i = 0; i < n_got; i++)
        if (got[i].path && strcmp(got[i].path, "/blob") == 0) {
            if (got[i].elem_start != 0 || got[i].elem_count != NBLOB) {
                printf("  FAIL  /blob push is [%llu, %llu), expected the whole [0, %d)\n",
                       (unsigned long long)got[i].elem_start,
                       (unsigned long long)(got[i].elem_start + got[i].elem_count), NBLOB);
                rc = 1;
            }
            else
                printf("  ok    predicate on compound data declines to the whole object (%zu bytes), "
                       "over-sending rather than under-sending\n",
                       got[i].size);
        }

    for (i = 0; i < n_got; i++) {
        free(got[i].path);
        free(got[i].buf);
    }

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);

    /* Leave before the writer tears the SSG group down -- see
     * test/t_subscribe.c's own note on this shutdown ordering. */
    touch_sentinel(READER_DONE_SENTINEL);
    return rc;
}

static int
run_writer(void)
{
    hid_t   vol_id, fapl, fid, temp_space, blob_type, blob_space, blob_ds;
    hid_t   temp_ds = H5I_INVALID_HID;
    hsize_t temp_dims = NTEMP, blob_dims = NBLOB;
    int    *temp_vals;
    blob_t  blob_vals[NBLOB];
    int     s, i;

    if ((vol_id = H5VL_stream_register()) < 0) {
        printf("writer: FAIL register\n");
        return 1;
    }
    if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) < 0 || H5Pset_vol(fapl, vol_id, NULL) < 0 ||
        H5Pset_file_locking(fapl, false, true) < 0) {
        printf("writer: FAIL fapl\n");
        return 1;
    }
    if ((fid = H5Fcreate(FNAME, H5F_ACC_TRUNC, H5P_DEFAULT, fapl)) < 0) {
        printf("writer: FAIL create (transport up? VOL_STREAM_NA set?)\n");
        return 1;
    }

    if (wait_for_sentinel(READY_SENTINEL, 200) < 0) {
        printf("writer: FAIL reader never subscribed\n");
        return 1;
    }

    if (NULL == (temp_vals = (int *)malloc(NTEMP * sizeof(int)))) {
        printf("writer: FAIL alloc\n");
        return 1;
    }
    for (i = 0; i < NBLOB; i++) {
        blob_vals[i].a = i;
        blob_vals[i].b = (double)i / 4.0;
    }

    if ((temp_space = H5Screate_simple(1, &temp_dims, NULL)) < 0 ||
        (blob_space = H5Screate_simple(1, &blob_dims, NULL)) < 0) {
        printf("writer: FAIL create dataspaces\n");
        return 1;
    }
    if ((blob_type = H5Tcreate(H5T_COMPOUND, sizeof(blob_t))) < 0 ||
        H5Tinsert(blob_type, "a", HOFFSET(blob_t, a), H5T_NATIVE_INT) < 0 ||
        H5Tinsert(blob_type, "b", HOFFSET(blob_t, b), H5T_NATIVE_DOUBLE) < 0) {
        printf("writer: FAIL build compound type\n");
        return 1;
    }

    for (s = 0; s < NSTEPS; s++) {
        if (H5Fbegin_step(fid, 0, NULL, 0) < 0) {
            printf("writer: FAIL begin_step %d\n", s);
            return 1;
        }

        for (i = 0; i < NTEMP; i++)
            temp_vals[i] = temp_value(s, i);

        /* Created once and rewritten each step through the same handle --
         * how a simulation actually writes a variable per iteration, and
         * (until the capture fix this test forced) a silent no-op after
         * step 0: the write bypassed capture, so no manifest entry, no
         * push, and the earlier step's copy overwritten. test/t_step_
         * rewrite.c pins that behavior directly and without a transport;
         * here it is load-bearing because steps 1 and 2 are what make "no
         * push at all" and "two runs" observable. */
        if (s == 0 && (temp_ds = H5Dcreate2(fid, "/temp", H5T_NATIVE_INT, temp_space, H5P_DEFAULT,
                                             H5P_DEFAULT, H5P_DEFAULT)) < 0) {
            printf("writer: FAIL create /temp\n");
            return 1;
        }
        if (H5Dwrite(temp_ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, temp_vals) < 0) {
            printf("writer: FAIL write /temp step %d\n", s);
            return 1;
        }

        /* /blob only in step 0 -- one push is enough to pin the decline. */
        if (s == 0) {
            if ((blob_ds = H5Dcreate2(fid, "/blob", blob_type, blob_space, H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT)) < 0 ||
                H5Dwrite(blob_ds, blob_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, blob_vals) < 0) {
                printf("writer: FAIL write /blob\n");
                return 1;
            }
            H5Dclose(blob_ds);
        }

        if (H5Fend_step(fid) < 0) {
            printf("writer: FAIL end_step %d\n", s);
            return 1;
        }
    }

    touch_sentinel(WRITES_DONE_SENTINEL);

    H5Dclose(temp_ds);
    H5Tclose(blob_type);
    H5Sclose(temp_space);
    H5Sclose(blob_space);
    free(temp_vals);

    /* Outlive the reader so its departure, not the group's destruction,
     * comes first. */
    wait_for_sentinel(READER_DONE_SENTINEL, 200);

    H5Fclose(fid);
    H5Pclose(fapl);
    H5VLclose(vol_id);
    return 0;
}

int
main(void)
{
    pid_t pid;
    int   reader_status = 0, writer_status;
    int   nerrors        = 0;

    printf("vol-stream M9: predicate pushdown (na+sm)\n");

    /* A default, not a requirement -- an externally-set VOL_STREAM_NA wins,
     * so the CI matrix can run this over another NA plugin. */
    setenv("VOL_STREAM_NA", "na+sm", 0);
    unlink(READY_SENTINEL);
    unlink(WRITES_DONE_SENTINEL);
    unlink(READER_DONE_SENTINEL);

    /* Cleared before the fork, not inside run_writer(): a reader polls for
     * this sidecar to know the writer is up, so one left behind by a previous
     * run in the same directory sends it to join a group that died with that
     * run -- and then to open a file the current writer is about to unlink
     * ("can't retrieve stat info for file"). Same fix, and the same reason,
     * as t_precision_dual.c's own pre-fork cleanup. */
    unlink(FNAME ".vsgroup");
    unlink(FNAME);

    fflush(NULL);
    if ((pid = fork()) < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        int rc = run_reader();

        fflush(NULL);
        _exit(rc);
    }

    writer_status = run_writer();

    if (waitpid(pid, &reader_status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    unlink(READY_SENTINEL);
    unlink(WRITES_DONE_SENTINEL);
    unlink(READER_DONE_SENTINEL);

    if (writer_status != 0) {
        printf("\nwriter process reported failure\n");
        nerrors++;
    }
    if (!(WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 0)) {
        printf("\nreader process reported failure (status=%d)\n", reader_status);
        nerrors++;
    }

    if (nerrors) {
        printf("\n%d failure(s)\n", nerrors);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
