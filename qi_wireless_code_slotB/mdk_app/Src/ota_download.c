/**
  **************************************************************************
  * @file     ota_download.c
  * @brief    Write the inactive APP slot via UDS 0x31/0x34/0x36/0x37.
  *           0x37 验签并 commit_trial 成功后自行 NVIC_SystemReset，
  *           Boot 按 trial PENDING 切槽；主机 11 01 保留为旧 APP
  *           兼容与复位未生效时的补发手段。
  **************************************************************************
  */

#include "ota_download.h"
#include "ota_trigger.h"
#include "can_protocol.h"
#include "can_driver.h"
#include "sit1145.h"
#include "timer_drv.h"
#include "lifecycle.h"
#include "device_info.h"
#include "sha256.h"
#include "uECC.h"
#include "at32f422_426_flash.h"
#include "at32f422_426.h"
#include <string.h>

#define FLASH_PHYSICAL_END  0x08020000U
#define VERIFY_CHUNK        256U

/* 擦除路径有界等待预算（TC-1307 擦除挂死加固，Branch-B）：
 * 库 flash_sector_erase 内部 ERASE_TIMEOUT=0x40000000 次轮询≈无界
 * （at32f422_426_flash.h:152）——flash BUSY 楔死时关中断窗口内
 * SysTick 停走、全树无看门狗，整机挂死只能断电恢复
 * （取证报告 /mnt/k/ota_erase_forensics_0919.md §2c）。
 * 预算推导：flash_operation_wait_for 单次轮询为数十 ns 量级（对照库
 * PROGRAMMING_TIMEOUT=0x100000 次覆盖字编程 µs 级典型耗时的配比），
 * 单扇区典型擦除 ~25ms ≈ 1M 次轮询，4M ≈ 4× 余量（≈单扇区典型耗时
 * ×4 的设计目标；关中断窗口内 tick 停走，只能用迭代上限）。健康路径
 * 擦完即返回，不受预算影响；楔死路径从"无界挂死"变为有界超时→
 * NRC 0x72 可观测失败。 */
#define OTA_ERASE_POLLS_MAX  4000000U

/* 擦除路径局部有界擦除：与库 flash_sector_erase 相同的寄存器序列，
 * 仅把无界 ERASE_TIMEOUT 换成调用方给定的轮询上限。库函数本身不动，
 * 其他调用路径语义不变。 */
static flash_status_type flash_sector_erase_bounded(uint32_t sector_address,
                                                    uint32_t max_polls)
{
  flash_status_type status;

  FLASH->ctrl_bit.secers = TRUE;
  FLASH->addr = sector_address;
  FLASH->ctrl_bit.erstr = TRUE;
  status = flash_operation_wait_for(max_polls);
  FLASH->ctrl_bit.secers = FALSE;
  return status;
}

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

static uint8_t s_di_pubkey[DEVICE_INFO_PUBKEY_LEN];

static const uint8_t *image_pubkey(void)
{
  device_info_t di;

  if (device_info_read(&di) == 0)
  {
    if ((di.pubkey_valid == 0x01U) && (di.ecdsa_pubkey[0] == 0x04U))
    {
      memcpy(s_di_pubkey, di.ecdsa_pubkey, DEVICE_INFO_PUBKEY_LEN);
      return s_di_pubkey;
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

  g_trial_ready = 0U;
  ota_dl_abort();
  g_slot = inactive_slot();
  g_base = slot_base(g_slot);
  g_size = OTA_APP_A_SIZE;

  if (g_base == ota_running_slot_base())
  {
    can_proto_send_nrc(UDS_SID_ROUTINE_CONTROL, UDS_NRC_CONDITIONS_NOT_CORRECT);
    return;
  }
  if ((g_base < OTA_APP_A_BASE_ADDR) ||
      ((g_base + g_size) > FLASH_PHYSICAL_END) ||
      ((g_base + g_size) > OTA_META_PRIMARY_ADDR))
  {
    can_proto_send_nrc(UDS_SID_ROUTINE_CONTROL, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
    return;
  }

  can_proto_begin_long_op(UDS_SID_ROUTINE_CONTROL);
  for (addr = g_base; addr < (g_base + g_size); addr += OTA_FLASH_SECTOR_SIZE)
  {
    flash_status_type st;

    /* Single-bank: IRQ fetch during sector erase hardfaults / wedges CAN. */
    __disable_irq();
    flash_unlock();
    st = flash_sector_erase_bounded(addr, OTA_ERASE_POLLS_MAX);
    flash_lock();
    __enable_irq();
    if (st != FLASH_OPERATE_DONE)
    {
      /* 有界超时/擦除错误路径：flash_lock+__enable_irq 已在上方无条件
       * 完成；can_proto_end_long_op 做 CAN offline/online 恢复（不清
       * 回调）后回 NRC 0x72 generalProgrammingFailure，可观测失败。
       * 幂等不变：脚本 _erase_with_retry 重试重擦已擦槽安全；活跃槽
       * 防护（上方 g_base==ota_running_slot_base() → NRC 0x22）不动。 */
      can_proto_end_long_op();
      can_proto_send_nrc(UDS_SID_ROUTINE_CONTROL, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
      return;
    }
    can_proto_pump_long_op();
  }

  /* 循环结束后先泵一帧 0x78 再进 metadata 双副本擦写：尾部这段没有任何
   * 帧发出，若恰逢 bus-off 恢复或擦写偏慢，主机易在此判超时。 */
  can_proto_pump_long_op();

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
    /* metadata 落盘的关中断保护已下沉到 ota_trigger.c meta_write_to_flash
     * 咽喉点（erase+program 全覆盖）。此处不再外层包裹：__enable_irq
     * 无条件清 PRIMASK，双层包裹会在内层返回时提前开中断，名存实亡。
     * 上方泵帧 0x78 保持在落盘之前，长操作静默段规则不变。 */
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
  /* 切槽激活设计：0x37 收尾在 verify_slot_image + commit_trial 全部成功
   * 之后 APP 自行复位——commit_trial 已把 trial PENDING metadata 先备
   * 后主落盘（ota_trigger.c meta_write_to_flash 咽喉点），Boot 复位后按
   * trial PENDING 选槽跳转新 APP。主机 11 01 保留：旧 APP（无自复位
   * 逻辑）兼容 + 应答丢失/复位未生效时脚本补发手段
   * （can_protocol.c handle_ecu_reset）。
   * 响应必须先落总线再复位：wait_tx_idle 等 0x77 发送完成，SHUTDOWN
   * 生命周期帧同理等 TX 空闲后才 NVIC_SystemReset。
   * 0x37 重试幂等性：复位后重试帧落在重启后的新 APP（默认会话，
   * handler 回 NRC 0x22）；g_trial_ready 分支仍保护复位前窗口内的重复
   * 0x37（直接回正响应，不重复 commit）。 */
  (void)can_driver_wait_tx_idle(50U);
  lifecycle_set_state(LIFECYCLE_SHUTDOWN);
  (void)can_driver_wait_tx_idle(20U);
  NVIC_SystemReset();
}
