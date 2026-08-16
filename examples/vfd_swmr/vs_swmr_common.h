/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Shared setup for the vol-stream + VFD SWMR proof of concept.
 * See README.md in this directory for what the PoC demonstrates.
 */

#ifndef VS_SWMR_COMMON_H
#define VS_SWMR_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdf5.h"

#define VS_SWMR_FNAME  "vfd_swmr_poc.h5"
#define VS_SWMR_SHADOW "vfd_swmr_poc_shadow"
#define VS_SWMR_DSET   "/series"
#define VS_SWMR_NSTEPS 12
#define VS_SWMR_CHUNK  256

/* tick_len is in TENTHS of a second -- see H5Fvfd_swmr.c, which computes
 * tick_len * NANOSECS_PER_TENTH_SEC. So 4 -> 0.4s, and max_lag 7 -> 2.8s. */
#define VS_SWMR_TICK_LEN 4
#define VS_SWMR_MAX_LAG  7

/*-------------------------------------------------------------------------
 * VFD SWMR requires a paged allocation strategy on the FCPL, and the page
 * size must match the page buffer size set on the FAPL below. HDF5's own
 * VFD SWMR test configs set both to 4096 explicitly; so do we.
 *-------------------------------------------------------------------------
 */
static hid_t
vs_swmr_fcpl(void)
{
    hid_t fcpl = H5Pcreate(H5P_FILE_CREATE);

    if (fcpl < 0)
        return H5I_INVALID_HID;
    if (H5Pset_file_space_strategy(fcpl, H5F_FSPACE_STRATEGY_PAGE, false, 1) < 0 ||
        H5Pset_file_space_page_size(fcpl, 4096) < 0) {
        H5Pclose(fcpl);
        return H5I_INVALID_HID;
    }
    return fcpl;
}

/*-------------------------------------------------------------------------
 * VFD SWMR requires the latest file format and page buffering before its own
 * configuration is applied.
 *
 * enable_swmr == 0 is the CONTROL arm: plain HDF5, no VFD SWMR at all. That
 * arm is what makes the demonstration decisive rather than merely positive --
 * see README.md.
 *-------------------------------------------------------------------------
 */
static hid_t
vs_swmr_fapl(int writer, int enable_swmr)
{
    hid_t                 fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5F_vfd_swmr_config_t cfg;

    if (fapl < 0)
        return H5I_INVALID_HID;

    if (!enable_swmr) {
        /* Locking off so the control reader can at least attempt the open;
         * otherwise it fails for a reason unrelated to what is being shown. */
        H5Pset_file_locking(fapl, false, true);
        return fapl;
    }

    if (H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) < 0)
        goto fail;
    if (H5Pset_page_buffer_size(fapl, 4096, 100, 0) < 0)
        goto fail;

    memset(&cfg, 0, sizeof(cfg));
    cfg.version                = H5F__CURR_VFD_SWMR_CONFIG_VERSION;
    cfg.tick_len               = VS_SWMR_TICK_LEN;
    cfg.max_lag                = VS_SWMR_MAX_LAG;
    cfg.writer                 = writer ? true : false;
    cfg.maintain_metadata_file = true;
    cfg.md_pages_reserved      = 128;

    /* Set VS_SWMR_NO_FLUSH_RAW to clear this. A step's CONTENT is raw data,
     * so the expectation is that clearing it leaves a reader seeing structure
     * without contents -- but measured, that did not happen here, likely
     * because H5Fvfd_swmr_end_tick() pushes everything out anyway. Left as a
     * knob precisely so the next person can re-check rather than trust the
     * claim. See docs/user-guide.md's VFD SWMR section. */
    cfg.flush_raw_data = (getenv("VS_SWMR_NO_FLUSH_RAW") == NULL);

    /* md_file_path is a DIRECTORY and md_file_name is the shadow file's name
     * -- two separate fields, populated independently (see
     * init_vfd_swmr_config() in HDF5's own test/vfd_swmr_common.c). Passing a
     * full path here and leaving md_file_name empty makes H5Fcreate() fail,
     * and in a Debug HDF5 that surfaces as an assertion inside the library
     * rather than a clean error:
     *
     *   H5Fint.c: H5F__dest: Assertion `H5AC_cache_is_clean(...)' failed.
     *
     * That looks like a library bug and is not one. */
    strncpy(cfg.md_file_path, "./", sizeof(cfg.md_file_path) - 1);
    strncpy(cfg.md_file_name, VS_SWMR_SHADOW, sizeof(cfg.md_file_name) - 1);

    if (H5Pset_vfd_swmr_config(fapl, &cfg) < 0)
        goto fail;

    return fapl;

fail:
    H5Pclose(fapl);
    return H5I_INVALID_HID;
}

/* How many steps are visible right now. Opens /step fresh on each poll so no
 * object-refresh semantics are involved in the measurement. */
static long
vs_swmr_count_steps(hid_t fid)
{
    hid_t      grp;
    H5G_info_t info;
    long       n;

    H5E_BEGIN_TRY { grp = H5Gopen2(fid, "/step", H5P_DEFAULT); }
    H5E_END_TRY

    if (grp < 0)
        return -1; /* /step not visible yet */

    if (H5Gget_info(grp, &info) < 0) {
        H5Gclose(grp);
        return -1;
    }
    n = (long)info.nlinks;
    H5Gclose(grp);
    return n;
}

#endif /* VS_SWMR_COMMON_H */
