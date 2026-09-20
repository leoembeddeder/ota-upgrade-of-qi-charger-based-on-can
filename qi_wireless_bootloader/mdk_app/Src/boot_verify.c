/**
 **************************************************************************
 * @file     boot_verify.c
 * @brief    XATO image verification (header + CRC + ECDSA + vectors)
 **************************************************************************
 *
 * Verify an image physically located at src_base (App or Backup region)
 * while requiring its reset vector to target the App run window. This
 * allows the Backup staging copy to be fully validated BEFORE the App
 * region is erased (OTA-ARCH-0920).
 */

#include "boot_verify.h"
#include "boot_metadata.h"
#include "uECC.h"
#include "sha256.h"

/** @brief  ECDSA P-256 public key (uncompressed SEC1 04||X||Y)
 *          dedicated .rodata section; magic marker follows for corruption
 *          detection. Fallback when Device Info carries no provisioned key. */
__attribute__((section(".ecdsa_pubkey"), used))
const uint8_t g_ecdsa_public_key[65] = {
  0x04,
  0x79, 0x0d, 0x96, 0xca, 0x91, 0x2d, 0x90, 0xdb,
  0x73, 0xdf, 0x21, 0xb0, 0x6e, 0xe7, 0xce, 0x19,
  0xaa, 0x7c, 0x1f, 0x75, 0x30, 0x55, 0x0a, 0x48,
  0x21, 0x84, 0x19, 0xb4, 0x4b, 0x4c, 0x37, 0xcb,
  0xf5, 0x7c, 0xd3, 0xfc, 0x9e, 0x26, 0xbe, 0x1b,
  0xa6, 0x94, 0xdd, 0x45, 0x62, 0x7e, 0xaa, 0xca,
  0x71, 0x38, 0xf5, 0x7a, 0x8e, 0xa8, 0xd5, 0xdd,
  0x20, 0x70, 0x33, 0x26, 0xf0, 0x95, 0x41, 0x71
};

__attribute__((section(".ecdsa_pubkey"), used))
static const uint32_t g_pubkey_magic = ECDSA_PUBKEY_MAGIC;

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

/** @brief  Device Info pubkey offsets (must match device_info_t layout) */
#define DI_PUBKEY_OFFSET        56U
#define DI_PUBKEY_VALID_OFFSET  121U
#define DI_PUBKEY_VALID_VALUE   0x01U

const uint8_t *boot_verify_get_public_key(void)
{
  const uint8_t *di_base = (const uint8_t *)DEVICE_INFO_ADDR;

  if (di_base[DI_PUBKEY_VALID_OFFSET] == DI_PUBKEY_VALID_VALUE)
  {
    if (di_base[DI_PUBKEY_OFFSET] == 0x04U)
    {
      return &di_base[DI_PUBKEY_OFFSET];
    }
  }

  if (g_pubkey_magic != ECDSA_PUBKEY_MAGIC)
  {
    return (const uint8_t *)0;
  }
  return g_ecdsa_public_key;
}

int8_t boot_verify_image(uint32_t src_base, uint32_t src_size,
                         uint32_t run_base, uint32_t run_size)
{
  const ota_image_view_t *header;
  const uint8_t *image_data;
  uint32_t computed_crc;
  uint32_t max_image_len;

  header = (const ota_image_view_t *)src_base;

  /* check 1: XATO magic */
  if (header->magic != 0x4F544158U)
  {
    g_verify_fail_step = 1;
    return -1;
  }

  /* check 2: payload length within source region */
  max_image_len = src_size - IMAGE_HEADER_SIZE;
  if ((header->image_length == 0U) || (header->image_length > max_image_len))
  {
    g_verify_fail_step = 2;
    return -1;
  }

  /* check 3: payload CRC32 */
  image_data = (const uint8_t *)(src_base + IMAGE_HEADER_SIZE);
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

  /* check 3b: reset vector must target the App run window
   * (Backup-staged images are linked for App, so this rejects images
   * built for any other base) */
  {
    const uint32_t *vec = (const uint32_t *)(src_base + IMAGE_HEADER_SIZE);
    uint32_t reset = vec[1] & 0xFFFFFFFEU;
    uint32_t entry = run_base + IMAGE_HEADER_SIZE;
    uint32_t end   = run_base + run_size;
    if ((reset < entry) || (reset >= end))
    {
      g_verify_fail_step = 4;
      return -1;
    }
  }

  /* check 4: ECDSA P-256 over payload */
  {
    const uint8_t *public_key;
    uint8_t image_hash[32];
    int verify_result;

    public_key = boot_verify_get_public_key();
    if (public_key == (const uint8_t *)0)
    {
      g_verify_fail_step = 5;
      return -1;
    }

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
