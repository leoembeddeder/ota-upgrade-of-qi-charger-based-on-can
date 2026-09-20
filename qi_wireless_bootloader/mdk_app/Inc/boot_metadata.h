/**
 **************************************************************************
 * @file     boot_metadata.h
 * @brief    OTA metadata management for bootloader (Boot + App architecture)
 **************************************************************************
 *
 * Single-App OTA architecture (OTA-ARCH-0920, A/B slots removed):
 *   Boot     0x08000000..0x08003FFF  16KB  (uvprojx IROM 0x08000000,0x4000)
 *   App      0x08004000..0x0800FFFF  48KB  XATO header @0x08004000,
 *                                          code entry  @0x08004100
 *   Backup   0x08010000..0x0801BFFF  48KB  pending-upgrade firmware image
 *   Metadata 0x0801C000 / 0x0801C800 primary / backup copies
 *   DeviceInfo 0x0801D000  NVM 0x0801E000..0x0801FFFF
 *
 * Upgrade flow: host streams firmware into Backup via APP UDS
 * (0x31/0x34/0x36/0x37) -> APP verifies image -> APP sets backup_valid
 * flag in metadata -> reset -> BOOT erases App region, copies Backup ->
 * App, re-verifies, clears flag, jumps to App.
 *
 * Metadata struct v3 slim layout (28 bytes, crc32 at offset 0x18,
 * META_VERSION=3), defined identically in boot/app projects —
 * C/Python same-source; v2-and-earlier metadata rejected -> defaults
 * rebuild (Q2 slimming).
 */

#ifndef __BOOT_METADATA_H
#define __BOOT_METADATA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "at32f422_426.h"

/* exported constants ------------------------------------------------------ */

#define BOOT_BASE_ADDR          0x08000000U
#define BOOT_SIZE               0x4000U       /* 16KB bootloader */
#define APP_BASE_ADDR           0x08004000U   /* App image region (incl. header) */
#define APP_SIZE                0xC000U       /* 48KB */
#define BACKUP_BASE_ADDR        0x08010000U   /* pending-firmware staging region */
#define BACKUP_SIZE             0xC000U       /* 48KB */
#define IMAGE_HEADER_SIZE       256U
#define APP_ENTRY_ADDR          (APP_BASE_ADDR + IMAGE_HEADER_SIZE) /* 0x08004100 */
#define META_PRIMARY_ADDR       0x0801C000U
#define META_BACKUP_ADDR        0x0801C800U
#define META_PAGE_SIZE          0x400U
#define DEVICE_INFO_ADDR        0x0801D000U
#define DEVICE_INFO_SIZE        0x1000U
#define FLASH_SECTOR_SIZE       0x400U
#define SRAM_BASE_ADDR          0x20000000U
#define SRAM_SIZE               0x5000U

#define META_MAGIC              0x4F54414DU   /* "MATO" */
#define META_VERSION            3U            /* v3 slim layout (Q2) */

/* boot reason codes */
#define BOOT_REASON_POWER_ON    0x00U
#define BOOT_REASON_SW          0x01U
#define BOOT_REASON_WDG         0x02U
#define BOOT_REASON_OTA_ACT     0x03U         /* backup copy in progress */
#define BOOT_REASON_COPY_FAIL   0x04U         /* backup copy verification failed */

/* ota_state codes */
#define OTA_STATE_IDLE          0x00U
#define OTA_STATE_DOWNLOADING   0x01U


/* exported types ---------------------------------------------------------- */

/**
 * @brief  OTA metadata structure v3 (28 bytes total, slim layout)
 * @note   Field offsets must stay identical across boot/app projects.
 *         Legacy A/B-slot fields are retained as reserved bytes.
 */
typedef struct
{
  uint32_t magic;            /* @0x00 0x4F54414D "MATO" */
  uint32_t version;          /* @0x04 META_VERSION = 3 */
  uint8_t  app_valid;        /* @0x08 App region image valid (M1 diag b1) */
  uint8_t  backup_valid;     /* @0x09 Backup pending-copy flag */
  uint8_t  copy_fail_step;   /* @0x0A last backup-copy fail_step */
  uint8_t  last_boot_reason; /* @0x0B BOOT_REASON_* */
  uint32_t backup_crc32;     /* @0x0C staging payload CRC (copy pre-check) */
  uint32_t copy_retry_count; /* @0x10 failed copy attempts (DID 0x2116) */
  uint8_t  ota_state;        /* @0x14 OTA_STATE_* */
  uint8_t  reserved[3];      /* @0x15 alignment/future */
  uint32_t crc32;            /* @0x18 CRC32 of all above fields */
} ota_metadata_t;

/* exported functions ------------------------------------------------------ */

int8_t boot_metadata_init(ota_metadata_t *meta);
int8_t boot_metadata_save(ota_metadata_t *meta);
uint32_t boot_crc32(const void *data, uint32_t length);
uint32_t boot_crc32_continue(uint32_t crc, const void *data, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_METADATA_H */
