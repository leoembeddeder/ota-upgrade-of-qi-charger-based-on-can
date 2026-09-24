/**
 **************************************************************************
 * @file     boot_trial.c
 * @brief    Backup->App copy engine + boot decision (OTA-ARCH-0920)
 **************************************************************************
 *
 * Single-App OTA: APP streams new firmware into the Backup region and
 * sets meta.backup_valid; this file (Boot side) performs the physical
 * copy Backup -> App. An APP cannot erase the flash region it executes
 * from, so the copy must happen here, before jumping.
 *
 * Power-loss idempotence: backup_valid is cleared only AFTER the copied
 * image re-verifies in the App region. Interruptions at any point leave
 * the flag set; the next power-on re-runs the whole sequence.
 */

#include "boot_trial.h"
#include "boot_verify.h"
#include "boot_jump.h"
#include "boot_safe_mode.h"
#include "at32f422_426_conf.h"
#include <string.h>

ota_metadata_t g_meta;

uint8_t detect_boot_reason(void)
{
  uint8_t reason = BOOT_REASON_POWER_ON;
  flag_status wdt  = crm_flag_get(CRM_WDT_RESET_FLAG);
  flag_status wwdt = crm_flag_get(CRM_WWDT_RESET_FLAG);
  flag_status sw   = crm_flag_get(CRM_SW_RESET_FLAG);
  flag_status por  = crm_flag_get(CRM_POR_RESET_FLAG);

  /* snapshot first: crm_flag_clear(RSTFC) clears every reset flag */
  if ((wdt != RESET) || (wwdt != RESET))
  {
    reason = BOOT_REASON_WDG;
  }
  else if (sw != RESET)
  {
    reason = BOOT_REASON_SW;
  }
  else if (por != RESET)
  {
    reason = BOOT_REASON_POWER_ON;
  }

  crm_flag_clear(CRM_ALL_RESET_FLAG);
  return reason;
}

int8_t boot_backup_pending(const ota_metadata_t *meta)
{
  return (meta->backup_valid != 0U) ? 1 : 0;
}

/**
 * @brief  erase the App flash region with bounded polling
 * @note   IRQ-off + flash unlock/lock around the erase burst, same
 *         single-bank discipline as meta_write_to_flash (boot_metadata.c)
 *         and copy_program_region below. Flash stays locked until the
 *         first write of the boot session: with metadata primary valid
 *         (normal OTA path) Boot performs no earlier flash write, so an
 *         erase without unlock always fails -> copy_fail_step=0xFE
 *         (OTA-ARCH-0920-D5 P0-1). Erase range/sector size unchanged.
 */
static int8_t copy_erase_app_region(void)
{
  uint32_t addr;
  flash_status_type status;

  __disable_irq();
  flash_unlock();
  for (addr = APP_BASE_ADDR; addr < (APP_BASE_ADDR + APP_SIZE);
       addr += FLASH_SECTOR_SIZE)
  {
    status = flash_sector_erase(addr);
    if (status != FLASH_OPERATE_DONE)
    {
      flash_lock();
      __enable_irq();
      return -1;
    }
  }
  flash_lock();
  __enable_irq();
  return 0;
}

/**
 * @brief  word-program [src, src+len) at dst, then readback verify
 * @note   IRQ disabled per write burst (single-bank flash contention,
 *         same rationale as meta_write_to_flash in boot_metadata.c)
 */
static int8_t copy_program_region(uint32_t dst, const uint8_t *src,
                                  uint32_t len)
{
  uint32_t i;
  uint32_t words;
  const uint32_t *wsrc = (const uint32_t *)src;

  words = (len + 3U) / 4U;
  __disable_irq();
  flash_unlock();
  for (i = 0U; i < words; i++)
  {
    if (flash_word_program(dst + (i * 4U), wsrc[i]) != FLASH_OPERATE_DONE)
    {
      flash_lock();
      __enable_irq();
      return -1;
    }
  }
  for (i = 0U; i < words; i++)
  {
    if (*(volatile uint32_t *)(dst + (i * 4U)) != wsrc[i])
    {
      flash_lock();
      __enable_irq();
      return -1;
    }
  }
  flash_lock();
  __enable_irq();
  return 0;
}

int8_t boot_copy_backup(ota_metadata_t *meta)
{
  const ota_image_view_t *hdr;
  uint32_t total;

  hdr = (const ota_image_view_t *)BACKUP_BASE_ADDR;

  /* 1. verify Backup image; vectors must target the App window */
  boot_diag_m3(0U, 0xFFU, 0U); /* pre-verify marker: target=Backup region */
  if (boot_verify_image(BACKUP_BASE_ADDR, BACKUP_SIZE,
                        APP_BASE_ADDR, APP_SIZE) != 0)
  {
    meta->copy_fail_step = g_verify_fail_step;
    meta->copy_retry_count++;
    meta->last_boot_reason = BOOT_REASON_COPY_FAIL;
    (void)boot_metadata_save(meta);
    boot_diag_m3(0U, g_verify_fail_step, 0U);
    return -1;
  }
  boot_diag_m3(1U, g_verify_fail_step, 0U);

  /* staging consistency: metadata record must match image CRC */
  if (meta->backup_crc32 != hdr->crc32)
  {
    meta->copy_fail_step = 0xFFU; /* record mismatch */
    meta->copy_retry_count++;
    meta->last_boot_reason = BOOT_REASON_COPY_FAIL;
    (void)boot_metadata_save(meta);
    return -1;
  }

  /* 2. erase App region */
  if (copy_erase_app_region() != 0)
  {
    meta->copy_fail_step = 0xFEU; /* erase fail */
    meta->copy_retry_count++;
    meta->last_boot_reason = BOOT_REASON_COPY_FAIL;
    (void)boot_metadata_save(meta);
    return -1;
  }

  /* 3. copy header + payload from Backup to App */
  total = IMAGE_HEADER_SIZE + hdr->image_length;
  if (copy_program_region(APP_BASE_ADDR, (const uint8_t *)BACKUP_BASE_ADDR,
                          total) != 0)
  {
    meta->copy_fail_step = 0xFDU; /* program fail */
    meta->copy_retry_count++;
    meta->last_boot_reason = BOOT_REASON_COPY_FAIL;
    (void)boot_metadata_save(meta);
    return -1;
  }

  /* 4. re-verify image in App region */
  boot_diag_m3(0U, 0xFFU, 1U); /* pre-verify marker: target=App region */
  if (boot_verify_image(APP_BASE_ADDR, APP_SIZE,
                        APP_BASE_ADDR, APP_SIZE) != 0)
  {
    meta->copy_fail_step = g_verify_fail_step;
    meta->copy_retry_count++;
    meta->last_boot_reason = BOOT_REASON_COPY_FAIL;
    (void)boot_metadata_save(meta);
    boot_diag_m3(0U, g_verify_fail_step, 1U);
    return -1;
  }

  /* 5. commit: only now clear the pending flag (power-loss safe) */
  meta->app_valid    = 1U;
  meta->backup_valid = 0U;
  meta->ota_state    = OTA_STATE_IDLE;
  meta->last_boot_reason = BOOT_REASON_OTA_ACT;
  meta->copy_fail_step = 0U;
  (void)boot_metadata_save(meta);
  boot_diag_m3(1U, g_verify_fail_step, 1U);
  return 0;
}

int8_t boot_app_image_ok(void)
{
  if (boot_verify_image(APP_BASE_ADDR, APP_SIZE,
                        APP_BASE_ADDR, APP_SIZE) == 0)
  {
    return 0;
  }
  return -1;
}
