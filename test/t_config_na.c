/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The transport turned on per file, through the FAPL rather than
 * VOL_STREAM_NA: with the variable unset, a file whose H5Pset_fapl_stream()
 * config names an NA string starts the transport (its <file>.vsgroup sidecar
 * appears), and a file created beside it with the defaults does not.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

int
main(void)
{
    H5VL_stream_config_t cfg;
    hid_t                fapl_na, fapl_def, f1, f2;
    int                  on, off, rc = 0;

    printf("vol-stream: the transport turned on through the FAPL (na+sm)\n");
    unsetenv("VOL_STREAM_NA");
    unlink("t_config_na_on.h5.vsgroup");
    unlink("t_config_na_off.h5.vsgroup");

    H5VL_stream_config_init(&cfg);
    cfg.na         = "na+sm";
    cfg.bulk_threshold = 0; /* accepted per file; exercised by the bulk tests */
    fapl_na        = H5Pcreate(H5P_FILE_ACCESS);
    fapl_def       = H5Pcreate(H5P_FILE_ACCESS);
    if (H5Pset_fapl_stream(fapl_na, &cfg) < 0 || H5Pset_fapl_stream(fapl_def, NULL) < 0 ||
        (f1 = H5Fcreate("t_config_na_on.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_na)) < 0 ||
        (f2 = H5Fcreate("t_config_na_off.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_def)) < 0) {
        printf("  FAIL  setup\n");
        return 1;
    }
    on  = access("t_config_na_on.h5.vsgroup", F_OK) == 0;
    off = access("t_config_na_off.h5.vsgroup", F_OK) == 0;
    if (on)
        printf("  ok    na=na+sm on the FAPL started the transport (group sidecar published)\n");
    else {
        printf("  FAIL  na=na+sm on the FAPL did not start the transport\n");
        rc = 1;
    }
    if (!off)
        printf("  ok    a default FAPL in the same process did not\n");
    else {
        printf("  FAIL  a default FAPL started the transport too\n");
        rc = 1;
    }

    H5Fclose(f1);
    H5Fclose(f2);
    H5Pclose(fapl_na);
    H5Pclose(fapl_def);
    unlink("t_config_na_on.h5");
    unlink("t_config_na_off.h5");
    unlink("t_config_na_on.h5.vsgroup");
    unlink("t_config_na_off.h5.vsgroup");
    printf(rc ? "\nfailure\n" : "\nall checks passed\n");
    return rc;
}
