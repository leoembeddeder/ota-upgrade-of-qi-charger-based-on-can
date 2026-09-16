/**
  **************************************************************************
  * @file     boot_metadata.h
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

/* define to prevent recursive inclusion -------------------------------------*/
#ifndef __BOOT_METADATA_H
#define __BOOT_METADATA_H

#ifdef __cplusplus
extern "C" {
#endif

/* includes ------------------------------------------------------------------*/
#include "at32f422_426.h"

/* exported constants --------------------------------------------------------*/

/** @brief  Flash layout (Keil Target IROM, no scatter).
 *          Bootloader 16KB (jump/trial only). APP_A/B 48KB each. */
#define BOOT_BASE_ADDR          0x08000000U   /*!< bootloader start address */
#define DEVICE_INFO_ADDR        0x0801D000U   /*!< SN area; OTA must not erase */
#define DEVICE_INFO_SIZE        0x1000U
#define BOOT_SIZE               0x4000U       /*!< bootloader size: 16KB */
#define APP_A_BASE_ADDR         0x08004000U   /*!< application A start address */
#define APP_A_SIZE              0xC000U       /*!< application A size: 48KB */
#define APP_B_BASE_ADDR         0x08010000U   /*!< application B start address */
#define APP_B_SIZE              0xC000U       /*!< application B size: 48KB */
#define IMAGE_HEADER_SIZE       256U          /*!< image header size in bytes */
#define META_PRIMARY_ADDR       0x0801C000U   /*!< primary metadata (2KB sector) */
#define META_BACKUP_ADDR        0x0801C800U   /*!< backup metadata (next 2KB sector) */
#define META_PAGE_SIZE          0x400U        /*!< metadata erase size: 1 sector */
#define APP_A_ENTRY_ADDR        (APP_A_BASE_ADDR + IMAGE_HEADER_SIZE)
#define APP_B_ENTRY_ADDR        (APP_B_BASE_ADDR + IMAGE_HEADER_SIZE)
#define FLASH_SECTOR_SIZE       0x400U        /*!< AT32F426 sector size: 1KB */
#define SRAM_BASE_ADDR          0x20000000U
#define SRAM_SIZE               0x5000U       /*!< 20KB SRAM */

/** @brief  OTA metadata magic and version */
#define META_MAGIC              0x4F54414DU   /*!< "MATO" */
#define META_VERSION            1U            /*!< metadata format version */

/** @brief  Slot definitions */
#define SLOT_NONE               0xFEU         /*!< no pending slot */
#define SLOT_A                  0U            /*!< slot A index */
#define SLOT_B                  1U            /*!< slot B index */

/** @brief  Trial boot state machine */
#define TRIAL_STATE_IDLE        0U            /*!< no trial in progress */
#define TRIAL_STATE_PENDING     1U            /*!< trial requested, not started */
#define TRIAL_STATE_ACTIVE      2U            /*!< trial boot in progress */
#define TRIAL_STATE_CONFIRMED   3U            /*!< trial passed, confirmed */

/** @brief  Default trial parameters */
#define TRIAL_MAX_RETRIES       3U            /*!< max trial retry count */
#define TRIAL_TIMEOUT_SEC       10U           /*!< trial timeout in seconds */

/** @brief  Boot reason codes */
#define BOOT_REASON_POWER_ON    0x00U         /*!< POR / NRST / unknown */
#define BOOT_REASON_SW          0x01U         /*!< NVIC_SystemReset / UDS 0x11 */
#define BOOT_REASON_WDG         0x02U         /*!< IWDG/WWDT (legacy, watchdog removed) */
#define BOOT_REASON_OTA_ACT     0x03U         /*!< OTA activation */
#define BOOT_REASON_ROLLBACK    0x04U         /*!< rollback from failed trial */

/** @brief  OTA state codes */
#define OTA_STATE_IDLE          0x00U         /*!< no OTA in progress */
#define OTA_STATE_DOWNLOADING   0x01U         /*!< OTA download in progress */

/* exported types ------------------------------------------------------------*/

/**
 * @brief  OTA metadata structure (272 bytes total)
 * @note   stored in Flash at META_PRIMARY_ADDR and META_BACKUP_ADDR.
 *         CRC32 covers all fields except the trailing crc32 field itself.
 */
typedef struct
{
  uint32_t magic;               /*!< 0x4F54414D "MATO" */
  uint32_t version;             /*!< metadata format version = 1 */
  uint8_t  active_slot;         /*!< 0=A, 1=B */
  uint8_t  pending_slot;        /*!< 0=A, 1=B, 0xFE=none */
  uint8_t  slot_a_valid;        /*!< 1=slot A image valid */
  uint8_t  slot_b_valid;        /*!< 1=slot B image valid */
  uint32_t slot_a_crc32;        /*!< CRC32 of slot A image */
  uint32_t slot_b_crc32;        /*!< CRC32 of slot B image */
  uint8_t  trial_state;         /*!< 0=IDLE, 1=PENDING, 2=ACTIVE, 3=CONFIRMED */
  uint8_t  trial_slot;          /*!< slot under trial (0=A, 1=B) */
  uint8_t  trial_retry_count;   /*!< current retry count */
  uint8_t  trial_max_retries;   /*!< max retries (default 3) */
  uint16_t trial_timeout_sec;   /*!< trial timeout in seconds (default 10) */
  uint16_t reserved1;           /*!< reserved for alignment */
  uint32_t rollback_count;      /*!< number of rollbacks performed */
  uint8_t  last_boot_reason;    /*!< 0x00=POR, 0x01=SW, 0x02=WDG, 0x03=OTA, 0x04=rollback */
  uint8_t  ota_state;           /*!< 0x00=idle, 0x01=downloading */
  uint8_t  reserved2[2];        /*!< reserved */
  uint8_t  padding[232];        /*!< padding to 272 bytes (reduced from 512 for size optimization) */
  uint32_t crc32;               /*!< CRC32 of all above fields */
} ota_metadata_t;

/* exported functions -------------------------------------------------------*/

/**
 * @brief  initialize metadata module, load and validate metadata
 * @param  meta: pointer to metadata structure to fill
 * @retval 0 on success (valid metadata loaded), -1 on failure (defaults used)
 */
int8_t boot_metadata_init(ota_metadata_t *meta);

/**
 * @brief  save metadata to backup then primary (power-loss safe)
 * @param  meta: pointer to metadata structure to save
 * @retval 0 on success, -1 on Flash write error
 */
int8_t boot_metadata_save(ota_metadata_t *meta);

/**
 * @brief  get the base address for a given slot
 * @param  slot: slot index (0=A, 1=B)
 * @retval base address of the slot, or 0 if invalid
 */
uint32_t boot_metadata_slot_addr(uint8_t slot);

/**
 * @brief  get the size of a slot
 * @param  slot: slot index (0=A, 1=B)
 * @retval slot size in bytes, or 0 if invalid
 */
uint32_t boot_metadata_slot_size(uint8_t slot);

/**
 * @brief  compute CRC32 over a memory region
 * @param  data: pointer to data
 * @param  length: number of bytes
 * @retval CRC32 value
 */
uint32_t boot_crc32(const void *data, uint32_t length);

/**
 * @brief  continue an IEEE 802.3 CRC32 (running value, not final-xor'd)
 * @param  crc: previous running CRC (start with 0xFFFFFFFF)
 * @param  data: pointer to next bytes
 * @param  length: number of bytes
 * @retval updated running CRC (caller does `^ 0xFFFFFFFF` at the end)
 */
uint32_t boot_crc32_continue(uint32_t crc, const void *data, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_METADATA_H */
