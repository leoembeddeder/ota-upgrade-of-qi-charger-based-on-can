/**
 **************************************************************************
 * @file     boot_trial.h
 * @brief    Backup->App copy engine + boot decision (OTA-ARCH-0920)
 **************************************************************************
 *
 * Replaces the legacy A/B trial-boot state machine. Boot now:
 *   1. loads metadata (M1 diag)
 *   2. if backup_valid flag set -> copy Backup region to App region
 *      (verify -> erase -> copy -> re-verify -> clear flag); flag stays
 *      set until re-verify passes, so power loss during copy simply
 *      retries on next boot (idempotent recovery)
 *   3. verifies App region image and jumps (M3/M4 diag)
 */

#ifndef __BOOT_TRIAL_H
#define __BOOT_TRIAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "boot_metadata.h"

extern ota_metadata_t g_meta;

uint8_t detect_boot_reason(void);

/** @brief  nonzero when metadata carries a pending backup->App copy */
int8_t boot_backup_pending(const ota_metadata_t *meta);

/**
 * @brief  copy Backup region firmware into App region
 * @note   power-loss safe: backup_valid stays set until copy + recheck
 *         pass; each power-on retries. Steps: verify Backup image
 *         (magic/length/CRC/ECDSA + vectors target App window) -> erase
 *         App region -> word copy -> re-verify App -> update metadata
 *         (app_valid/app_crc32, clear backup_valid) -> save.
 * @retval 0 on success, -1 on failure (fail_step stored in
 *         meta.reserved_trial[META_COPY_FAIL_STEP_OFF], copy_retry_count++
 *         saved; flag left set for retry)
 */
int8_t boot_copy_backup(ota_metadata_t *meta);

/**
 * @brief  verify App region image (magic/length/CRC/ECDSA + vectors)
 * @retval 0 if valid, -1 on failure (g_verify_fail_step set)
 */
int8_t boot_app_image_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_TRIAL_H */
