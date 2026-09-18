/**
  **************************************************************************
  * @file     ota_trigger.c
  * @brief    OTA trigger module for APP firmware
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
#include "ota_trigger.h"
#include "at32f422_426_can.h"
#include "sit1145.h"
#include "timer_drv.h"
#include "at32f422_426_conf.h"
#include <string.h>

/* private define ------------------------------------------------------------*/

/** @brief  byte offset of the crc32 field within ota_metadata_t */
#define META_CRC32_OFFSET    (sizeof(ota_metadata_t) - sizeof(uint32_t))

/* private functions ---------------------------------------------------------*/

/**
 * @brief  validate metadata structure: check magic, version, and CRC32
 * @param  meta: pointer to metadata to validate
 * @retval 0 if valid, -1 if invalid
 */
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

/**
 * @brief  fill metadata with default values
 * @param  meta: pointer to metadata to initialize
 * @retval none
 */
static void meta_fill_defaults(ota_metadata_t *meta)
{
  memset((void *)meta, 0, sizeof(ota_metadata_t));

  meta->magic             = OTA_META_MAGIC;
  meta->version           = OTA_META_VERSION;
  meta->active_slot       = OTA_SLOT_A;
  meta->pending_slot      = OTA_SLOT_NONE;
  meta->slot_a_valid      = 0;
  meta->slot_b_valid      = 0;
  meta->slot_a_crc32      = 0;
  meta->slot_b_crc32      = 0;
  meta->trial_state       = 0;  /* TRIAL_STATE_IDLE */
  meta->trial_slot        = OTA_SLOT_A;
  meta->trial_retry_count = 0;
  meta->trial_max_retries = 3;
  meta->trial_timeout_sec = 10;
  meta->reserved1         = 0;
  meta->rollback_count    = 0;
  meta->last_boot_reason  = 0;
  meta->ota_state         = OTA_STATE_IDLE;
}

/* exported functions --------------------------------------------------------*/

/**
 * @brief  compute CRC32 (IEEE 802.3, polynomial 0xEDB88320)
 * @param  data: pointer to data
 * @param  length: number of bytes
 * @retval CRC32 value
 */
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

/**
 * @brief  read metadata from primary flash location
 * @param  meta: pointer to metadata structure to fill
 * @retval 0 on success (valid metadata), -1 on failure (invalid or read error)
 */
int8_t ota_metadata_read(ota_metadata_t *meta)
{
  const ota_metadata_t *primary;
  const ota_metadata_t *backup;

  /* try primary metadata */
  primary = (const ota_metadata_t *)OTA_META_PRIMARY_ADDR;
  if (meta_validate(primary) == 0)
  {
    memcpy((void *)meta, (const void *)primary, sizeof(ota_metadata_t));
    return 0;
  }

  /* primary invalid, try backup */
  backup = (const ota_metadata_t *)OTA_META_BACKUP_ADDR;
  if (meta_validate(backup) == 0)
  {
    memcpy((void *)meta, (const void *)backup, sizeof(ota_metadata_t));
    return 0;
  }

  /* both invalid, use defaults */
  meta_fill_defaults(meta);
  return -1;
}

static int8_t meta_write_to_flash(uint32_t addr, const ota_metadata_t *meta)
{
  const uint32_t *src;
  uint32_t words;
  uint32_t i;
  flash_status_type status;

  /* Single Bank: metadata 保存 = 擦 1KB 扇区 + 逐字编程，擦写期间内核
   * 从同一 Bank 取指会卡死（4ec5858 事故同根因），IRQ 取指同样踩 Flash。
   * 本函数是 APP 侧 metadata 落盘唯一咽喉点：trial confirm、0x37
   * commit_trial、0x31 擦除后 invalidate 落盘等全部调用方自动受保护。
   * 调用点不得再包一层 __disable_irq/__enable_irq：__enable_irq 无条件
   * 清 PRIMASK，双层包裹会在内层返回时提前开中断，属误导性代码。 */
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
  flash_lock();

  __enable_irq();
  return 0;
}

/**
 * @brief  save metadata to backup then primary Flash
 */
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

uint32_t ota_running_slot_base(void)
{
  uint32_t pc;

  /* Do not use SCB->VTOR: it may still be the link address of a
   * Slot-A-built image running from Slot B. PC is always in this image. */
  __asm volatile ("mov %0, pc" : "=r"(pc));
  if (pc >= OTA_APP_B_BASE_ADDR)
  {
    return OTA_APP_B_BASE_ADDR;
  }
  return OTA_APP_A_BASE_ADDR;
}

uint8_t ota_running_slot(void)
{
  if (ota_running_slot_base() == OTA_APP_B_BASE_ADDR)
  {
    return OTA_SLOT_B;
  }
  return OTA_SLOT_A;
}

/* 2026-09-18：ota_get_image_version() 已删除——镜像头不再携带版本号
   （版本号唯一真相源=can_protocol.c 的 SW_VERSION_STR 编译常量，DID 0xF195
   直接取该常量）。51e52cf 改造后本函数已无调用者；后续改造又将
   image_header_t/ota_image_header_t 的 version[16] 字段声明删除，0x4C 区
   改为 hdr_reserved_ver[16] 保留占位（固定填 0x00，偏移锁定不可回收）。
   头总长 256B 与其余字段偏移逐字节不变，无任何代码引用该区域。*/

#define TRIAL_HEALTH_DELAY_MS   100U

static uint8_t  g_trial_pending = 0;
static uint32_t g_trial_start_ms = 0;
static uint32_t g_trial_deadline_ms = 0;

static int8_t ota_confirm_trial(void)
{
  ota_metadata_t meta;

  if (ota_metadata_read(&meta) != 0)
  {
    return -1;
  }
  if (meta.trial_state != TRIAL_STATE_ACTIVE)
  {
    return 0;
  }

  /* only confirm if this image is the one under trial */
  if (meta.trial_slot != ota_running_slot())
  {
    return -1;
  }

  meta.active_slot  = meta.trial_slot;
  meta.pending_slot = OTA_SLOT_NONE;
  meta.trial_state  = TRIAL_STATE_CONFIRMED;
  meta.trial_retry_count = 0;
  meta.ota_state    = OTA_STATE_IDLE;

  if (ota_metadata_save(&meta) != 0)
  {
    return -1;
  }
  /* Same-bank Flash erase stalls CAN; error IRQ is lost. Kick bus-off so
   * a second OTA without power cycle can still talk. */
  if (can_busoff_get(CAN1) != RESET)
  {
    can_busoff_reset(CAN1);
  }
  (void)sit1145_normal_mode_set();
  return 0;
}

void ota_trial_init(void)
{
  ota_metadata_t meta;
  uint32_t timeout_ms;

  g_trial_pending = 0;
  if (ota_metadata_read(&meta) != 0)
  {
    return;
  }
  if (meta.trial_state != TRIAL_STATE_ACTIVE)
  {
    return;
  }
  if (meta.trial_slot != ota_running_slot())
  {
    return;
  }

  timeout_ms = (uint32_t)meta.trial_timeout_sec * 1000U;
  if (timeout_ms == 0U)
  {
    timeout_ms = 10000U;
  }

  g_trial_pending     = 1;
  g_trial_start_ms    = timer_get_tick();
  g_trial_deadline_ms = g_trial_start_ms + timeout_ms;
}

void ota_trial_poll(void)
{
  uint32_t now;

  if (g_trial_pending == 0U)
  {
    return;
  }

  now = timer_get_tick();
  if ((int32_t)(now - g_trial_deadline_ms) >= 0)
  {
    /* trial timed out: reset into bootloader to roll back */
    NVIC_SystemReset();
  }

  /* confirm only after core init has been up for a short health window */
  if ((now - g_trial_start_ms) >= TRIAL_HEALTH_DELAY_MS)
  {
    if (ota_confirm_trial() == 0)
    {
      g_trial_pending = 0;
    }
  }
}
