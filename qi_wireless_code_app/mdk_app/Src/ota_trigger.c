/**
 **************************************************************************
 * @file     ota_trigger.c
 * @brief    OTA metadata access for APP (Boot + App architecture)
 **************************************************************************
 *
 * Metadata dual-copy read/save + CRC32. Trial-boot logic removed
 * (OTA-ARCH-0920): backup->App copy is performed by BOOT; APP no longer
 * confirms trials. ota_running_slot()/ota_running_slot_base() remain as
 * compatibility shims for can_protocol.c DID handlers.
 */

#include "ota_trigger.h"
#include "at32f422_426_can.h"
#include "sit1145.h"
#include "timer_drv.h"
#include "at32f422_426_conf.h"
#include <string.h>

#define META_CRC32_OFFSET    (sizeof(ota_metadata_t) - sizeof(uint32_t))

static int8_t meta_validate(const ota_metadata_t *meta)
{
  uint32_t computed_crc;
  uint32_t stored_crc;

  if (meta->magic != OTA_META_MAGIC)
  {
    return -1;
  }
  if (meta->version != OTA_META_VERSION)
  {
    return -1;
  }
  stored_crc   = meta->crc32;
  computed_crc = ota_crc32((const void *)meta, META_CRC32_OFFSET);
  if (computed_crc != stored_crc)
  {
    return -1;
  }
  return 0;
}

static void meta_fill_defaults(ota_metadata_t *meta)
{
  memset((void *)meta, 0, sizeof(ota_metadata_t));

  meta->magic             = OTA_META_MAGIC;
  meta->version           = OTA_META_VERSION;
  meta->app_valid         = 0U;
  meta->backup_valid      = 0U;
  meta->copy_fail_step    = 0U;
  meta->last_boot_reason  = 0U;
  meta->backup_crc32      = 0U;
  meta->copy_retry_count  = 0U;
  meta->ota_state         = OTA_STATE_IDLE;
  meta->reserved[0]       = 0U;
  meta->reserved[1]       = 0U;
  meta->reserved[2]       = 0U;
}

uint32_t ota_crc32(const void *data, uint32_t length)
{
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFU;
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
  return crc ^ 0xFFFFFFFFU;
}

int8_t ota_metadata_read(ota_metadata_t *meta)
{
  const ota_metadata_t *primary;
  const ota_metadata_t *backup;

  primary = (const ota_metadata_t *)OTA_META_PRIMARY_ADDR;
  if (meta_validate(primary) == 0)
  {
    memcpy((void *)meta, (const void *)primary, sizeof(ota_metadata_t));
    return 0;
  }

  backup = (const ota_metadata_t *)OTA_META_BACKUP_ADDR;
  if (meta_validate(backup) == 0)
  {
    memcpy((void *)meta, (const void *)backup, sizeof(ota_metadata_t));
    return 0;
  }

  meta_fill_defaults(meta);
  return -1;
}

/**
 * @brief  write one metadata copy to flash (IRQ-off single-bank funnel;
 *         every APP-side save goes through here — incident 4ec5858)
 */
static int8_t meta_write_to_flash(uint32_t addr, const ota_metadata_t *meta)
{
  const uint32_t *src;
  uint32_t words;
  uint32_t i;
  flash_status_type status;

  __disable_irq();

  flash_unlock();
  status = flash_sector_erase(addr);
  if (status != FLASH_OPERATE_DONE)
  {
    flash_lock();
    __enable_irq();
    return -1;
  }

  src   = (const uint32_t *)meta;
  words = sizeof(ota_metadata_t) / sizeof(uint32_t);
  for (i = 0; i < words; i++)
  {
    status = flash_word_program(addr + (i * 4U), src[i]);
    if (status != FLASH_OPERATE_DONE)
    {
      flash_lock();
      __enable_irq();
      return -1;
    }
  }

  /* word-by-word readback verify — same structure as bootloader
   * boot_metadata.c meta_write_to_flash (evaluator N1, dual-project
   * same-source); IRQs stay masked for the readback window */
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

int8_t ota_metadata_save(const ota_metadata_t *meta)
{
  ota_metadata_t meta_copy;

  memcpy((void *)&meta_copy, (const void *)meta, sizeof(ota_metadata_t));
  meta_copy.crc32 = ota_crc32((const void *)&meta_copy, META_CRC32_OFFSET);

  if (meta_write_to_flash(OTA_META_BACKUP_ADDR, &meta_copy) != 0)
  {
    return -1;
  }
  if (meta_write_to_flash(OTA_META_PRIMARY_ADDR, &meta_copy) != 0)
  {
    return -1;
  }
  return 0;
}

/* deprecated compatibility shims — single-App architecture has no slots;
 * DID 0x2113 keeps answering 0x00 ("running from App region") so legacy
 * host tooling does not hard-fail */

uint32_t ota_running_slot_base(void)
{
  return OTA_APP_BASE_ADDR;
}

uint8_t ota_running_slot(void)
{
  return 0U;
}
