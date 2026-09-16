/**
  **************************************************************************
  * @file     ota_download.c
  * @brief    Write the inactive APP slot via UDS 0x31/0x34/0x36/0x37
  **************************************************************************
  */

#include "ota_download.h"
#include "ota_trigger.h"
#include "can_protocol.h"
#include "can_driver.h"
#include "sit1145.h"
#include "timer_drv.h"
#include "device_info.h"
#include "sha256.h"
#include "uECC.h"
#include "at32f422_426_flash.h"
#include <string.h>

#define FLASH_PHYSICAL_END  0x08020000U
#define VERIFY_CHUNK        256U

static uint8_t  g_erased;
static uint8_t  g_active;
static uint8_t  g_trial_ready;
static uint8_t  g_slot;
static uint32_t g_base;
static uint32_t g_size;
static uint32_t g_write_addr;
static uint32_t g_bytes_written;
static uint32_t g_expected_size;
static uint8_t  g_block_seq;
static uint8_t  g_pad[4];
static uint8_t  g_pad_len;
static uint8_t  g_exit_pending;

static uint8_t inactive_slot(void)
{
  return (ota_running_slot() == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
}

static uint32_t slot_base(uint8_t slot)
{
  return (slot == OTA_SLOT_B) ? OTA_APP_B_BASE_ADDR : OTA_APP_A_BASE_ADDR;
}

void ota_dl_abort(void)
{
  g_erased = 0U;
  g_active = 0U;
  g_exit_pending = 0U;
  g_pad_len = 0U;
  g_block_seq = 0U;
  g_bytes_written = 0U;
  g_expected_size = 0U;
}

uint8_t ota_dl_target_slot(void)
{
  return g_slot;
}

uint8_t ota_dl_erased(void)
{
  return g_erased;
}

uint8_t ota_dl_active(void)
{
  return g_active;
}

uint8_t ota_dl_trial_ready(void)
{
  return g_trial_ready;
}

static uint8_t program_words(const uint8_t *src, uint16_t src_len)
{
  uint16_t n = src_len;
  uint16_t idx = 0U;
  flash_status_type st;

  while (n > 0U)
  {
    while ((g_pad_len < 4U) && (n > 0U))
    {
      g_pad[g_pad_len++] = src[idx++];
      n--;
    }
    if (g_pad_len < 4U)
    {
      break;
    }
    if ((g_bytes_written + 4U) > g_size)
    {
      return 1U;
    }
    {
      uint32_t word;
      word  =  (uint32_t)g_pad[0];
      word |= ((uint32_t)g_pad[1] << 8);
      word |= ((uint32_t)g_pad[2] << 16);
      word |= ((uint32_t)g_pad[3] << 24);
      st = flash_word_program(g_write_addr, word);
      if (st != FLASH_OPERATE_DONE)
      {
        return 1U;
      }
    }
    g_write_addr += 4U;
    g_bytes_written += 4U;
    g_pad_len = 0U;
  }
  return 0U;
}

static uint8_t program_flush(void)
{
  uint32_t word;
  uint8_t k;
  flash_status_type st;

  if (g_pad_len == 0U)
  {
    return 0U;
  }
  word = 0xFFFFFFFFU;
  for (k = 0U; k < g_pad_len; k++)
  {
    word &= ~((uint32_t)0xFFU << (k * 8U));
    word |= ((uint32_t)g_pad[k] << (k * 8U));
  }
  st = flash_word_program(g_write_addr, word);
  if (st != FLASH_OPERATE_DONE)
  {
    return 1U;
  }
  g_write_addr += (uint32_t)g_pad_len;
  g_bytes_written += (uint32_t)g_pad_len;
  g_pad_len = 0U;
  return 0U;
}

extern const uint8_t g_app_ecdsa_pubkey[65];

static const uint8_t *image_pubkey(void)
{
  device_info_t di;

  if (device_info_read(&di) == 0)
  {
    if ((di.pubkey_valid == 0x01U) && (di.ecdsa_pubkey[0] == 0x04U))
    {
      return di.ecdsa_pubkey;
    }
  }
  if (g_app_ecdsa_pubkey[0] == 0x04U)
  {
    return g_app_ecdsa_pubkey;
  }
  return (const uint8_t *)0;
}

static int8_t verify_slot_image(uint32_t base, uint32_t slot_size)
{
  const ota_image_header_t *hdr = (const ota_image_header_t *)base;
  const uint8_t *payload;
  uint32_t max_len;
  uint32_t crc;
  const uint8_t *pk;
  uint8_t hash[32];
  sha256_ctx_t ctx;
  uint32_t ofs;
  uint32_t n;
  uint32_t reset;
  uint32_t entry;
  uint32_t end;

  if (hdr->magic != OTA_IMAGE_MAGIC)
  {
    return -1;
  }
  max_len = slot_size - OTA_IMAGE_HEADER_SIZE;
  if ((hdr->image_length == 0U) || (hdr->image_length > max_len))
  {
    return -1;
  }
  payload = (const uint8_t *)(base + OTA_IMAGE_HEADER_SIZE);
  crc = ota_crc32(payload, hdr->image_length);
  if (crc != hdr->crc32)
  {
    return -1;
  }
  reset = (*(const uint32_t *)(base + OTA_IMAGE_HEADER_SIZE + 4U)) & 0xFFFFFFFEU;
  entry = base + OTA_IMAGE_HEADER_SIZE;
  end = base + slot_size;
  if ((reset < entry) || (reset >= end))
  {
    return -1;
  }
  pk = image_pubkey();
  if (pk == (const uint8_t *)0)
  {
    return -1;
  }
  sha256_init(&ctx);
  ofs = 0U;
  while (ofs < hdr->image_length)
  {
    n = hdr->image_length - ofs;
    if (n > VERIFY_CHUNK)
    {
      n = VERIFY_CHUNK;
    }
    sha256_update(&ctx, payload + ofs, n);
    ofs += n;
    can_proto_send_pending(UDS_SID_TRANSFER_EXIT);
  }
  sha256_final(&ctx, hash);
  if (uECC_verify(pk, hash, hdr->signature) != 1)
  {
    return -1;
  }
  return 0;
}

static int8_t commit_trial(void)
{
  ota_metadata_t meta;
  const ota_image_header_t *hdr = (const ota_image_header_t *)g_base;

  if (ota_metadata_read(&meta) != 0)
  {
    memset(&meta, 0, sizeof(meta));
    meta.magic = OTA_META_MAGIC;
    meta.version = OTA_META_VERSION;
    meta.active_slot = ota_running_slot();
    meta.pending_slot = OTA_SLOT_NONE;
    meta.trial_max_retries = 3U;
    meta.trial_timeout_sec = 10U;
    if (meta.active_slot == OTA_SLOT_A)
    {
      meta.slot_a_valid = 1U;
    }
    else
    {
      meta.slot_b_valid = 1U;
    }
  }

  if (g_slot == OTA_SLOT_A)
  {
    meta.slot_a_valid = 1U;
    meta.slot_a_crc32 = hdr->crc32;
  }
  else
  {
    meta.slot_b_valid = 1U;
    meta.slot_b_crc32 = hdr->crc32;
  }
  meta.ota_state = OTA_STATE_IDLE;
  meta.pending_slot = g_slot;
  meta.trial_state = TRIAL_STATE_PENDING;
  meta.trial_slot = g_slot;
  meta.trial_retry_count = 0U;
  if (meta.trial_max_retries == 0U)
  {
    meta.trial_max_retries = 3U;
  }
  if (meta.trial_timeout_sec == 0U)
  {
    meta.trial_timeout_sec = 10U;
  }
  return ota_metadata_save(&meta);
}

void ota_dl_handle_erase(uint8_t *data, uint16_t len)
{
  uint16_t rid;
  uint8_t sub;
  uint32_t addr;
  uint8_t resp[4];
  ota_metadata_t meta;

  if (!can_proto_in_programming())
  {
    can_proto_send_nrc(UDS_SID_ROUTINE_CONTROL, UDS_NRC_CONDITIONS_NOT_CORRECT);
    return;
  }
  if (!can_proto_security_unlocked())
  {
    can_proto_send_nrc(UDS_SID_ROUTINE_CONTROL, UDS_NRC_SECURITY_ACCESS_DENIED);
    return;
  }
  if (len < 4U)
  {
    can_proto_send_nrc(UDS_SID_ROUTINE_CONTROL, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    return;
  }
  sub = data[1];
  rid = ((uint16_t)data[2] << 8) | (uint16_t)data[3];
  if ((sub != 0x01U) || (rid != ROUTINE_ERASE_MEMORY))
  {
    can_proto_send_nrc(UDS_SID_ROUTINE_CONTROL, UDS_NRC_REQUEST_OUT_OF_RANGE);
    return;
  }

  g_slot = inactive_slot();
  g_base = slot_base(g_slot);
  g_size = OTA_APP_A_SIZE;
  g_trial_ready = 0U;
  ota_dl_abort();
  g_slot = inactive_slot();
  g_base = slot_base(g_slot);
  g_size = OTA_APP_A_SIZE;

  if ((g_base + g_size) > FLASH_PHYSICAL_END)
  {
    can_proto_send_nrc(UDS_SID_ROUTINE_CONTROL, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
    return;
  }

  can_proto_begin_long_op(UDS_SID_ROUTINE_CONTROL);
  flash_unlock();
  for (addr = g_base; addr < (g_base + g_size); addr += OTA_FLASH_SECTOR_SIZE)
  {
    if (flash_sector_erase(addr) != FLASH_OPERATE_DONE)
    {
      flash_lock();
      can_proto_end_long_op();
      can_proto_send_nrc(UDS_SID_ROUTINE_CONTROL, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
      return;
    }
    can_proto_send_pending(UDS_SID_ROUTINE_CONTROL);
  }
  flash_lock();

  if (ota_metadata_read(&meta) == 0)
  {
    if (g_slot == OTA_SLOT_A)
    {
      meta.slot_a_valid = 0U;
    }
    else
    {
      meta.slot_b_valid = 0U;
    }
    meta.pending_slot = g_slot;
    (void)ota_metadata_save(&meta);
  }

  g_erased = 1U;
  g_write_addr = g_base;
  g_bytes_written = 0U;
  g_pad_len = 0U;
  g_block_seq = 0U;
  can_proto_end_long_op();
  resp[0] = (uint8_t)(UDS_SID_ROUTINE_CONTROL + UDS_POSITIVE_RESPONSE_OFFSET);
  resp[1] = sub;
  resp[2] = data[2];
  resp[3] = data[3];
  can_proto_send_response(resp, 4);
  (void)can_driver_wait_tx_idle(50U);
}

void ota_dl_handle_request_download(uint8_t *data, uint16_t len)
{
  uint8_t resp[4];
  uint8_t alfid;
  uint8_t addr_n;
  uint8_t size_n;
  uint32_t mem_size;
  uint8_t i;

  if (!can_proto_in_programming())
  {
    can_proto_send_nrc(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_CONDITIONS_NOT_CORRECT);
    return;
  }
  if (!can_proto_security_unlocked())
  {
    can_proto_send_nrc(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_SECURITY_ACCESS_DENIED);
    return;
  }
  if (g_erased == 0U)
  {
    can_proto_send_nrc(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_REQUEST_SEQUENCE_ERROR);
    return;
  }
  if (len < 3U)
  {
    can_proto_send_nrc(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    return;
  }
  alfid = data[2];
  addr_n = (uint8_t)((alfid >> 4) & 0x0FU);
  size_n = (uint8_t)(alfid & 0x0FU);
  if ((addr_n == 0U) || (size_n == 0U) || (addr_n > 4U) || (size_n > 4U) ||
      (len < (uint16_t)(3U + addr_n + size_n)))
  {
    can_proto_send_nrc(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    return;
  }
  mem_size = 0U;
  for (i = 0U; i < size_n; i++)
  {
    mem_size = (mem_size << 8) | (uint32_t)data[3U + addr_n + i];
  }
  if ((mem_size == 0U) || (mem_size > g_size))
  {
    can_proto_send_nrc(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_REQUEST_OUT_OF_RANGE);
    return;
  }
  g_expected_size = mem_size;
  g_write_addr = g_base;
  g_bytes_written = 0U;
  g_pad_len = 0U;
  g_block_seq = 0U;
  g_active = 1U;
  g_trial_ready = 0U;
  resp[0] = (uint8_t)(UDS_SID_REQUEST_DOWNLOAD + UDS_POSITIVE_RESPONSE_OFFSET);
  resp[1] = 0x20U;
  resp[2] = (uint8_t)((OTA_DL_MAX_BLOCK_LEN >> 8) & 0xFFU);
  resp[3] = (uint8_t)(OTA_DL_MAX_BLOCK_LEN & 0xFFU);
  can_proto_send_response(resp, 4);
}

void ota_dl_handle_transfer_data(uint8_t *data, uint16_t len)
{
  uint8_t bsc;
  uint16_t dlen;
  uint8_t resp[2];

  if (!can_proto_in_programming())
  {
    can_proto_send_nrc(UDS_SID_TRANSFER_DATA, UDS_NRC_CONDITIONS_NOT_CORRECT);
    return;
  }
  if (!can_proto_security_unlocked())
  {
    can_proto_send_nrc(UDS_SID_TRANSFER_DATA, UDS_NRC_SECURITY_ACCESS_DENIED);
    return;
  }
  if (g_active == 0U)
  {
    can_proto_send_nrc(UDS_SID_TRANSFER_DATA, UDS_NRC_REQUEST_SEQUENCE_ERROR);
    return;
  }
  if (len < 3U)
  {
    can_proto_send_nrc(UDS_SID_TRANSFER_DATA, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    return;
  }
  bsc = data[1];
  if ((g_block_seq != 0U) && (bsc == g_block_seq))
  {
    resp[0] = (uint8_t)(UDS_SID_TRANSFER_DATA + UDS_POSITIVE_RESPONSE_OFFSET);
    resp[1] = bsc;
    can_proto_send_response(resp, 2);
    return;
  }
  g_block_seq++;
  if (g_block_seq == 0U)
  {
    g_block_seq = 1U;
  }
  if (bsc != g_block_seq)
  {
    g_active = 0U;
    can_proto_send_nrc(UDS_SID_TRANSFER_DATA, UDS_NRC_WRONG_BLOCK_SEQUENCE);
    return;
  }
  dlen = (uint16_t)(len - 2U);
  if ((g_bytes_written + g_pad_len + (uint32_t)dlen) > g_size)
  {
    g_active = 0U;
    can_proto_send_nrc(UDS_SID_TRANSFER_DATA, UDS_NRC_TRANSFER_DATA_ABORTED);
    return;
  }
  flash_unlock();
  if (program_words(&data[2], dlen) != 0U)
  {
    g_active = 0U;
    flash_lock();
    can_proto_send_nrc(UDS_SID_TRANSFER_DATA, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
    return;
  }
  flash_lock();
  resp[0] = (uint8_t)(UDS_SID_TRANSFER_DATA + UDS_POSITIVE_RESPONSE_OFFSET);
  resp[1] = bsc;
  can_proto_send_response(resp, 2);
  (void)can_driver_wait_tx_idle(20U);
}

void ota_dl_handle_transfer_exit(uint8_t *data, uint16_t len)
{
  uint8_t resp[1];

  (void)data;
  (void)len;
  if (!can_proto_in_programming())
  {
    can_proto_send_nrc(UDS_SID_TRANSFER_EXIT, UDS_NRC_CONDITIONS_NOT_CORRECT);
    return;
  }
  if (g_exit_pending != 0U)
  {
    can_proto_send_pending(UDS_SID_TRANSFER_EXIT);
    return;
  }
  if (g_active == 0U)
  {
    if (g_trial_ready != 0U)
    {
      resp[0] = (uint8_t)(UDS_SID_TRANSFER_EXIT + UDS_POSITIVE_RESPONSE_OFFSET);
      can_proto_send_response(resp, 1);
    }
    else
    {
      can_proto_send_nrc(UDS_SID_TRANSFER_EXIT, UDS_NRC_TRANSFER_DATA_ABORTED);
    }
    return;
  }
  if (!can_proto_security_unlocked())
  {
    can_proto_send_nrc(UDS_SID_TRANSFER_EXIT, UDS_NRC_SECURITY_ACCESS_DENIED);
    return;
  }
  g_active = 0U;
  g_exit_pending = 1U;
  can_proto_begin_long_op(UDS_SID_TRANSFER_EXIT);
}

void ota_dl_poll(void)
{
  uint8_t resp[1];

  if (g_exit_pending == 0U)
  {
    return;
  }
  g_exit_pending = 0U;

  flash_unlock();
  if (program_flush() != 0U)
  {
    flash_lock();
    can_proto_end_long_op();
    can_proto_send_nrc(UDS_SID_TRANSFER_EXIT, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
    return;
  }
  flash_lock();

  if ((g_expected_size != 0U) && (g_bytes_written != g_expected_size))
  {
    can_proto_end_long_op();
    can_proto_send_nrc(UDS_SID_TRANSFER_EXIT, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
    return;
  }
  if (verify_slot_image(g_base, g_size) != 0)
  {
    can_proto_end_long_op();
    can_proto_send_nrc(UDS_SID_TRANSFER_EXIT, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
    return;
  }
  if (commit_trial() != 0)
  {
    can_proto_end_long_op();
    can_proto_send_nrc(UDS_SID_TRANSFER_EXIT, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
    return;
  }
  g_erased = 0U;
  g_trial_ready = 1U;
  can_proto_end_long_op();
  resp[0] = (uint8_t)(UDS_SID_TRANSFER_EXIT + UDS_POSITIVE_RESPONSE_OFFSET);
  can_proto_send_response(resp, 1);
  (void)can_driver_wait_tx_idle(50U);
}
