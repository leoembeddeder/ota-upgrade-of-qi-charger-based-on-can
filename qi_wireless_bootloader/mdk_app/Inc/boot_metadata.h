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
 * Metadata struct layout is byte-frozen (272 bytes, crc32 at offset 268)
 * and defined identically in boot/app projects — C/Python same-source.
 * META_VERSION bumped to 2: v1 (A/B era) metadata is rejected -> defaults.
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
#define META_VERSION            2U            /* single-App format */

/* boot reason codes */
#define BOOT_REASON_POWER_ON    0x00U
#define BOOT_REASON_SW          0x01U
#define BOOT_REASON_WDG         0x02U
#define BOOT_REASON_OTA_ACT     0x03U         /* backup copy in progress */
#define BOOT_REASON_COPY_FAIL   0x04U         /* backup copy verification failed */

/* ota_state codes */
#define OTA_STATE_IDLE          0x00U
#define OTA_STATE_DOWNLOADING   0x01U

/* Diagnostics: reserved_trial[0] holds last backup-copy fail_step */
#define META_COPY_FAIL_STEP_OFF 0U

/* exported types ---------------------------------------------------------- */

/**
 * @brief  OTA metadata structure (272 bytes total, layout byte-frozen)
 * @note   Field offsets must stay identical across boot/app projects.
 *         Legacy A/B-slot fields are retained as reserved bytes.
 */
typedef struct
{
  uint32_t magic;               /* 0x4F54414D "MATO"                 @0x00 */
  uint32_t version;             /* META_VERSION = 2                   @0x04 */
  uint8_t  reserved_slots[2];   /* legacy active/pending slot (dep.)  @0x08 */
  uint8_t  app_valid;           /* App region image valid             @0x0A */
  uint8_t  backup_valid;        /* Backup holds pending fw (copy flag)@0x0B */
  uint32_t app_crc32;           /* CRC32 of App payload               @0x0C */
  uint32_t backup_crc32;        /* CRC32 of Backup payload            @0x10 */
  uint8_t  reserved_trial[8];   /* legacy trial fields (deprecated)   @0x14 */
  uint32_t copy_retry_count;    /* failed copy attempts (diagnostics) @0x1C */
  uint8_t  last_boot_reason;    /* BOOT_REASON_*                      @0x20 */
  uint8_t  ota_state;           /* OTA_STATE_*                        @0x21 */
  uint8_t  reserved2[2];        /*                                    @0x22 */
  uint8_t  padding[232];        /*                                    @0x24 */
  uint32_t crc32;               /* CRC32 of all above                 @0x10C */
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
