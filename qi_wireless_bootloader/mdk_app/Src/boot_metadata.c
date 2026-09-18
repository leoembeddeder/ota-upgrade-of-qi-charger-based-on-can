/**
  **************************************************************************
  * @file     boot_metadata.c
  * @brief    OTA metadata management for bootloader
  **************************************************************************
  *
  * Copyright (c) 2025, Artery Technology, All rights reserved.
  *
  * The software Board Support Package (BSP) that is made available to
  * download from Artery official website is the copyrighted work of Artery.
  * Artery authorizes customers to use, copy, and distribute the BSP
  * software and its related documentation for the purpose of design and
  * development in conjunction with Artery microcontrollers. Use of the
  * software is governed by this copyright notice and the following disclaimer.
  *
  * THIS SOFTWARE IS PROVIDED ON "AS IS" BASIS WITHOUT WARRANTIES,
  * GUARANTEES OR REPRESENTATIONS OF ANY KIND. ARTERY EXPRESSLY DISCLAIMS,
  * TO THE FULLEST EXTENT PERMITTED BY LAW, ALL EXPRESS, IMPLIED OR
  * STATUTORY OR OTHER WARRANTIES, GUARANTEES OR REPRESENTATIONS,
  * INCLUDING BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY,
  * FITNESS FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
  *
  **************************************************************************
  */

/* includes ------------------------------------------------------------------*/
#include "boot_metadata.h"
#include "at32f422_426_conf.h"
#include <string.h>

/* private define ------------------------------------------------------------*/

/** @brief  byte offset of the crc32 field within ota_metadata_t */
#define META_CRC32_OFFSET    (sizeof(ota_metadata_t) - sizeof(uint32_t))

/* private functions ---------------------------------------------------------*/

/**
 * @brief  erase a flash page (sector) at the given address
 * @param  addr: flash page start address (must be page-aligned)
 * @retval 0 on success, -1 on failure
 */
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

/**
 * @brief  program a 32-bit word to flash
 * @param  addr: target address (must be word-aligned)
 * @param  data: 32-bit data to program
 * @retval 0 on success, -1 on failure
 */
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
 * @brief  write an ota_metadata_t structure to a flash page
 * @param  addr: target flash page start address
 * @param  meta: pointer to metadata to write
 * @retval 0 on success, -1 on failure
 */
static int8_t meta_write_to_flash(uint32_t addr, const ota_metadata_t *meta)
{
  const uint32_t *src;
  uint32_t words;
  uint32_t i;

  /* Single Bank: metadata 保存 = 擦 1KB 扇区 + 逐字编程 + 读回，擦写期间
   * 中断取指踩同一 Bank 会卡死内核（4ec5858 事故同根因）。Boot 上电后
   * SysTick 已在跑（timer_drv_init），状态机落盘（boot_trial.c 5 处、
   * try_boot_slot 2 处、boot_metadata_init 2 处、main.c 回滚落盘、
   * safe mode 现场落盘）全部经本咽喉点，自动受保护。
   * 关中断区间内仅 Flash 寄存器操作与 68 字读回，无 SPI/CAN/长循环。 */
  __disable_irq();

  flash_unlock();

  if (meta_flash_erase_page(addr) != 0)
  {
    flash_lock();
    __enable_irq();
    return -1;
  }

  /* write structure word by word */
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

  /* readback */
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

/**
 * @brief  validate metadata structure: check magic, version, and CRC32
 * @param  meta: pointer to metadata to validate
 * @retval 0 if valid, -1 if invalid
 */
static int8_t meta_validate(const ota_metadata_t *meta)
{
  uint32_t computed_crc;
  uint32_t stored_crc;

  /* check magic */
  if (meta->magic != META_MAGIC)
  {
    return -1;
  }

  /* check version */
  if (meta->version != META_VERSION)
  {
    return -1;
  }

  /* check CRC32: covers all fields except the trailing crc32 itself */
  stored_crc   = meta->crc32;
  computed_crc = boot_crc32((const void *)meta, META_CRC32_OFFSET);

  if (computed_crc != stored_crc)
  {
    return -1;
  }

  return 0;
}

/**
 * @brief  fill metadata with default values
 * @param  meta: pointer to metadata to initialize
 * @retval none
 */
static void meta_fill_defaults(ota_metadata_t *meta)
{
  memset((void *)meta, 0, sizeof(ota_metadata_t));

  meta->magic             = META_MAGIC;
  meta->version           = META_VERSION;
  meta->active_slot       = SLOT_A;
  meta->pending_slot      = SLOT_NONE;
  meta->slot_a_valid      = 0;
  meta->slot_b_valid      = 0;
  meta->slot_a_crc32      = 0;
  meta->slot_b_crc32      = 0;
  meta->trial_state       = TRIAL_STATE_IDLE;
  meta->trial_slot        = SLOT_A;
  meta->trial_retry_count = 0;
  meta->trial_max_retries = TRIAL_MAX_RETRIES;
  meta->trial_timeout_sec = TRIAL_TIMEOUT_SEC;
  meta->reserved1         = 0;
  meta->rollback_count    = 0;
  meta->last_boot_reason  = BOOT_REASON_POWER_ON;
  meta->ota_state         = OTA_STATE_IDLE;
}

/* exported functions --------------------------------------------------------*/

/**
 * @brief  compute CRC32 (IEEE 802.3, polynomial 0xEDB88320)
 * @param  data: pointer to data
 * @param  length: number of bytes
 * @retval CRC32 value
 */
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

/**
 * @brief  initialize metadata module, load and validate metadata
 * @note   tries primary first, then backup. if both invalid, uses defaults
 *         and writes defaults to primary area.
 * @param  meta: pointer to metadata structure to fill
 * @retval 0 on success (valid metadata loaded), -1 on failure (defaults used)
 */
int8_t boot_metadata_init(ota_metadata_t *meta)
{
  const ota_metadata_t *primary;
  const ota_metadata_t *backup;

  /* try primary metadata */
  primary = (const ota_metadata_t *)META_PRIMARY_ADDR;
  if (meta_validate(primary) == 0)
  {
    memcpy((void *)meta, (const void *)primary, sizeof(ota_metadata_t));
    return 0;
  }

  /* primary invalid, try backup */
  backup = (const ota_metadata_t *)META_BACKUP_ADDR;
  if (meta_validate(backup) == 0)
  {
    /* restore primary from backup */
    memcpy((void *)meta, (const void *)backup, sizeof(ota_metadata_t));
    boot_metadata_save(meta);
    return 0;
  }

  /* both invalid, use defaults */
  meta_fill_defaults(meta);
  meta->crc32 = boot_crc32((const void *)meta, META_CRC32_OFFSET);
  boot_metadata_save(meta);

  return -1;
}

/**
 * @brief  save metadata to backup then primary Flash
 * @note   backup is a dedicated 2KB sector (0x0801C800), NVM stays at 0x0801E000.
 * @param  meta: pointer to metadata structure to save
 * @retval 0 on success, -1 on Flash write error
 */
int8_t boot_metadata_save(ota_metadata_t *meta)
{
  meta->crc32 = boot_crc32((const void *)meta, META_CRC32_OFFSET);

  /* write backup first so a torn primary write can still be recovered */
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

/**
 * @brief  get the base address for a given slot
 * @param  slot: slot index (0=A, 1=B)
 * @retval base address of the slot, or 0 if invalid
 */
uint32_t boot_metadata_slot_addr(uint8_t slot)
{
  if (slot == SLOT_A)
  {
    return APP_A_BASE_ADDR;
  }
  else if (slot == SLOT_B)
  {
    return APP_B_BASE_ADDR;
  }

  return 0;
}

/**
 * @brief  get the size of a slot
 * @param  slot: slot index (0=A, 1=B)
 * @retval slot size in bytes, or 0 if invalid
 */
uint32_t boot_metadata_slot_size(uint8_t slot)
{
  if (slot == SLOT_A)
  {
    return APP_A_SIZE;
  }
  else if (slot == SLOT_B)
  {
    return APP_B_SIZE;
  }

  return 0;
}
