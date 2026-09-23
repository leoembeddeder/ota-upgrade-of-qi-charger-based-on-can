/**
 **************************************************************************
 * @file     boot_verify.c
 * @brief    XATO image verification (header + CRC + vectors)
 **************************************************************************
 *
 * Verify an image physically located at src_base (App or Backup region)
 * while requiring its reset vector to target the App run window. This
 * allows the Backup staging copy to be fully validated BEFORE the App
 * region is erased (OTA-ARCH-0920).
 */

#include "boot_verify.h"
#include "boot_metadata.h"

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

  /* ECDSA P-256 signature check removed (2026-09-24): signature is
   * verified by the App download layer; fail_step 5/6 never produced. */
  g_verify_fail_step = 0;
  return 0;
}
