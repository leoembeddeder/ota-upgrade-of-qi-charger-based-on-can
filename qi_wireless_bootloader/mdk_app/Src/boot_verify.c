/**
  **************************************************************************
  * @file     boot_verify.c
  * @brief    Image verification for bootloader (CRC32 + ECDSA P-256)
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
#include "boot_verify.h"
#include "boot_metadata.h"
#include "uECC.h"
#include "sha256.h"

/* private constants ---------------------------------------------------------*/

/** @brief  ECDSA P-256 public key (uncompressed SEC1: 04 || x || y)
 *          Stored in a dedicated .rodata section.  The linker will place this
 *          in the bootloader flash region (0x08000000..0x08003FFF, 16KB).
 *          A magic marker is appended after the key for corruption detection.
 *          Generated via: openssl ecparam -genkey -name prime256v1
 *          Private key stored at: docs/keys/private.pem (for host-side signing)
 */
__attribute__((section(".ecdsa_pubkey"), used))
const uint8_t g_ecdsa_public_key[65] = {
  0x04,  /* uncompressed point tag */
  /* 32-byte X coordinate */
  0x79, 0x0d, 0x96, 0xca, 0x91, 0x2d, 0x90, 0xdb,
  0x73, 0xdf, 0x21, 0xb0, 0x6e, 0xe7, 0xce, 0x19,
  0xaa, 0x7c, 0x1f, 0x75, 0x30, 0x55, 0x0a, 0x48,
  0x21, 0x84, 0x19, 0xb4, 0x4b, 0x4c, 0x37, 0xcb,
  /* 32-byte Y coordinate */
  0xf5, 0x7c, 0xd3, 0xfc, 0x9e, 0x26, 0xbe, 0x1b,
  0xa6, 0x94, 0xdd, 0x45, 0x62, 0x7e, 0xaa, 0xca,
  0x71, 0x38, 0xf5, 0x7a, 0x8e, 0xa8, 0xd5, 0xdd,
  0x20, 0x70, 0x33, 0x26, 0xf0, 0x95, 0x41, 0x71
};

/** @brief  Magic marker immediately after the public key (for corruption check) */
__attribute__((section(".ecdsa_pubkey"), used))
static const uint32_t g_pubkey_magic = ECDSA_PUBKEY_MAGIC;

/** @brief  Last verification failure step (0=pass, 1=magic, 2=length, 3=CRC, 4=reset_handler, 5=pubkey, 6=ECDSA) */
volatile uint8_t g_verify_fail_step = 0;

static void (*g_verify_pump)(void) = 0;

#define VERIFY_PUMP_CHUNK  256U

void boot_verify_set_progress_cb(void (*cb)(void))
{
  g_verify_pump = cb;
}

static void verify_pump(void)
{
  if (g_verify_pump != 0)
  {
    g_verify_pump();
  }
}

/* exported functions --------------------------------------------------------*/

/** @brief  Device Info pubkey offset (must match device_info_t layout) */
#define DI_PUBKEY_OFFSET        56U   /* offsetof ecdsa_pubkey */
#define DI_PUBKEY_VALID_OFFSET  121U  /* offsetof pubkey_valid */
#define DI_PUBKEY_VALID_VALUE   0x01U

/**
 * @brief  get pointer to the ECDSA public key
 * @note   ä¼åä» Device Info åºè¯»åï¼æ¯æåææ´æ¢ï¼ï¼
 *          è¥ Device Info æªéç½®ååéå° Bootloader åç½®å¬é¥ã
 * @retval pointer to 65-byte uncompressed SEC1 public key, or NULL
 */
const uint8_t *boot_verify_get_public_key(void)
{
  const uint8_t *di_base = (const uint8_t *)DEVICE_INFO_ADDR;

  /* try Device Info first (supports key rotation) */
  if (di_base[DI_PUBKEY_VALID_OFFSET] == DI_PUBKEY_VALID_VALUE)
  {
    if (di_base[DI_PUBKEY_OFFSET] == 0x04U)
    {
      return &di_base[DI_PUBKEY_OFFSET];
    }
  }

  /* fallback: hardcoded public key in bootloader flash */
  if (g_pubkey_magic != ECDSA_PUBKEY_MAGIC)
  {
    return (const uint8_t *)0;
  }
  return g_ecdsa_public_key;
}

/**
 * @brief  get pointer to image header at a given base address
 * @param  base_addr: start address of the image slot
 * @retval pointer to image_header_t (memory-mapped)
 */
const image_header_t *boot_verify_get_header(uint32_t base_addr)
{
  return (const image_header_t *)base_addr;
}

/**
 * @brief  verify an application image at the given base address
 * @note   checks: magic, length, CRC32, Reset Handler in-slot, KEYP, ECDSA.
 * @param  base_addr: start address of the image slot (header is at this address)
 * @param  slot_size: total size of the slot in bytes
 * @retval 0 if image is valid, -1 if verification fails
 */
int8_t boot_verify_image(uint32_t base_addr, uint32_t slot_size)
{
  const image_header_t *header;
  const uint8_t *image_data;
  uint32_t computed_crc;
  uint32_t max_image_len;

  /* cast base address to header pointer */
  header = (const image_header_t *)base_addr;

  /* check 1: verify magic number */
  if (header->magic != IMAGE_MAGIC)
  {
    g_verify_fail_step = 1;
    return -1;
  }

  /* check 2: verify image_length is within slot bounds */
  /* slot_size minus header size is the maximum allowed image length */
  max_image_len = slot_size - IMAGE_HEADER_SIZE;
  if ((header->image_length == 0) || (header->image_length > max_image_len))
  {
    g_verify_fail_step = 2;
    return -1;
  }

  /* check 3: verify CRC32 of image data (data follows the header) */
  image_data = (const uint8_t *)(base_addr + IMAGE_HEADER_SIZE);
  {
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t ofs = 0U;
    uint32_t n;

    verify_pump();
    while (ofs < header->image_length)
    {
      n = header->image_length - ofs;
      if (n > VERIFY_PUMP_CHUNK)
      {
        n = VERIFY_PUMP_CHUNK;
      }
      crc = boot_crc32_continue(crc, (const void *)(image_data + ofs), n);
      ofs += n;
      verify_pump();
    }
    computed_crc = crc ^ 0xFFFFFFFFU;
  }

  if (computed_crc != header->crc32)
  {
    g_verify_fail_step = 3;
    return -1;
  }

  /* check 3b: reset handler must live inside this slot (rejects A-linked image in B) */
  {
    const uint32_t *vec = (const uint32_t *)(base_addr + IMAGE_HEADER_SIZE);
    uint32_t reset = vec[1] & 0xFFFFFFFEU;
    uint32_t entry = base_addr + IMAGE_HEADER_SIZE;
    uint32_t end   = base_addr + slot_size;
    if ((reset < entry) || (reset >= end))
    {
      g_verify_fail_step = 4;
      return -1;
    }
  }

  /* check 4: ECDSA P-256 signature verification */
  {
    const uint8_t *public_key;
    uint8_t image_hash[32];
    int verify_result;

    /* read the pre-provisioned public key (symbol-based, no hard-coded address) */
    public_key = boot_verify_get_public_key();
    if (public_key == (const uint8_t *)0)
    {
      g_verify_fail_step = 5;
      return -1;
    }

    /* compute SHA-256 hash of the image data (excluding header), sliced so
     * Boot 0x37 can keep NRC 0x78 / SIT1145 / CAN RX alive. */
    {
      sha256_ctx_t ctx;
      uint32_t ofs = 0U;
      uint32_t n;

      sha256_init(&ctx);
      while (ofs < header->image_length)
      {
        n = header->image_length - ofs;
        if (n > VERIFY_PUMP_CHUNK)
        {
          n = VERIFY_PUMP_CHUNK;
        }
        sha256_update(&ctx, (const void *)(image_data + ofs), n);
        ofs += n;
        verify_pump();
      }
      sha256_final(&ctx, image_hash);
      verify_pump();
    }

    /* verify ECDSA P-256 signature
     * public_key : 65-byte uncompressed SEC1 point (04 || x || y)
     * image_hash : 32-byte SHA-256 digest
     * signature  : 64-byte IEEE P1363 (R || S, each 32 bytes) */
    verify_result = uECC_verify(public_key, image_hash, header->signature);

    if (verify_result != 1)
    {
      g_verify_fail_step = 6;
      return -1;
    }
  }

  g_verify_fail_step = 0;
  return 0;
}
