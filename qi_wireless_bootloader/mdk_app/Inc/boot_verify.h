/**
 **************************************************************************
 * @file     boot_verify.h
 * @brief    XATO image verification (header + CRC + ECDSA + vectors)
 **************************************************************************
 */

#ifndef __BOOT_VERIFY_H
#define __BOOT_VERIFY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief  image header view at a flash address (XATO layout, 256B)
 */
typedef struct
{
  uint32_t magic;               /* 0x4F544158 "XATO" */
  uint32_t image_length;       /* payload bytes after header */
  uint32_t crc32;              /* CRC32 of payload */
  uint8_t  signature[64];      /* ECDSA P-256 R||S */
  uint8_t  hdr_reserved_ver[16]; /* reserved placeholder @0x4C, filled 0x00 */
  uint32_t build_timestamp;
  uint8_t  reserved[160];
} ota_image_view_t;

/** @brief  Magic marker value to detect public key corruption in Flash */
#define ECDSA_PUBKEY_MAGIC  0x4B594550U  /* "KEYP" */

/** @brief  last verification failure step
 *          0=pass 1=magic 2=length 3=CRC 4=reset-window 5=pubkey 6=ECDSA */
extern volatile uint8_t g_verify_fail_step;

void boot_verify_set_progress_cb(void (*cb)(void));
const uint8_t *boot_verify_get_public_key(void);

/**
 * @brief  verify XATO image at src_base; reset vector must fall inside
 *         [run_base + IMAGE_HEADER_SIZE, run_base + run_size)
 * @param  src_base: where the image physically sits (App or Backup region)
 * @param  src_size: size of that flash region
 * @param  run_base: region the image is linked to execute from (App)
 * @param  run_size: size of the run region
 * @retval 0 if valid, -1 on failure (g_verify_fail_step set)
 */
int8_t boot_verify_image(uint32_t src_base, uint32_t src_size,
                         uint32_t run_base, uint32_t run_size);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_VERIFY_H */
