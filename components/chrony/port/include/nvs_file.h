/*
 * Small files for chronyd (driftfile) stored as NVS blobs.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHRONY_ESP_NVS_FILE_H
#define CHRONY_ESP_NVS_FILE_H

/* Mount point; a file "/chrony/<name>" is the NVS blob <name> (at most
   15 characters, no '/') in namespace "chronyfs". */
#define CES_FILE_ROOT "/chrony"

/* Register the filesystem. Requires nvs_flash_init(). Returns 1 on
   success. */
extern int CES_MountFiles(void);

#endif
