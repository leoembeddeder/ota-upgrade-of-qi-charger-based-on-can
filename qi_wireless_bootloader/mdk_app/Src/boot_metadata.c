/**
 **************************************************************************
 * @file     boot_metadata.c
 * @brief    OTA metadata management for bootloader (Boot + App architecture)
 **************************************************************************
 *
 * Dual-copy metadata at 0x0801C000 / 0x0801C800. Single-App format
 * (META_VERSION=2): legacy A/B-era metadata is rejected -> defaults.
 * All writes funnel through meta_write_to_flash (IRQ-off single-bank
 * erase+program+readback) — power-safe, backup copy written first.
 */

#include "boot_metadata.h"
#include "boot_safe_mode.h"
#include "at32f422_426_conf.h"
#include <string.h>

#define META_CRC32_OFFSET    (sizeof(ota_metadata_t) - sizeof(uint32_t))

static int8_t meta_flash_erase_page(uint32_t addr)
{
  flash_status_type status;

  status = flash_sector_erase(addr);
  if (status != FLASH_OPERATE_DONE)
  {
    return -1;
  }
  return 0;
}

static int8_t meta_flash_write_word(uint32_t addr, uint32_t data)
{
  flash_status_type status;

  status = flash_word_program(addr, data);
  if (status != FLASH_OPERATE_DONE)
  {
    return -1;
  }
  return 0;
}

/**
 * @brief  write ota_metadata_t to one flash sector (IRQ-off: single-bank
 *         erase/program while fetching from same bank wedges the core,
 *         incident 4ec5858 — every metadata save funnels through here)
 */
static int8_t meta_write_to_flash(uint32_t addr, const ota_metadata_t *meta)
{
  const uint32_t *src;
  uint32_t words;
  uint32_t i;

  __disable_irq();

  flash_unlock();

  if (meta_flash_erase_page(addr) != 0)
  {
    flash_lock();
    __enable_irq();
    return -1;
  }

  src   = (const uint32_t *)meta;
  words = sizeof(ota_metadata_t) / sizeof(uint32_t);

  for (i = 0; i < words; i++)
  {
    if (meta_flash_write_word(addr + (i * 4U), src[i]) != 0)
    {
      flash_lock();
      __enable_irq();
      return -1;
    }
  }

  for (i = 0; i < words; i++)
  {
    if (*(volatile uint32_t *)(addr + (i * 4U)) != src[i])
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

static int8_t meta_validate(const ota_metadata_t *meta)
{
  uint32_t computed_crc;
  uint32_t stored_crc;

  if (meta->magic != META_MAGIC)
  {
    return -1;
  }
  if (meta->version != META_VERSION)
  {
    return -1;
  }
  stored_crc   = meta->crc32;
  computed_crc = boot_crc32((const void *)meta, META_CRC32_OFFSET);
  if (computed_crc != stored_crc)
  {
    return -1;
  }
  return 0;
}

static void meta_fill_defaults(ota_metadata_t *meta)
{
  memset((void *)meta, 0, sizeof(ota_metadata_t));

  meta->magic             = META_MAGIC;
  meta->version           = META_VERSION;
  meta->reserved_slots[0] = 0U;
  meta->reserved_slots[1] = 0xFEU;
  meta->app_valid         = 0U;
  meta->backup_valid      = 0U;
  meta->app_crc32         = 0U;
  meta->backup_crc32      = 0U;
  memset(meta->reserved_trial, 0, sizeof(meta->reserved_trial));
  meta->copy_retry_count  = 0U;
  meta->last_boot_reason  = BOOT_REASON_POWER_ON;
  meta->ota_state         = OTA_STATE_IDLE;
  meta->reserved2[0]      = 0U;
  meta->reserved2[1]      = 0U;
}

uint32_t boot_crc32_continue(uint32_t crc, const void *data, uint32_t length)
{
  const uint8_t *p = (const uint8_t *)data;
  uint32_t i;
  uint32_t j;
  uint32_t bit;

  for (i = 0; i < length; i++)
  {
    crc ^= (uint32_t)p[i];
    for (j = 0; j < 8; j++)
    {
      bit = crc & 1U;
      crc >>= 1;
      if (bit != 0U)
      {
        crc ^= 0xEDB88320U;
      }
    }
  }
  return crc;
}

uint32_t boot_crc32(const void *data, uint32_t length)
{
  return boot_crc32_continue(0xFFFFFFFFU, data, length) ^ 0xFFFFFFFFU;
}

int8_t boot_metadata_init(ota_metadata_t *meta)
{
  const ota_metadata_t *primary;
  const ota_metadata_t *backup;

  primary = (const ota_metadata_t *)META_PRIMARY_ADDR;
  if (meta_validate(primary) == 0)
  {
    memcpy((void *)meta, (const void *)primary, sizeof(ota_metadata_t));
    g_diag_meta_src = 0U;
    boot_diag_m1(meta);
    return 0;
  }

  backup = (const ota_metadata_t *)META_BACKUP_ADDR;
  if (meta_validate(backup) == 0)
  {
    memcpy((void *)meta, (const void *)backup, sizeof(ota_metadata_t));
    boot_metadata_save(meta);
    g_diag_meta_src = 1U;
    boot_diag_m1(meta);
    return 0;
  }

  meta_fill_defaults(meta);
  meta->crc32 = boot_crc32((const void *)meta, META_CRC32_OFFSET);
  boot_metadata_save(meta);
  g_diag_meta_src = 2U;
  boot_diag_m1(meta);

  return -1;
}

int8_t boot_metadata_save(ota_metadata_t *meta)
{
  meta->crc32 = boot_crc32((const void *)meta, META_CRC32_OFFSET);

  /* backup copy first so a torn primary write stays recoverable */
  if (meta_write_to_flash(META_BACKUP_ADDR, meta) != 0)
  {
    return -1;
  }
  if (meta_write_to_flash(META_PRIMARY_ADDR, meta) != 0)
  {
    return -1;
  }
  return 0;
}
