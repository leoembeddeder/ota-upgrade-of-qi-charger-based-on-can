/**
 **************************************************************************
 * @file     ota_trigger.h
 * @brief    OTA metadata + constants for APP (Boot + App architecture)
 **************************************************************************
 *
 * Single-App OTA (OTA-ARCH-0920): A/B slots removed. APP receives
 * firmware into the Backup region via UDS, verifies it, flags metadata
 * (backup_valid) and resets; BOOT performs the physical copy.
 *
 * Flash layout (must match bootloader boot_metadata.h — same-source):
 *   Boot     0x08000000..0x08003FFF  16KB
 *   App      0x08004000..0x0800FFFF  48KB (XATO header @0x08004000,
 *            code entry @0x08004100 = Keil IROM1 0x08004100,0xBF00)
 *   Backup   0x08010000..0x0801BFFF  48KB
 *   Metadata 0x0801C000 / 0x0801C800, DeviceInfo 0x0801D000, NVM 0x0801E000
 *
 * ota_metadata_t layout is byte-frozen (272B, crc32 @0x10C) and identical
 * across projects; legacy slot fields retained as reserved bytes.
 */

#ifndef __OTA_TRIGGER_H
#define __OTA_TRIGGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f422_426.h"

/* exported constants ------------------------------------------------------ */

#define OTA_BOOT_BASE_ADDR      0x08000000U
#define OTA_BOOT_SIZE           0x4000U
#define OTA_APP_BASE_ADDR       0x08004000U   /* App image region */
#define OTA_APP_SIZE            0xC000U       /* 48KB */
#define OTA_BACKUP_BASE_ADDR    0x08010000U   /* staging region */
#define OTA_BACKUP_SIZE         0xC000U       /* 48KB */
#define OTA_APP_ENTRY_ADDR      (OTA_APP_BASE_ADDR + OTA_IMAGE_HEADER_SIZE)

#define OTA_META_PRIMARY_ADDR   0x0801C000U
#define OTA_META_BACKUP_ADDR    0x0801C800U
#define OTA_META_PAGE_SIZE      0x400U
#define OTA_IMAGE_HEADER_SIZE   256U
#define OTA_FLASH_SECTOR_SIZE   0x400U

#define OTA_META_MAGIC          0x4F54414DU   /* "MATO" */
#define OTA_META_VERSION        2U            /* single-App format */
#define OTA_IMAGE_MAGIC         0x4F544158U   /* "XATO" */

/* DID diagnostics markers (0x2114 during download reports 0x02 = backup) */
#define OTA_DL_TARGET_BACKUP    0x02U
#define OTA_SLOT_NONE           0xFEU         /* deprecated alias */
#define OTA_SLOT_A              0U            /* deprecated: App region */
#define OTA_SLOT_B              1U            /* deprecated alias */

#define OTA_STATE_IDLE          0x00U
#define OTA_STATE_DOWNLOADING   0x01U

#define OTA_BOOT_REASON_POWER_ON   0x00U
#define OTA_BOOT_REASON_SW         0x01U
#define OTA_BOOT_REASON_WDG        0x02U
#define OTA_BOOT_REASON_OTA_ACT    0x03U
#define OTA_BOOT_REASON_COPY_FAIL  0x04U
#define OTA_BOOT_REASON_ROLLBACK   OTA_BOOT_REASON_COPY_FAIL /* deprecated alias */

/* exported types ---------------------------------------------------------- */

/**
 * @brief  XATO image header (256B, byte-frozen layout; @0x4C reserved
 *         placeholder — original version field removed, packed as 0x00)
 */
typedef struct
{
  uint32_t magic;               /* 0x4F544158 "XATO" */
  uint32_t image_length;       /* payload bytes after header */
  uint32_t crc32;              /* CRC32 of payload */
  uint8_t  signature[64];      /* ECDSA P-256 R||S */
  uint8_t  hdr_reserved_ver[16];
  uint32_t build_timestamp;
  uint8_t  reserved[160];
} ota_image_header_t;

/**
 * @brief  OTA metadata (272B, byte-frozen; identical to boot_metadata.h)
 */
typedef struct
{
  uint32_t magic;               /* @0x00 */
  uint32_t version;             /* @0x04 = 2 */
  uint8_t  reserved_slots[2];   /* @0x08 deprecated */
  uint8_t  app_valid;           /* @0x0A */
  uint8_t  backup_valid;        /* @0x0B pending-copy flag */
  uint32_t app_crc32;           /* @0x0C */
  uint32_t backup_crc32;        /* @0x10 */
  uint8_t  reserved_trial[8];   /* @0x14 deprecated */
  uint32_t copy_retry_count;    /* @0x1C */
  uint8_t  last_boot_reason;    /* @0x20 */
  uint8_t  ota_state;           /* @0x21 */
  uint8_t  reserved2[2];        /* @0x22 */
  uint8_t  padding[232];        /* @0x24 */
  uint32_t crc32;               /* @0x10C */
} ota_metadata_t;

/* exported functions ------------------------------------------------------ */

int8_t ota_metadata_read(ota_metadata_t *meta);
int8_t ota_metadata_save(const ota_metadata_t *meta);
uint32_t ota_crc32(const void *data, uint32_t length);

/* deprecated compatibility shims (single-App architecture) */
uint8_t ota_running_slot(void);      /* always 0: App region */
uint32_t ota_running_slot_base(void);/* always OTA_APP_BASE_ADDR */

#ifdef __cplusplus
}
#endif

#endif /* __OTA_TRIGGER_H */
