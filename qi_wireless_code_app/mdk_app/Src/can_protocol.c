/**
  **************************************************************************
  * @file     can_protocol.c
  * @brief    CAN UDS protocol handler for APP firmware
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
#include "can_protocol.h"
#include "can_driver.h"
#include "ota_trigger.h"
#include "ota_download.h"
#include "isotp.h"
#include "timer_drv.h"
#include "lifecycle.h"
#include "device_info.h"
#include "board_gpio.h"
#include "qi_protocol.h"
#include "nvm_drv.h"
#include "sha256.h"
#include "uECC.h"
#include "sit1145.h"
#include "at32f422_426_can.h"
#include <string.h>

/* ========================================================================== */
/*  Version string constants (UTF-8, max 16 bytes including null terminator)  */
/*                                                                            */
/*  SW_VERSION_STR 是运行软件版本的【唯一真相源】：UDS DID 0xF195 应答直接  */
/*  取此 APP 编译时常量，不读 OTA metadata / XATO 镜像头（2026-09-18 起：   */
/*  镜像头 version 区打包固定 0x00，不携带版本号；metadata 无软件版本字段）。 */
/*  改版本号只改本常量 + docs 文档，无打包脚本联动环节。                     */
/* ========================================================================== */

static const char SW_VERSION_STR[]     = "QC_JYF_FW_1.1.1";   /*!< 运行版本唯一真相源 */
static const char BOOTLOADER_VER_STR[] = "QC_JYF_BL_1.0.0";
static const char HW_VERSION_STR[]     = "QC_JYF_HW_1.1.5";

/* same public key as Bootloader boot_verify.c */
const uint8_t g_app_ecdsa_pubkey[65] = {
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

#define SECURITY_LOCKOUT_MS    30000U
#define SECURITY_MAX_FAILURES  3U

static void session_reset_to_default(void);

static uint8_t  current_session           = SESSION_DEFAULT;
static uint8_t  security_unlocked         = 0;
static uint32_t last_tester_present_tick  = 0;
static uint8_t  g_seed_generated          = 0;
static uint8_t  g_seed[32];
static uint8_t  g_seed_sub                = 0;
static uint8_t  g_security_fail_count     = 0;
static uint32_t g_security_lockout_until_ms = 0;
static uint8_t  g_sa_sig_buf[64];
static uint8_t  g_sa_sig_bytes_received   = 0;
static uint8_t  g_sa_sig_block_seq        = 0;

/* Qi IAP state tracking */
#define QI_IAP_IDLE         0x00U
#define QI_IAP_IN_PROGRESS  0x01U
#define QI_IAP_SUCCESS      0x02U
#define QI_IAP_FAILED       0x03U
#define QI_IAP_WAIT_ACK     0x04U   /*!< waiting for Qi chip UART ACK */

/* Qi IAP auto-complete timeout after last data packet sent */
#define QI_IAP_DONE_TIMEOUT_MS  3000U

/** @brief  Qi chip UART ACK timeout (ms) for each data packet (flash write) */
#define QI_IAP_ACK_TIMEOUT_MS      2000U
/** @brief  Qi chip prepare/erase timeout (ms) for DID 0x2130 start */
#define QI_IAP_PREPARE_TIMEOUT_MS  2500U
#define QI_VER_QUERY_TIMEOUT_MS    500U   /*!< DID 0x2013 Qi 版本查询回复超时 */

static uint32_t g_qi_iap_last_tx_ms = 0U;

static uint8_t  g_qi_iap_state    = QI_IAP_IDLE;
static uint8_t  g_qi_iap_progress = 0U;
static uint16_t g_qi_iap_total    = 0U;
static uint16_t g_qi_iap_sent     = 0U;
static uint16_t g_qi_fw_version   = 0U;     /*!< Qi 版本缓存：DID 0x2133/0x2013 读出源；由 0x01 上报解析或 0x2013 主动问询回复更新 */

/** @brief  deferred UDS response while waiting for Qi chip ACK */
static uint32_t g_qi_iap_wait_start_ms = 0U; /*!< timestamp when WAIT_ACK entered */
static uint8_t  g_qi_iap_pending_did[2] = {0U}; /*!< DID bytes for deferred response */
static uint32_t g_qi_iap_ack_timeout_ms = QI_IAP_ACK_TIMEOUT_MS;
static uint16_t g_qi_iap_pending_chunk = 0U; /*!< bytes to add to sent after ACK */

/** @brief  DID 0x2013 Qi 版本主动问询（延迟应答）状态 */
static uint8_t  g_qi_ver_q_state    = 0U;   /*!< 0=idle, 1=waiting Qi 回复 */
static uint32_t g_qi_ver_q_start_ms = 0U;

/* ========================================================================== */
/*  Qi charging state variables                                              */
/* ========================================================================== */

static uint8_t  g_qi_charger_enable   = 0U;     /*!< DID 0x2101: volatile, reset to 0 */
static uint8_t  g_qi_charge_state     = QI_CHARGE_DISABLED;  /*!< DID 0x2102 */
static uint8_t  g_qi_device_present   = 0U;     /*!< DID 0x2103 */
static uint16_t g_qi_output_power_mw  = 0U;     /*!< DID 0x2104, mW */
static uint8_t  g_qi_voltage_raw      = 0U;     /*!< DID 0x2105 byte 0 */
static uint8_t  g_qi_current_raw      = 0U;     /*!< DID 0x2105 byte 1 */
static uint8_t  g_qi_pcb_temp         = 0U;     /*!< DID 0x2108, PCB temperature ℃ */
static uint8_t  g_qi_fod_status       = 0U;     /*!< DID 0x2109 */
static uint8_t  g_qi_fault_code       = 0U;     /*!< DID 0x210B */
static uint8_t  g_qi_thermal_derate   = 100U;   /*!< DID 0x210C, 100%=no derating */
static uint8_t  g_qi_last_fault_detail[4] = {0U};  /*!< DID 0x2110, 4-byte fault detail */

/* Qi persistent config (loaded from NVM at init) */
static uint16_t g_qi_power_limit_mw   = 1500U;  /*!< DID 0x210D, 1500 mW = 15W default */

/** @brief  SIT1145 Normal + CAN online. Power-on default is Standby. */
static uint8_t  g_can_awake = 0;
static uint8_t  g_need_lifecycle_announce = 0;
static uint32_t g_uds_last_ms = 0;
static uint32_t g_standby_since_ms = 0;
static uint32_t g_announce_due_ms = 0;
static uint8_t  g_lp_ever_standby = 0;
static uint8_t  g_lp_wup_count = 0;
static uint8_t  g_lp_woke_from_standby = 0;
static uint8_t  g_lp_last_wake_src = 0;
static uint16_t g_lp_last_standby_sec = 0;
/** OTA trial 推迟到 __enable_irq() 之后再 enter_normal：harvest/wait_cts 依赖 SysTick */
static uint8_t  g_lp_need_online = 0;

/** ignore self-wake for a short window after entering Standby */
#define CAN_LP_WAKE_INHIBIT_MS  100U
/** send BOOTUP after UDS has a chance to ACK/reply the wake frame */
#define CAN_LP_ANNOUNCE_DELAY_MS  100U
/** after CAN online, spin-poll RX so host hardware retransmit of 10 01 can be ACKed */
#define CAN_LP_RX_HARVEST_MS      30U

/** 180 s with no UDS RX/TX → SIT1145 Standby (ISO 11898-2 WUP can wake)
 *  受 CAN_LP_STANDBY_ENABLE 总开关控制：开关=0 时该超时路径整段不编译 */
#define CAN_LP_IDLE_TIMEOUT_MS  (180UL * 1000UL)

static void can_lp_mark_uds(void)
{
  g_uds_last_ms = timer_get_tick();
}

static uint8_t can_lp_trial_needs_normal(void)
{
  ota_metadata_t meta;

  if (ota_metadata_read(&meta) != 0)
  {
    return 0U;
  }

  /* single-App arch (OTA-ARCH-0920): keep CAN online while a download
     is active or a pending backup copy failed validation — host must be
     able to probe the device in either state */
  if (meta.ota_state == OTA_STATE_DOWNLOADING)
  {
    return 1U;
  }
  if (meta.backup_valid != 0U)
  {
    return 1U;
  }

  /* after a failed backup copy the old APP boots with last_boot_reason
     == COPY_FAIL: keep CAN online so the host sees the device state,
     otherwise the old APP enters Standby and probe frames die as WUP */
  if (meta.last_boot_reason == OTA_BOOT_REASON_ROLLBACK)
  {
    return 1U;
  }

  return 0U;
}

static uint8_t g_lp_ident_sent;

static void can_lp_tx_marker(uint8_t b0, uint8_t b2, uint8_t b3,
                             uint8_t b4, uint8_t b5, uint8_t b6, uint8_t b7)
{
  uint8_t d[8];

  memset(d, 0, sizeof(d));
  d[0] = b0;
  d[1] = 0x41U;
  d[2] = b2;
  d[3] = b3;
  d[4] = b4;
  d[5] = b5;
  d[6] = b6;
  d[7] = b7;
  (void)can_driver_send(CAN_ID_LIFECYCLE_BROADCAST, d, 8);
  (void)can_driver_wait_tx_idle(20U);
}

/** 识别帧打到 0x18FF260D。harvest 结束只能走这条，避免抢在 50 01 前面占 UDS ID */
static void can_lp_send_ident_bus(void)
{
  if (g_lp_woke_from_standby != 0U)
  {
    can_lp_tx_marker(LIFECYCLE_BOOTUP, 0x57U, 0x4BU, g_lp_wup_count,
                     g_lp_last_wake_src,
                     (uint8_t)(g_lp_last_standby_sec & 0xFFU),
                     (uint8_t)((g_lp_last_standby_sec >> 8) & 0xFFU));
  }
  else
  {
    can_lp_tx_marker(LIFECYCLE_BOOTUP, 0U, 0U, 0U, 0U, 0U, 0U);
  }
  g_need_lifecycle_announce = 0U;
}

/** 50 01 之后再发：18FF260D + UDS 响应 ID 上的 ISO-TP SF（01 41 …） */
static void can_lp_send_ident(void)
{
  uint8_t uds[8];
  uint8_t i;

  can_lp_send_ident_bus();

  for (i = 0U; i < 8U; i++)
  {
    uds[i] = 0xCCU;
  }

  if (g_lp_woke_from_standby != 0U)
  {
    uds[0] = 0x07U;
    uds[1] = LIFECYCLE_BOOTUP;
    uds[2] = 0x41U;
    uds[3] = 0x57U;
    uds[4] = 0x4BU;
    uds[5] = g_lp_wup_count;
    uds[6] = g_lp_last_wake_src;
    uds[7] = (uint8_t)(g_lp_last_standby_sec & 0xFFU);
  }
  else
  {
    uds[0] = 0x03U;
    uds[1] = LIFECYCLE_BOOTUP;
    uds[2] = 0x41U;
    uds[3] = 0x00U;
  }

  (void)can_driver_send(CAN_PROTO_UDS_RESPONSE, uds, 8);
  (void)can_driver_wait_tx_idle(20U);
  g_lp_ident_sent = 1U;
}

static void can_lp_enter_normal(void)
{
  uint8_t retry;
  uint32_t t0;
  uint32_t now;
  uint32_t dur_sec;

  if (g_can_awake != 0U)
  {
    return;
  }

  now = timer_get_tick();
  if (g_lp_ever_standby != 0U)
  {
    dur_sec = (now - g_standby_since_ms) / 1000U;
    if (dur_sec > 0xFFFFU)
    {
      dur_sec = 0xFFFFU;
    }
    g_lp_last_standby_sec = (uint16_t)dur_sec;
    if (g_lp_wup_count < 0xFFU)
    {
      g_lp_wup_count++;
    }
    g_lp_woke_from_standby = 1U;
  }
  else
  {
    g_lp_woke_from_standby = 0U;
  }

  /* TXD 必须先回到 CAN AF，再切 Normal */
  can_driver_pins_active();

  for (retry = 0U; retry < 3U; retry++)
  {
    if (sit1145_normal_mode_set() != 0U)
    {
      break;
    }
    t0 = timer_get_tick();
    while ((timer_get_tick() - t0) < 10U) { __NOP(); }
  }

  /* 清 CW，等 RXD 从唤醒强制低恢复成隐性，再开 CAN，否则会 bus-off */
  sit1145_wakeup_clear();
  t0 = timer_get_tick();
  while ((timer_get_tick() - t0) < 5U)
  {
    if (gpio_input_data_bit_read(GPIOA, GPIO_PINS_11) != RESET)
    {
      break;
    }
  }

  can_driver_online();
  g_can_awake = 1U;
  can_lp_mark_uds();
  g_lp_ident_sent = 0U;
  g_need_lifecycle_announce = 1U;
  g_announce_due_ms = timer_get_tick() + CAN_LP_ANNOUNCE_DELAY_MS;

  /* Standby 下第一帧只当 WUP，MCU 收不到。主机 CAN 控制器会无 ACK 重发，
   * 这里空转收 RX，赶在重发窗口内 ACK 并回 50 01；quiet 期内禁止 BOOTUP。
   * trial 上电不是 WUP，且 SysTick 未跑时 harvest 会死等，跳过。 */
  if (g_lp_ever_standby != 0U)
  {
    t0 = timer_get_tick();
    while ((timer_get_tick() - t0) < CAN_LP_RX_HARVEST_MS)
    {
      can_driver_poll();
    }
  }

  if (g_lp_ident_sent == 0U)
  {
    /* 10 01 若还在重发路上，UDS ID 必须留给 50 01 */
    can_lp_send_ident_bus();
  }
}

/* Standby 进入函数：受 CAN_LP_STANDBY_ENABLE 总开关控制（can_protocol.h）。
 * 开关=0（回归调试期临时禁用）时不编译，源码完整保留，唤醒/恢复路径不受影响；
 * 生产恢复改 1 后本段与历史实现逐字节一致。 */
#if (CAN_LP_STANDBY_ENABLE != 0U)
static void can_lp_hold_standby(void)
{
  /* 先关 MCU CAN、再切收发器 Standby，最后才改 GPIO。
   * 若还在 Normal 就把 TXD 改成 GPIO，会在总线上打出显性。 */
  can_driver_offline();
  sit1145_wake_enable();
  (void)sit1145_standby_mode_set();
  sit1145_wakeup_clear();
  can_driver_pins_standby();
  g_can_awake = 0U;
  g_standby_since_ms = timer_get_tick();
  g_lp_ever_standby = 1U;
}

static void can_lp_enter_standby(void)
{
  if (g_can_awake != 0U)
  {
    (void)can_driver_wait_tx_idle(20U);
    session_reset_to_default();
    /* 进睡前打 06 41 53 42，总线上先看到 SB 再静音，才能确认真进了 Standby */
    can_lp_tx_marker(LIFECYCLE_SHUTDOWN, 0x53U, 0x42U, g_lp_wup_count, 0U, 0U, 0U);
  }
  can_lp_hold_standby();
}
#endif /* CAN_LP_STANDBY_ENABLE */

/* ========================================================================== */
/*  Private helper functions                                                 */
/* ========================================================================== */

/**
 * @brief  send a UDS response frame
 * @param  data: pointer to response data
 * @param  len: data length
 * @retval none
 */
#define PROTO_TX_PEND_MAX  256U
static uint8_t  g_tx_pend[PROTO_TX_PEND_MAX];
static uint16_t g_tx_pend_len = 0U;

static void proto_flush_pending_tx(void)
{
  uint16_t n = g_tx_pend_len;

  if (n == 0U)
  {
    return;
  }
  g_tx_pend_len = 0U;
  (void)isotp_tx_send(CAN_PROTO_UDS_RESPONSE, g_tx_pend, n);
}

static void proto_send_response(uint8_t *data, uint16_t len)
{
  can_lp_mark_uds();
  if ((data == (uint8_t *)0) || (len == 0U) || (len > PROTO_TX_PEND_MAX))
  {
    return;
  }
  /* SF can send from RX callback. MF must wait for Flow Control — if we
   * block inside can_driver_poll's callback, FC sits in the same FIFO
   * and N_Bs times out (27 01 34-byte seed looks like "no response"). */
  if (len <= 7U)
  {
    (void)isotp_tx_send(CAN_PROTO_UDS_RESPONSE, data, len);
    return;
  }
  memcpy(g_tx_pend, data, len);
  g_tx_pend_len = len;
}

/**
 * @brief  send UDS negative response
 * @param  service_id: the rejected service ID
 * @param  nrc: negative response code
 * @retval none
 */
static void proto_send_nrc(uint8_t service_id, uint8_t nrc)
{
  uint8_t resp[3];
  resp[0] = UDS_NEGATIVE_RESPONSE;
  resp[1] = service_id;
  resp[2] = nrc;
  proto_send_response(resp, 3);
}

static uint8_t  g_long_op_sid;
static uint32_t g_long_op_last_pending_ms = 0U;

/** @brief  P2* 补发阈门：距上次 0x78 超过此时长才允许补发下一帧 */
#define LONG_OP_PENDING_REFRESH_MS  4500U

/**
 * @brief  recover CAN after Flash stall (IRQ may have missed bus-off)
 */
static void proto_can_busoff_recover(void)
{
  uint32_t start;
  uint8_t n;

  /* 250kbps 下 bus-off 恢复需 128×11 个隐性位（≈5.6ms 总线时间）；
   * 单 Bank Flash 擦除 stall 后控制器状态复位更慢，原 3×10ms 窗口
   * 系统性不足，导致每次都掉进下面的兜底分支。5×20ms 覆盖最坏情况。 */
  for (n = 0U; n < 5U; n++)
  {
    if (can_busoff_get(CAN1) == RESET)
    {
      return;
    }
    can_busoff_reset(CAN1);
    start = timer_get_tick();
    while ((timer_get_tick() - start) < 20U)
    {
      if (can_busoff_get(CAN1) == RESET)
      {
        return;
      }
    }
  }
  /* 恢复失败时禁止走 can_driver_init()：那是上电初始化路径，会清掉
   * rx_callback/busoff_recovery_cb 并把 CAN 停在 software reset 等待
   * can_driver_online()——0x31 擦槽循环中一旦触发，本函数后续泵出的
   * 0x78 与尾部 0x71 正响应全部黑洞；返回主循环后 RX 回调已丢，
   * 设备 UDS 永久失聪，只有断电才能恢复（06:52 擦除 55s 全静默根因）。
   * offline→online 只复位 CAN 外设+清 pending+重挂 RX/ERR 中断，
   * rx_callback/busoff_recovery_cb 保持不变，长操作路径可自愈。 */
  can_driver_offline();
  can_driver_online();
}

/**
 * @brief  NRC 0x78 as a raw ISO-TP SF (do not use isotp_tx_send; it can block 1s)
 */
static void proto_send_pending(uint8_t service_id)
{
  uint8_t sf[8];
  sf[0] = 0x03U;
  sf[1] = UDS_NEGATIVE_RESPONSE;
  sf[2] = service_id;
  sf[3] = UDS_NRC_RESPONSE_PENDING;
  sf[4] = 0xCCU;
  sf[5] = 0xCCU;
  sf[6] = 0xCCU;
  sf[7] = 0xCCU;
  (void)can_driver_send(CAN_PROTO_UDS_RESPONSE, sf, 8);
}

static void proto_begin_long_op(uint8_t service_id)
{
  g_long_op_sid = service_id;
  proto_can_busoff_recover();
  (void)sit1145_normal_mode_set();
  proto_send_pending(service_id);
  g_long_op_last_pending_ms = timer_get_tick();
  (void)can_driver_wait_tx_idle(50U);
}

static void proto_end_long_op(void)
{
  proto_can_busoff_recover();
  (void)sit1145_normal_mode_set();
  (void)can_driver_wait_tx_idle(50U);
}

void can_proto_pump_long_op(void)
{
  /* 时间闸门：仅在距上次 0x78 接近 P2*（4500ms）时才补发，
   * 防止擦除循环/主循环以毫秒级频率洪泛 NRC 0x78（UDS 规范：
   * 首次 0x78 后应在 P2* 内返回最终响应，仅当处理将再次
   * 超过 P2* 时才需再次发送 0x78）。 */
  if ((timer_get_tick() - g_long_op_last_pending_ms) < LONG_OP_PENDING_REFRESH_MS)
  {
    return;
  }
  proto_can_busoff_recover();
  (void)sit1145_normal_mode_set();
  proto_send_pending(g_long_op_sid);
  g_long_op_last_pending_ms = timer_get_tick();
  (void)can_driver_wait_tx_idle(20U);
}

void can_proto_send_response(uint8_t *data, uint16_t len)
{
  proto_send_response(data, len);
}

void can_proto_send_nrc(uint8_t service_id, uint8_t nrc)
{
  proto_send_nrc(service_id, nrc);
}

void can_proto_begin_long_op(uint8_t service_id)
{
  proto_begin_long_op(service_id);
}

void can_proto_end_long_op(void)
{
  proto_end_long_op();
}

void can_proto_send_pending(uint8_t service_id)
{
  proto_send_pending(service_id);
}

uint8_t can_proto_security_unlocked(void)
{
  return security_unlocked;
}

uint8_t can_proto_in_programming(void)
{
  return (current_session == SESSION_PROGRAMMING) ? 1U : 0U;
}

/**
 * @brief  reset session to default and clear security state
 * @note   called on session timeout or switch to default session
 * @retval none
 */
static void session_reset_to_default(void)
{
  current_session   = SESSION_DEFAULT;
  security_unlocked = 0;
  g_seed_generated  = 0;
  g_seed_sub        = 0;
  g_tx_pend_len     = 0U;
  ota_dl_abort();
}

/**
 * @brief  handle session switch rules per spec section 7.3
 * @param  new_session: requested session type
 * @retval none
 */
static void session_switch(uint8_t new_session)
{
  if (new_session == SESSION_DEFAULT)
  {
    /* entering default: abort any firmware transfer, clear security */
    /* (OTA transfer is not active in APP, but clear state anyway) */
    session_reset_to_default();
  }
  else if (current_session != SESSION_DEFAULT && new_session != current_session)
  {
    /* non-default -> different non-default: clear security */
    security_unlocked = 0;
    current_session = new_session;
  }
  else
  {
    /* default -> non-default, or same session: just update */
    current_session = new_session;
  }
}


/**
 * @brief  copy string into response buffer (including null terminator)
 * @param  dst: destination buffer
 * @param  src: source string
 * @retval number of bytes written (including null terminator)
 */
static uint32_t generate_random_seed(void)
{
  static uint32_t lfsr = 0xA5A5A5A5U;
  uint32_t tick = timer_get_tick();
  uint32_t bit;

  lfsr ^= tick;
  bit = ((lfsr >> 0) ^ (lfsr >> 1) ^ (lfsr >> 21) ^ (lfsr >> 31)) & 1U;
  lfsr = (lfsr >> 1) | (bit << 31);
  lfsr ^= (tick << 7) ^ (tick >> 13);
  return lfsr;
}

/* ========================================================================== */
/*  NVM persistence helpers for Qi config DIDs                               */
/* ========================================================================== */

/**
 * @brief  load persistent Qi config from NVM
 */
static void qi_nvm_load_config(void)
{
  uint8_t buf[8];

  if (nvm_drv_is_valid() != 0U)
  {
    if (nvm_drv_read(NVM_OFFSET_POWER_LIMIT, buf, 2U) == NVM_STATUS_OK)
    {
      uint16_t val = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
      if ((val == 500U) || (val == 1000U) || (val == 1500U))
      {
        g_qi_power_limit_mw = val;
      }
    }
  }
}

/**
 * @brief  save one persistent Qi config field to NVM
 * @param  offset: NVM offset
 * @param  data: pointer to data
 * @param  len: data length
 * @retval 0=ok, -1=error
 */
static int8_t qi_nvm_save(uint16_t offset, const uint8_t *data, uint16_t len)
{
  if (nvm_drv_write(offset, (uint8_t *)data, len) != NVM_STATUS_OK)
  {
    return -1;
  }
  return 0;
}

static int8_t fill_did_payload(uint16_t did, uint8_t *out, uint8_t *olen)
{
  device_info_t di;

  switch (did)
  {
    case DID_SW_VERSION:
    {
      /* 版本唯一真相源 = APP 编译时常量 SW_VERSION_STR（本文件顶部）。
       * 不再从 OTA metadata / XATO 镜像头 version 字段取值：metadata 会被
       * trial/rollback/defaults 重建改写，双副本全坏时会被默认值破坏性覆盖；
       * 镜像头 version 仅保留镜像标识/打包校验用途，不代表运行代码版本。 */
      device_info_pad32(out, SW_VERSION_STR);
      *olen = 32U;
      return 0;
    }
    case DID_BOOTLOADER_VERSION:
      device_info_pad32(out, BOOTLOADER_VER_STR);
      *olen = 32U;
      return 0;
    case DID_HW_VERSION:
      if (device_info_read(&di) == 0)
      {
        device_info_pad32(out, di.hw_version);
      }
      else
      {
        device_info_pad32(out, HW_VERSION_STR);
      }
      *olen = 32U;
      return 0;
    case DID_SERIAL_NUMBER:
      if (device_info_read(&di) != 0)
      {
        return -1;
      }
      device_info_pad32(out, di.sn);
      *olen = 32U;
      return 0;
    case DID_FW_TYPE:
      out[0] = FW_TYPE_APP;
      *olen = 1U;
      return 0;
    case DID_OTA_STATE:
    {
      ota_metadata_t meta;
      uint8_t status;
      if (ota_metadata_read(&meta) != 0U)
      {
        out[0] = 0xFFU;
        *olen = 1U;
        return 0;
      }
      /* OTA Status DID 0x2112 八状态定义:
       * 0x00 Idle               无 OTA 操作，无待报告结果
       * 0x01 Downloading        正在下载固件到备份区
       * 0x02 Validating         传输完成，正在验证（CRC/签名）
       * 0x03 Pending Activation 验证成功，等待 11 01 激活重启
       * 0x04 Trial Boot         新 APP 启动未确认（单 App 架构不适用）
       * 0x05 Confirmed          OTA 搬运成功，新固件已生效
       * 0x06 Rolled Back        搬运失败，已回滚到旧固件
       * 0x07 Failed             下载或验证失败，旧固件保留 */
      if (meta.ota_state == OTA_STATE_DOWNLOADING)
      {
        status = 0x01U;  /* Downloading */
      }
      else if (meta.backup_valid != 0U)
      {
        status = 0x03U;  /* Pending Activation */
      }
      else if (meta.last_boot_reason == 0x03U)
      {
        status = 0x05U;  /* Confirmed: OTA 搬运成功 */
      }
      else if (meta.last_boot_reason == 0x04U)
      {
        status = 0x06U;  /* Rolled Back: 搬运失败回滚 */
      }
      else
      {
        status = 0x00U;  /* Idle */
      }
      out[0] = status;
      *olen = 1U;
      return 0;
    }
    case DID_ACTIVE_SLOT:
      out[0] = ota_running_slot();
      *olen = 1U;
      return 0;
    case DID_PENDING_SLOT:
    {
      ota_metadata_t meta;
      if (ota_dl_erased() != 0U)
      {
        out[0] = ota_dl_target_slot();
      }
      else
      {
        out[0] = (ota_metadata_read(&meta) == 0) ?
                 ((meta.backup_valid != 0U) ? 0x02U : 0xFEU) : 0xFEU;
      }
      *olen = 1U;
      return 0;
    }
    case DID_LAST_BOOT_REASON:
    {
      ota_metadata_t meta;
      out[0] = (ota_metadata_read(&meta) == 0) ? meta.last_boot_reason : 0xFFU;
      *olen = 1U;
      return 0;
    }
    case DID_ROLLBACK_COUNT:
    {
      ota_metadata_t meta;
      out[0] = (ota_metadata_read(&meta) == 0) ?
               (uint8_t)(meta.copy_retry_count & 0xFFU) : 0U;
      *olen = 1U;
      return 0;
    }
    case DID_CLAMP_STATE:
      /* PA0 low (magnetic field/phone present) → 0x00, PA0 high (no phone) → 0x01 */
      out[0] = (gpio_input_data_bit_read(GPIOA, GPIO_PINS_0) != RESET) ? 0x01U : 0x00U;
      *olen = 1U;
      return 0;
    case DID_SIT1145_LP_STATUS:
      /* [0] bit0=ever_standby bit1=last_wake_was_wup
       * [1] wup_count  [2-3] last_standby_sec LE */
      out[0] = (uint8_t)((g_lp_ever_standby != 0U) | ((g_lp_woke_from_standby != 0U) << 1) |
                         ((g_lp_last_wake_src & 0x0FU) << 4));
      out[1] = g_lp_wup_count;
      out[2] = (uint8_t)(g_lp_last_standby_sec & 0xFFU);
      out[3] = (uint8_t)((g_lp_last_standby_sec >> 8) & 0xFFU);
      *olen = 4U;
      return 0;
    case DID_ECDSA_PUBKEY:
    {
      if (device_info_read(&di) != 0)
      {
        return -1;
      }
      if (di.pubkey_valid != 0x01U)
      {
        return -1;
      }
      memcpy(out, di.ecdsa_pubkey, 65U);
      *olen = 65U;
      return 0;
    }
    case DID_CHARGER_CAPABILITY:
      out[0] = 15U;   /* max power 15W */
      out[1] = 0U;
      out[2] = 0U;
      out[3] = 0U;
      *olen = 4U;
      return 0;
    case DID_CHARGE_STATE:
      /* read PB2 actual level for charge state */
      if (gpio_output_data_bit_read(GPIOB, GPIO_PINS_2) != RESET)
        out[0] = QI_CHARGE_CHARGING;  /* PB2 high → CHARGING */
      else if (g_qi_charger_enable != 0U)
        out[0] = QI_CHARGE_STANDBY;   /* enabled but no device */
      else
        out[0] = QI_CHARGE_DISABLED;  /* disabled */
      *olen = 1U;
      return 0;
    case DID_DEVICE_PRESENT:
      out[0] = g_qi_device_present;
      *olen = 1U;
      return 0;
    case DID_OUTPUT_POWER:
      out[0] = (uint8_t)(g_qi_output_power_mw & 0xFFU);
      out[1] = (uint8_t)((g_qi_output_power_mw >> 8) & 0xFFU);
      *olen = 2U;
      return 0;
    case DID_INPUT_VI:
      out[0] = g_qi_voltage_raw;
      out[1] = g_qi_current_raw;
      *olen = 2U;
      return 0;
    case DID_INPUT_CURRENT:
    case DID_COIL_TEMP:
    case DID_ALIGNMENT:
      /* HW not supported */
      return -1;
    case DID_PCB_TEMP:
      out[0] = g_qi_pcb_temp;
      *olen = 1U;
      return 0;
    case DID_FOD_STATUS:
      out[0] = g_qi_fod_status;
      *olen = 1U;
      return 0;
    case DID_FAULT_CODE:
      out[0] = g_qi_fault_code;
      *olen = 1U;
      return 0;
    case DID_THERMAL_DERATE:
      out[0] = g_qi_thermal_derate;
      *olen = 1U;
      return 0;
    case DID_POWER_LIMIT:
      out[0] = (uint8_t)(g_qi_power_limit_mw & 0xFFU);
      out[1] = (uint8_t)((g_qi_power_limit_mw >> 8) & 0xFFU);
      *olen = 2U;
      return 0;
    case DID_LAST_FAULT_DETAIL:
      memcpy(out, g_qi_last_fault_detail, 4U);
      *olen = 4U;
      return 0;
    case 0x21FFU:
      out[0] = sit1145_get_mode();  /* 0x04=Standby, 0x07=Normal, 0x01=Sleep */
      *olen = 1U;
      return 0;
    case DID_QI_IAP_STATUS:
      /* [0]state [1]progress [2-3]Qi版本 LE [4-5]已发 LE [6-7]总长 LE */
      out[0] = (g_qi_iap_state == QI_IAP_WAIT_ACK) ? QI_IAP_IN_PROGRESS : g_qi_iap_state;
      out[1] = g_qi_iap_progress;
      out[2] = (uint8_t)(g_qi_fw_version & 0xFFU);
      out[3] = (uint8_t)((g_qi_fw_version >> 8) & 0xFFU);
      out[4] = (uint8_t)(g_qi_iap_sent & 0xFFU);
      out[5] = (uint8_t)((g_qi_iap_sent >> 8) & 0xFFU);
      out[6] = (uint8_t)(g_qi_iap_total & 0xFFU);
      out[7] = (uint8_t)((g_qi_iap_total >> 8) & 0xFFU);
      *olen = 8U;
      return 0;
    case DID_QI_FW_VERSION:
      out[0] = (uint8_t)(g_qi_fw_version & 0xFFU);
      out[1] = (uint8_t)((g_qi_fw_version >> 8) & 0xFFU);
      *olen = 2U;
      return 0;
    default:
      return -1;
  }
}

/* ========================================================================== */
/*  UDS service handlers                                                     */
/* ========================================================================== */

/**
 * @brief  DiagnosticSessionControl (0x10)
 * @param  data: UDS payload
 * @param  len:  payload length
 * @retval none
 */
static void handle_diag_session_ctrl(uint8_t *data, uint16_t len)
{
  uint8_t resp[8];
  uint8_t sub_func;
  uint8_t suppress;
  uint8_t session_type;

  if (len < 2U)
  {
    proto_send_nrc(UDS_SID_DIAG_SESSION_CTRL, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    return;
  }

  sub_func     = data[1];
  suppress     = sub_func & UDS_SUBFUNC_SUPPRESS_POS_RESP;
  session_type = sub_func & UDS_SUBFUNC_MASK;

  /* validate session type */
  if ((session_type != SESSION_DEFAULT) &&
      (session_type != SESSION_PROGRAMMING) &&
      (session_type != SESSION_EXTENDED))
  {
    proto_send_nrc(UDS_SID_DIAG_SESSION_CTRL, UDS_NRC_SUBFUNCTION_NOT_SUPPORTED);
    return;
  }

  /* perform session switch */
  session_switch(session_type);
  if (session_type == SESSION_PROGRAMMING)
  {
    board_5v_set(0U);
  }

  /* update tester present tick on session control */
  last_tester_present_tick = timer_get_tick();

  /* send positive response unless suppressed */
  if (!suppress)
  {
    uint16_t p2star_units;
    resp[0] = UDS_SID_DIAG_SESSION_CTRL + UDS_POSITIVE_RESPONSE_OFFSET;
    resp[1] = session_type;
    /* ISO 14229 sessionParameterRecord: P2 (1ms), P2* (10ms). Do not piggyback
     * LP wakeup flags here — CCU treats bytes 2..5 as timing and P2*=0
     * makes 7F xx 78 expire in ~5s with no retry. */
    resp[2] = (uint8_t)((UDS_P2_TIMEOUT_MS >> 8) & 0xFFU);
    resp[3] = (uint8_t)(UDS_P2_TIMEOUT_MS & 0xFFU);
    p2star_units = (uint16_t)(UDS_P2_STAR_TIMEOUT_MS / 10U);
    resp[4] = (uint8_t)((p2star_units >> 8) & 0xFFU);
    resp[5] = (uint8_t)(p2star_units & 0xFFU);
    proto_send_response(resp, 6U);
    if (session_type == SESSION_DEFAULT)
    {
      (void)can_driver_wait_tx_idle(20U);
      can_lp_send_ident();
    }
  }
}

/**
 * @brief  ECUReset (0x11)
 * @note   竞态修复后顺序（旧序：发 51 01 → SHUTDOWN 广播插队 → 笼统
 *         TX-idle 等待 20ms（返回值被忽略）→复位，广播帧抢占总线并
 *         污染状态判定，多 mailbox 场景下51 01 尚在仲裁/重发时即被
 *         误判完成，复位导致帧丢失→主机 P2 超时）：
 *           发 51 01 → 按 handle 等该帧真正发送完成（50ms；超时或未
 *           入队时补发一次再等一次）→ lifecycle SHUTDOWN 广播 → 按
 *           handle 等广播帧发送完成（50ms）→ 全发送缓冲空闲兜底
 *           （50ms）→ 保险延迟 3ms → NVIC_SystemReset()。
 *         两次 51 01 等待均失败（极端总线故障）仍继续复位：主机侧真实
 *         兜底是 zcanpro_ext_ota_auto.py uds_ecu_reset() 对 11 01 的
 *         ≤3 次重试（单次响应窗 response_timeout_ms=3000ms，失败间隔
 *         0.5s）+ 复位后 confirm_app_after_reset() 确认窗口（22 2113
 *         盲探→生命周期帧监听）+ 失败时判别矩阵与断电重启指引；
 *         主机侧不存在按 2113/2114 判定结果再次发起 11 01 的机制。
 *         时序如实：设备通告 P2=50ms（can_protocol.h UDS_P2_TIMEOUT_MS，
 *         ISO 14229 会话参数语义）与主机工具实际单次响应窗 3000ms 是
 *         两个层面，不再混用；本路径名义最坏 4×50ms+3ms=203ms，病理
 *         场景叠加 proto SF 路径 ISOTP_N_As=1000ms 窗×2（发送重试或
 *         发送后内部 TX-idle 等待）≈2.2s（isotp.h:72），均在主机工具
 *         单次响应窗内。两次失败仍复位的决策理由：设备侧无限阻塞只会
 *         挂死无信息，复位后 Boot 按已落盘 metadata 处理，主机确认
 *         窗口可得明确判别信息。suppress(0x80) 分支不发正响应，跳过
 *         对 51 01 的等待，只等 SHUTDOWN 广播 + 兜底 + 保险延迟。
 * @param  data: UDS payload
 * @param  len:  payload length
 * @retval none
 */
static void handle_ecu_reset(uint8_t *data, uint16_t len)
{
  uint8_t resp[8];
  uint8_t sub_func;
  uint8_t suppress;
  uint8_t h_before;
  uint8_t h_after;
  uint8_t h_bc0;
  uint8_t have_prev;
  uint32_t t0;

  if (len < 2U)
  {
    proto_send_nrc(UDS_SID_ECU_RESET, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    return;
  }

  sub_func = data[1] & UDS_SUBFUNC_MASK;
  suppress = data[1] & UDS_SUBFUNC_SUPPRESS_POS_RESP;

  if (sub_func != 0x01U)
  {
    proto_send_nrc(UDS_SID_ECU_RESET, UDS_NRC_SUBFUNCTION_NOT_SUPPORTED);
    return;
  }

  /* 切槽激活现状（与 ota_download.c ota_dl_poll 注释同步）：0x37 收尾
   * verify+commit_backup 成功后 APP 不自行复位（0e167e8 起；0x77 先落
   * 总线，g_trial_ready=1 防重复 commit），等主机发 11 01 才复位；
   * Boot 按 trial PENDING 切槽。本 handler 保留：①旧 APP（自复位架构
   * 版本）兼容；②51 01 应答丢失/复位未生效时，主机由
   * zcanpro_ext_ota_auto.py uds_ecu_reset() 重试 11 01 ≤3 次（单次窗
   * 3000ms），三次全超时不再补发，转入 confirm_app_after_reset()
   * 确认窗口判定，失败输出判别矩阵+断电重启指引。无论谁触发复位，
   * metadata 均已在 commit_backup 落盘（backup_valid flag），BOOT 搬运
   * 不受影响。 */

  if (!suppress)
  {
    resp[0] = UDS_SID_ECU_RESET + UDS_POSITIVE_RESPONSE_OFFSET;
    resp[1] = sub_func;

    /* 发送前后对比 handle：proto_send_response 经 ISO-TP SF →
     * can_driver_send 无返回值；若两路缓冲满且 N_As 窗口内仍失败，
     * 帧根本没入队，handle 不推进——据此识别静默失败，避免对陈旧
     * handle 空等得到假完成 */
    have_prev = (can_driver_last_tx_handle(&h_before) == 0) ? 1U : 0U;
    proto_send_response(resp, 2);

    if ((can_driver_last_tx_handle(&h_after) == 0) &&
        ((have_prev == 0U) || (h_after != h_before)))
    {
      /* 已入队：等 51 01 真正发送完成（按 handle 精确判定） */
      if (can_driver_wait_tx_frame(h_after, 50U) != 0)
      {
        /* 首次等待超时/终态失败：补发一次 51 01 再等一次。正常帧
         * 250kbps 下约 0.5ms 发完，走到这里说明总线持续繁忙或故障；
         * 两次都失败属极端场景，仍继续复位——主机侧真实兜底是
         * uds_ecu_reset() ≤3 次重试（3000ms/次）+确认窗口+断电指引，
         * 名义最坏 203ms/病理 ~2.2s 均在主机单次响应窗 3000ms 内，
         * 设备侧无限阻塞只会挂死无信息（详见 @note） */
        h_before = h_after;
        proto_send_response(resp, 2);
        if ((can_driver_last_tx_handle(&h_after) == 0) && (h_after != h_before))
        {
          (void)can_driver_wait_tx_frame(h_after, 50U);
        }
      }
    }
    else
    {
      /* 51 01 未入队（缓冲满且 isotp N_As 窗口内重试仍失败）：直接
       * 补发一次并等待；仍失败则放弃，理由同上 */
      have_prev = (can_driver_last_tx_handle(&h_before) == 0) ? 1U : 0U;
      proto_send_response(resp, 2);
      if ((can_driver_last_tx_handle(&h_after) == 0) &&
          ((have_prev == 0U) || (h_after != h_before)))
      {
        (void)can_driver_wait_tx_frame(h_after, 50U);
      }
    }
  }

  /* SHUTDOWN 广播移到 51 01 确认之后：不再插在响应与发送完成等待之间 */
  have_prev = (can_driver_last_tx_handle(&h_bc0) == 0) ? 1U : 0U;
  lifecycle_set_state(LIFECYCLE_SHUTDOWN);

  if ((can_driver_last_tx_handle(&h_after) == 0) &&
      ((have_prev == 0U) || (h_after != h_bc0)))
  {
    /* 广播帧已入队：按 handle 等其发送完成 */
    (void)can_driver_wait_tx_frame(h_after, 50U);
  }
  /* 广播被 lifecycle tx_ready 门禁跳过或入队失败（handle 未推进）：
   * 无帧可等，直接进入全缓冲空闲兜底 */

  /* 兜底：等全部发送资源空闲（CAST 无 TME 标志的等价判定，见
   * can_driver_wait_tx_all_idle 注释）——确认所有已入队帧均已离开
   * 发送缓冲再复位 */
  (void)can_driver_wait_tx_all_idle(50U);

  /* 复位前保险延迟 3ms：覆盖发送完成判定之外的极端窗口（末位总线
   * 传输、收发器传播延迟、TSTAT 更新滞后）。timer_drv 无独立阻塞
   * 延时接口，沿用工程既有 SysTick tick 轮询写法（同本文件
   * can_lp_enter_normal 的 tick 等待机制），不自造周期级忙等 */
  t0 = timer_get_tick();
  while ((timer_get_tick() - t0) < 3U)
  {
    __NOP();
  }

  NVIC_SystemReset();
}

/**
 * @brief  ReadDataByIdentifier (0x22)
 * @param  data: UDS payload
 * @param  len:  payload length
 * @retval none
 */
static void handle_read_data_by_id(uint8_t *data, uint16_t len)
{
  uint8_t resp[256];
  uint16_t pos;
  uint16_t i;

  if ((len < 3U) || (((len - 1U) % 2U) != 0U))
  {
    proto_send_nrc(UDS_SID_READ_DATA_BY_ID, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    return;
  }

  /* DID 0x2013 主动问询：UART 往返延迟走延迟应答（先 7F 22 78，Qi 回复后
   * 62 20 13 ver_lo ver_hi），不进同步 fill 路径。仅支持单独读；组合读
   * 回 NRC 0x22（延迟应答无法服务多 DID）。 */
  if ((len == 3U) &&
      ((((uint16_t)data[1] << 8) | (uint16_t)data[2]) == DID_QI_VERSION_QUERY))
  {
    if ((g_qi_ver_q_state != 0U) || (g_qi_iap_state != QI_IAP_IDLE))
    {
      /* 查询进行中不排队；Qi IAP 升级中避免 UART 命令交叉 */
      proto_send_nrc(UDS_SID_READ_DATA_BY_ID, UDS_NRC_CONDITIONS_NOT_CORRECT);
      return;
    }
    g_qi_ver_q_state    = 1U;
    g_qi_ver_q_start_ms = timer_get_tick();
    proto_send_pending(UDS_SID_READ_DATA_BY_ID);
    (void)qi_protocol_send(QI_CMD_VERSION_QUERY, (const uint8_t *)0, 0U, 1U);
    return;
  }

  resp[0] = UDS_SID_READ_DATA_BY_ID + UDS_POSITIVE_RESPONSE_OFFSET;
  pos = 1U;
  for (i = 1U; i < len; i += 2U)
  {
    uint16_t did = ((uint16_t)data[i] << 8) | (uint16_t)data[i + 1U];
    uint8_t payload[32];
    uint8_t plen = 0U;

    if (did == DID_QI_VERSION_QUERY)
    {
      /* 组合读含 0x2013：延迟应答无法服务 → NRC 0x22 */
      proto_send_nrc(UDS_SID_READ_DATA_BY_ID, UDS_NRC_CONDITIONS_NOT_CORRECT);
      return;
    }

    if (fill_did_payload(did, payload, &plen) != 0)
    {
      proto_send_nrc(UDS_SID_READ_DATA_BY_ID, UDS_NRC_REQUEST_OUT_OF_RANGE);
      return;
    }
    if ((pos + 2U + plen) > sizeof(resp))
    {
      proto_send_nrc(UDS_SID_READ_DATA_BY_ID, UDS_NRC_RESPONSE_TOO_LONG);
      return;
    }
    resp[pos++] = data[i];
    resp[pos++] = data[i + 1U];
    memcpy(&resp[pos], payload, plen);
    pos = (uint16_t)(pos + plen);
  }
  proto_send_response(resp, pos);
}

/**
 * @brief  WriteDataByIdentifier (0x2E)
 * @note   逐 DID 门禁（本文件 WDBI case 表，同值 NRC 判读以 case 块内
 *         检查顺序为准）：0x2101/0x210D 需 EXTENDED 会话（10 03）；
 *         0x2010/0xF18C/0x2120/0x2130/0x2131 需 PROGRAMMING 会话
 *         （10 02）；全部可写 DID 均需 SecurityAccess 解锁；其他 DID
 *         回 NRC 0x31。同一 case 内会话检查先于安全检查（NRC 0x22 在
 *         0x33 之前——写步拿 0x33=会话存活证据，拿 0x22 才是会话丢失）。
 * @param  data: UDS payload
 * @param  len:  payload length
 * @retval none
 */
static void handle_write_data_by_id(uint8_t *data, uint16_t len)
{
  uint8_t resp[4];
  uint16_t did;

  /* minimum: SID + DID_H + DID_L + 1 byte data */
  if (len < 4U)
  {
    proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    return;
  }

  did = ((uint16_t)data[1] << 8) | (uint16_t)data[2];

  /* check session & security per DID */
  switch (did)
  {
    /* Extended session + SA DIDs (Qi charger config) */
    case DID_CHARGER_ENABLE:
    case DID_POWER_LIMIT:
      if (current_session != SESSION_EXTENDED)
      {
        proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_CONDITIONS_NOT_CORRECT);
        return;
      }
      if (!security_unlocked)
      {
        proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_SECURITY_ACCESS_DENIED);
        return;
      }
      break;

    /* Programming session + SA DIDs */
    case DID_FW_TYPE:
    case DID_SERIAL_NUMBER:
    case DID_ECDSA_PUBKEY:
    case DID_QI_IAP_CONTROL:
    case DID_QI_IAP_DATA:
      if (current_session != SESSION_PROGRAMMING)
      {
        proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_CONDITIONS_NOT_CORRECT);
        return;
      }
      if (!security_unlocked)
      {
        proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_SECURITY_ACCESS_DENIED);
        return;
      }
      break;

    default:
      proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_REQUEST_OUT_OF_RANGE);
      return;
  }

  {
    switch (did)
    {
      case DID_FW_TYPE:
      {
        uint8_t fw_type = data[3];
        if ((fw_type < FW_TYPE_APP) || (fw_type > FW_TYPE_BOOTLOADER))
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_REQUEST_OUT_OF_RANGE);
          return;
        }
        resp[0] = UDS_SID_WRITE_DATA_BY_ID + UDS_POSITIVE_RESPONSE_OFFSET;
        resp[1] = data[1];
        resp[2] = data[2];
        proto_send_response(resp, 3);
        break;
      }

      case DID_SERIAL_NUMBER:
      {
        uint8_t sn32[32];
        uint16_t n;
        uint16_t i;

        n = (uint16_t)(len - 3U);
        if (n > 32U)
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_REQUEST_OUT_OF_RANGE);
          return;
        }
        memset(sn32, 0x20, 32U);
        for (i = 0U; i < n; i++)
        {
          sn32[i] = data[3U + i];
        }
        if (device_info_write_sn(sn32) != 0)
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
          return;
        }
        (void)sit1145_normal_mode_set();
        (void)can_driver_wait_tx_idle(50U);
        resp[0] = UDS_SID_WRITE_DATA_BY_ID + UDS_POSITIVE_RESPONSE_OFFSET;
        resp[1] = data[1];
        resp[2] = data[2];
        proto_send_response(resp, 3);
        (void)can_driver_wait_tx_idle(50U);
        break;
      }

      case DID_ECDSA_PUBKEY:
      {
        uint16_t n;

        n = (uint16_t)(len - 3U);
        if (n != 65U)
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
          return;
        }
        if (data[3] != 0x04U)
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_REQUEST_OUT_OF_RANGE);
          return;
        }
        if (device_info_write_pubkey(&data[3]) != 0)
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
          return;
        }
        resp[0] = UDS_SID_WRITE_DATA_BY_ID + UDS_POSITIVE_RESPONSE_OFFSET;
        resp[1] = data[1];
        resp[2] = data[2];
        proto_send_response(resp, 3);
        break;
      }

      case DID_QI_IAP_CONTROL:
      {
        /* data[3]=0x01 启动（data[4..5]=固件大小 16-bit BE）
         * data[3]=0x00/0x02 中止。UART：0xCC 0x01 + size */
        uint8_t sub = data[3];
        if (sub == 0x01U)
        {
          if (len < 6U)
          {
            proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
            return;
          }
          if (g_qi_ver_q_state != 0U)
          {
            /* 版本问询进行中：避免 UART 命令交叉 */
            proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_CONDITIONS_NOT_CORRECT);
            return;
          }
          g_qi_iap_total    = ((uint16_t)data[4] << 8) | (uint16_t)data[5];
          g_qi_iap_sent     = 0U;
          g_qi_iap_progress = 0U;
          g_qi_iap_last_tx_ms = 0U;
          g_qi_iap_pending_chunk = 0U;
          /* Discard leftover 0x01 reports so the prepare ACK can be parsed. */
          qi_protocol_rx_flush();
          (void)qi_protocol_iap_prepare(g_qi_iap_total);
          /* Wait for Qi prepare ACK (chip may erase flash) before 6E 21 30. */
          g_qi_iap_state = QI_IAP_WAIT_ACK;
          g_qi_iap_ack_timeout_ms = QI_IAP_PREPARE_TIMEOUT_MS;
          g_qi_iap_wait_start_ms = timer_get_tick();
          g_qi_iap_pending_did[0] = data[1];
          g_qi_iap_pending_did[1] = data[2];
        }
        else if ((sub == 0x00U) || (sub == 0x02U))
        {
          g_qi_iap_state = QI_IAP_IDLE;
          g_qi_iap_progress = 0U;
          g_qi_iap_total = 0U;
          g_qi_iap_sent = 0U;
          g_qi_iap_last_tx_ms = 0U;
          g_qi_iap_pending_chunk = 0U;
          g_qi_iap_pending_did[0] = 0U;
          g_qi_iap_pending_did[1] = 0U;
          resp[0] = UDS_SID_WRITE_DATA_BY_ID + UDS_POSITIVE_RESPONSE_OFFSET;
          resp[1] = data[1];
          resp[2] = data[2];
          proto_send_response(resp, 3);
        }
        else
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_REQUEST_OUT_OF_RANGE);
        }
        break;
      }

      case DID_QI_IAP_DATA:
      {
        /* data[3..4]=地址 16-bit BE，data[5..]=固件（最多 22B）
         * UART：0xCC 0x02 + addr + data
         *
         * ACK 链路：发 UART 帧 → 等 Qi 芯片 ACK → 才回 UDS 正响应。
         * 非阻塞：设 WAIT_ACK 状态，UDS 响应在 qi_iap_ack_poll() 中延迟发送。
         * Host 侧收到 NRC 0x72 时重试当前包。 */
        uint16_t addr;
        uint16_t chunk_len;

        if (len < 6U)
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
          return;
        }
        if (g_qi_iap_state != QI_IAP_IN_PROGRESS)
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_CONDITIONS_NOT_CORRECT);
          return;
        }
        addr = ((uint16_t)data[3] << 8) | (uint16_t)data[4];
        chunk_len = (uint16_t)(len - 5U);
        if (chunk_len > QI_IAP_MAX_CHUNK)
        {
          chunk_len = QI_IAP_MAX_CHUNK;
        }
        (void)qi_protocol_iap_data(addr, &data[5], (uint8_t)chunk_len);
        g_qi_iap_pending_chunk = (uint16_t)chunk_len;
        g_qi_iap_last_tx_ms = timer_get_tick();
        /* 不立即回 UDS 响应——进入 WAIT_ACK 状态，
         * 在 qi_iap_ack_poll() 中等 Qi 芯片 ACK 后再回复 */
        g_qi_iap_state = QI_IAP_WAIT_ACK;
        g_qi_iap_ack_timeout_ms = QI_IAP_ACK_TIMEOUT_MS;
        g_qi_iap_wait_start_ms = timer_get_tick();
        g_qi_iap_pending_did[0] = data[1];
        g_qi_iap_pending_did[1] = data[2];
        break;
      }

      case DID_CHARGER_ENABLE:
      {
        uint8_t val = data[3];
        if (val > 0x01U)
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_REQUEST_OUT_OF_RANGE);
          return;
        }
        /* reject enable (0x01) when blocking fault active */
        if ((val == 0x01U) && (g_qi_fault_code != 0x00U))
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_CONDITIONS_NOT_CORRECT);
          return;
        }
        g_qi_charger_enable = val;
        board_charge_set_enable(val);  /* CCU enable; PB2 controlled by charge_poll */
        resp[0] = UDS_SID_WRITE_DATA_BY_ID + UDS_POSITIVE_RESPONSE_OFFSET;
        resp[1] = data[1];
        resp[2] = data[2];
        proto_send_response(resp, 3);
        break;
      }

      case DID_POWER_LIMIT:
      {
        uint16_t val;
        uint8_t nvm_buf[2];
        if (len < 5U)
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
          return;
        }
        val = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
        /* only accept 5W / 10W / 15W */
        if ((val != 500U) && (val != 1000U) && (val != 1500U))
        {
          proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_REQUEST_OUT_OF_RANGE);
          return;
        }
        g_qi_power_limit_mw = val;
        nvm_buf[0] = (uint8_t)(val & 0xFFU);
        nvm_buf[1] = (uint8_t)((val >> 8) & 0xFFU);
        (void)qi_nvm_save(NVM_OFFSET_POWER_LIMIT, nvm_buf, 2U);
        /* forward to Qi chip: 5W=0x01, 10W=0x02, 15W=0x03 */
        {
          uint8_t qi_power = (val == 500U) ? QI_POWER_5W :
                             (val == 1000U) ? QI_POWER_10W : QI_POWER_15W;
          (void)qi_protocol_set_power(qi_power, 0U);
        }
        resp[0] = UDS_SID_WRITE_DATA_BY_ID + UDS_POSITIVE_RESPONSE_OFFSET;
        resp[1] = data[1];
        resp[2] = data[2];
        proto_send_response(resp, 3);
        break;
      }

      default:
        /* DID not writable */
        proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_REQUEST_OUT_OF_RANGE);
        break;
    }
  }
}

/**
 * @brief  SecurityAccess (0x27)
 * @note   APP side implements SecurityAccess fully (Boot safe mode does not):
 *         27 01 -> 67 01 + 32-byte seed (g_seed[32], refreshed on every 27 01,
 *         signature buffer cleared with it); unlocked 27 01 -> 67 01 + 32x0x00.
 *         27 03 -> chunked signature transfer (4B/frame x 16, blockSeq 0x01..0x10,
 *         blockSeq 0x01 resets the buffer); 27 02 -> sha256_hash(g_seed, 32U) +
 *         uECC_verify on the accumulated 64-byte signature.
 *         Verify fail: NRC 0x35 (invalidKey, fail_count+1); fail_count >= 3 arms
 *         the ~30s lockout: 27 02 -> NRC 0x36, 27 01 inside lockout -> NRC 0x37.
 * @param  data: UDS payload
 * @param  len:  payload length
 * @retval none
 */
static void handle_security_access(uint8_t *data, uint16_t len)
{
  uint8_t resp[8];
  uint8_t sub_func;
  uint32_t now_ms;

  if (len < 2U)
  {
    proto_send_nrc(UDS_SID_SECURITY_ACCESS, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    return;
  }

  sub_func = data[1] & UDS_SUBFUNC_MASK;
  now_ms = timer_get_tick();

  if (sub_func == 0x01U)
  {
    if (g_security_fail_count >= SECURITY_MAX_FAILURES)
    {
      if ((int32_t)(now_ms - g_security_lockout_until_ms) < 0)
      {
        proto_send_nrc(UDS_SID_SECURITY_ACCESS, UDS_NRC_REQUIRED_TIME_DELAY);
        return;
      }
      g_security_fail_count = 0;
    }
    if (security_unlocked)
    {
      {
        uint8_t unlock_resp[34];
        unlock_resp[0] = UDS_SID_SECURITY_ACCESS + UDS_POSITIVE_RESPONSE_OFFSET;
        unlock_resp[1] = 0x01U;
        memset(&unlock_resp[2], 0, 32U);
        proto_send_response(unlock_resp, 34);
      }
      return;
    }
    {
      uint8_t idx;
      for (idx = 0U; idx < 32U; idx += 4U)
      {
        uint32_t seed_val = generate_random_seed();
        g_seed[idx]     = (uint8_t)((seed_val >> 24) & 0xFFU);
        g_seed[idx + 1] = (uint8_t)((seed_val >> 16) & 0xFFU);
        g_seed[idx + 2] = (uint8_t)((seed_val >> 8) & 0xFFU);
        g_seed[idx + 3] = (uint8_t)(seed_val & 0xFFU);
      }
    }
    g_seed_generated = 1;
    g_seed_sub = 0x01U;
    g_sa_sig_bytes_received = 0;
    g_sa_sig_block_seq = 0;
    {
      uint8_t sa_resp[34];
      sa_resp[0] = UDS_SID_SECURITY_ACCESS + UDS_POSITIVE_RESPONSE_OFFSET;
      sa_resp[1] = 0x01U;
      memcpy(&sa_resp[2], g_seed, 32U);
      proto_send_response(sa_resp, 34);
    }
  }
  else if (sub_func == 0x03U)
  {
    uint8_t block_seq;
    uint8_t chunk_len;
    uint8_t i;

    if (!g_seed_generated)
    {
      proto_send_nrc(UDS_SID_SECURITY_ACCESS, UDS_NRC_REQUEST_SEQUENCE_ERROR);
      return;
    }
    if (len < 3U)
    {
      proto_send_nrc(UDS_SID_SECURITY_ACCESS, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
      return;
    }
    block_seq = data[2];
    if (block_seq == 0x01U)
    {
      g_sa_sig_block_seq = 0;
      g_sa_sig_bytes_received = 0;
      memset(g_sa_sig_buf, 0, 64);
    }
    g_sa_sig_block_seq++;
    if (g_sa_sig_block_seq == 0x00U)
    {
      g_sa_sig_block_seq = 0x01U;
    }
    if (block_seq != g_sa_sig_block_seq)
    {
      g_sa_sig_bytes_received = 0;
      proto_send_nrc(UDS_SID_SECURITY_ACCESS, UDS_NRC_TRANSFER_DATA_SUSPENDED);
      return;
    }
    chunk_len = (uint8_t)(len - 3U);
    if ((g_sa_sig_bytes_received + chunk_len) > 64U)
    {
      chunk_len = (uint8_t)(64U - g_sa_sig_bytes_received);
    }
    for (i = 0U; i < chunk_len; i++)
    {
      g_sa_sig_buf[g_sa_sig_bytes_received + i] = data[3U + i];
    }
    g_sa_sig_bytes_received = (uint8_t)(g_sa_sig_bytes_received + chunk_len);
    resp[0] = UDS_SID_SECURITY_ACCESS + UDS_POSITIVE_RESPONSE_OFFSET;
    resp[1] = 0x03U;
    resp[2] = block_seq;
    proto_send_response(resp, 3);
  }
  else if (sub_func == 0x02U)
  {
    uint8_t hash[32];

    if (!g_seed_generated)
    {
      proto_send_nrc(UDS_SID_SECURITY_ACCESS, UDS_NRC_REQUEST_SEQUENCE_ERROR);
      return;
    }
    /* 27 02 + 64B key in one ISO-TP message, or 27 03 chunks already in buf.
     * Do not clear seed on short 27 02 — host may retry 27 03 / 27 02. */
    if ((g_sa_sig_bytes_received != 64U) && (len >= 66U))
    {
      uint16_t k;
      for (k = 0U; k < 64U; k++)
      {
        g_sa_sig_buf[k] = data[2U + k];
      }
      g_sa_sig_bytes_received = 64U;
    }
    if (g_sa_sig_bytes_received != 64U)
    {
      proto_send_nrc(UDS_SID_SECURITY_ACCESS, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
      return;
    }
    proto_begin_long_op(UDS_SID_SECURITY_ACCESS);
    sha256_hash(g_seed, 32U, hash);
    if (uECC_verify(g_app_ecdsa_pubkey, hash, g_sa_sig_buf) == 1)
    {
      security_unlocked = 1;
      g_security_fail_count = 0;
      g_seed_generated = 0;
      proto_end_long_op();
      resp[0] = UDS_SID_SECURITY_ACCESS + UDS_POSITIVE_RESPONSE_OFFSET;
      resp[1] = 0x02U;
      proto_send_response(resp, 2);
    }
    else
    {
      security_unlocked = 0;
      g_security_fail_count++;
      g_seed_generated = 0;
      g_sa_sig_bytes_received = 0;
      proto_end_long_op();
      if (g_security_fail_count >= SECURITY_MAX_FAILURES)
      {
        g_security_lockout_until_ms = timer_get_tick() + SECURITY_LOCKOUT_MS;
        proto_send_nrc(UDS_SID_SECURITY_ACCESS, UDS_NRC_EXCEEDED_NUMBER_OF_ATTEMPTS);
      }
      else
      {
        proto_send_nrc(UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_KEY);
      }
    }
  }
  else
  {
    proto_send_nrc(UDS_SID_SECURITY_ACCESS, UDS_NRC_SUBFUNCTION_NOT_SUPPORTED);
  }
}

/**
 * @brief  RoutineControl (0x31)
 * @note   按 rid 分发：
 *         - 0x2100 Clear Faults：默认会话即可执行，不检查 Programming 会话 /
 *           SecurityAccess（Qi 侧要求，门禁过严会回 NRC 0x22 导致协议不通）；
 *           StartRoutine 执行逻辑暂留空（TODO），正响应 71 01 21 00 00；
 *           StopRoutine/RequestRoutineResults 暂不支持，回 NRC 0x31。
 *         - 其余 rid（含 0xFF00）：原样转 ota_dl_handle_erase()，APP 实现
 *           槽擦写全链（ota_download.c）：0x31 擦除非活跃槽 → 0x34/0x36 下载
 *           编程 → 0x37 验签+commit_backup 后自复位，BOOT 搬运至 App 区；
 *           门禁=PROGRAMMING 会话+SecurityAccess（handler 内逐项检查）。
 *           Boot 仅在复位后按 trial metadata 选槽/回滚/进 safe mode，
 *           不承担下载编程（旧"Boot safe mode only"架构已废弃）。
 */
static void handle_routine_control(uint8_t *data, uint16_t len)
{
  uint8_t  resp[5];
  uint8_t  sub_func;
  uint8_t  suppress;
  uint16_t rid;

  /* 长度检查先于 rid 分发：len<4 的畸形请求统一回 NRC 0x13 */
  if (len < 4U)
  {
    proto_send_nrc(UDS_SID_ROUTINE_CONTROL, UDS_NRC_INCORRECT_MESSAGE_LENGTH);
    return;
  }

  sub_func = data[1] & UDS_SUBFUNC_MASK;
  suppress = data[1] & UDS_SUBFUNC_SUPPRESS_POS_RESP;
  rid      = ((uint16_t)data[2] << 8) | (uint16_t)data[3];

  if (rid == ROUTINE_CLEAR_FAULTS)
  {
    /* Clear Faults 分支：仅支持 StartRoutine(0x01)。
     * suppressPosResp 只抑制正响应，NRC 仍须发送（ISO 14229-1）。
     * 本分支刻意不检查 Programming 会话/SecurityAccess：
     * 默认会话必须可执行，否则又回 0x22，协议不通。 */
    if (sub_func != 0x01U)
    {
      proto_send_nrc(UDS_SID_ROUTINE_CONTROL, UDS_NRC_REQUEST_OUT_OF_RANGE);
      return;
    }

    /* TODO: 待定义 Qi 侧故障标志/寄存器清除动作（Qi 侧故障标志清单待确认） */

    if (!suppress)
    {
      resp[0] = UDS_SID_ROUTINE_CONTROL + UDS_POSITIVE_RESPONSE_OFFSET; /* 0x71 */
      resp[1] = 0x01U;                  /* StartRoutine 回显（suppress 位已剥除） */
      resp[2] = (uint8_t)(rid >> 8);    /* 0x21 */
      resp[3] = (uint8_t)(rid & 0xFFU); /* 0x00 */
      resp[4] = 0x00U;                  /* RoutineStatusRecord: 0x00 = 成功 */
      proto_send_response(resp, 5U);    /* 5 字节 ≤ 7，单帧直发，无流控/多帧 */
    }
    return;
  }

  /* 其余 rid（含 0xFF00 erase）：保持原行为，data/len 原样转发，
   * 门禁逻辑一字不改，仍在 ota_dl_handle_erase 内逐项检查 */
  ota_dl_handle_erase(data, len);
}

/**
 * @brief  TesterPresent (0x3E)
 * @param  data: UDS payload
 * @param  len:  payload length
 * @retval none
 */
static void handle_tester_present(uint8_t *data, uint16_t len)
{
  uint8_t resp[8];
  uint8_t sub_func;
  uint8_t suppress;

  /* update session keepalive timestamp */
  last_tester_present_tick = timer_get_tick();

  if (len >= 2U)
  {
    sub_func = data[1];
    suppress = sub_func & UDS_SUBFUNC_SUPPRESS_POS_RESP;

    if (!suppress)
    {
      resp[0] = UDS_SID_TESTER_KEEPALIVE + UDS_POSITIVE_RESPONSE_OFFSET;
      resp[1] = sub_func & UDS_SUBFUNC_MASK;
      proto_send_response(resp, 2);
    }
  }
  else
  {
    /* no sub-function: send positive response */
    resp[0] = UDS_SID_TESTER_KEEPALIVE + UDS_POSITIVE_RESPONSE_OFFSET;
    proto_send_response(resp, 1);
  }
}

/* ========================================================================== */
/*  Qi IAP frame callback                                                    */
/* ========================================================================== */

/** @brief  Qi IAP ACK status codes from Qi chip */
#define QI_IAP_ACK_OK       0x00U
#define QI_IAP_ACK_COMPLETE 0x02U
#define QI_IAP_ACK_FAILED   0x03U

/**
 * @brief  apply a parsed Qi IAP ACK status to the state machine
 */
static void qi_iap_apply_ack_status(uint8_t status)
{
  if (status == QI_IAP_ACK_FAILED)
  {
    g_qi_iap_state = QI_IAP_FAILED;
    g_qi_iap_pending_chunk = 0U;
    return;
  }

  if (status == QI_IAP_ACK_COMPLETE)
  {
    if (g_qi_iap_pending_chunk > 0U)
    {
      g_qi_iap_sent += g_qi_iap_pending_chunk;
      g_qi_iap_pending_chunk = 0U;
    }
    g_qi_iap_state = QI_IAP_SUCCESS;
    g_qi_iap_progress = 100U;
    return;
  }

  if (status == QI_IAP_ACK_OK)
  {
    if (g_qi_iap_state == QI_IAP_WAIT_ACK)
    {
      if (g_qi_iap_pending_chunk > 0U)
      {
        g_qi_iap_sent += g_qi_iap_pending_chunk;
        g_qi_iap_pending_chunk = 0U;
        if (g_qi_iap_total > 0U)
        {
          g_qi_iap_progress = (uint8_t)((uint32_t)g_qi_iap_sent * 100U / g_qi_iap_total);
          if (g_qi_iap_progress > 100U)
          {
            g_qi_iap_progress = 100U;
          }
        }
      }
      g_qi_iap_state = QI_IAP_IN_PROGRESS;
    }
  }
}

/**
 * @brief  Qi frame callback: handle IAP ACK and status report (0x01)
 */
static void qi_iap_frame_cb(const qi_frame_t *frame)
{
  if (frame == (const qi_frame_t *)0)
  {
    return;
  }

  /* ---- Qi IAP ACK ----
   * 0xCC: data[0]=sub_cmd (0x01/0x02), data[1]=status; reserved optional
   * 0x00: generic ACK, data[0]=status
   * Some chips omit the reserved 0x00, so accept data_len >= 1. */
  if (frame->cmd == QI_CMD_IAP)
  {
    uint8_t status;

    if (frame->data_len < 1U)
    {
      return;
    }
    if ((frame->data_len >= 2U) &&
        ((frame->data[0] == QI_IAP_PREPARE) || (frame->data[0] == QI_IAP_DATA)))
    {
      status = frame->data[1];
    }
    else
    {
      status = frame->data[0];
    }
    qi_iap_apply_ack_status(status);
    return;
  }

  if ((frame->cmd == QI_CMD_ACK) && (g_qi_iap_state == QI_IAP_WAIT_ACK))
  {
    if (frame->data_len >= 1U)
    {
      qi_iap_apply_ack_status(frame->data[0]);
    }
    return;
  }

  /* ---- Qi 版本查询回复（DID 0x2013 主动问询）----
   * 主格式：CMD 0x03 + 2B 版本 LE；兼容：Qi 侧用通用 ACK(0x00) 携带版本
   *（仅在 IAP 空闲时采纳，避免误吞 IAP ACK）。收到即同步缓存并直发
   * 62 20 13 正响应；超时兜底在 qi_ver_query_poll()。 */
  if (g_qi_ver_q_state == 1U)
  {
    const uint8_t *vsrc = (const uint8_t *)0;
    if ((frame->cmd == QI_CMD_VERSION_QUERY) && (frame->data_len >= 2U))
    {
      vsrc = &frame->data[0];
    }
    else if ((frame->cmd == QI_CMD_ACK) && (g_qi_iap_state == QI_IAP_IDLE) &&
             (frame->data_len >= 2U))
    {
      vsrc = &frame->data[0];
    }
    if (vsrc != (const uint8_t *)0)
    {
      uint8_t resp[5];
      g_qi_fw_version   = (uint16_t)vsrc[0] | ((uint16_t)vsrc[1] << 8);
      g_qi_ver_q_state  = 0U;
      resp[0] = UDS_SID_READ_DATA_BY_ID + UDS_POSITIVE_RESPONSE_OFFSET;
      resp[1] = 0x20U;
      resp[2] = 0x13U;
      resp[3] = vsrc[0];
      resp[4] = vsrc[1];
      proto_send_response(resp, 5);
      return;
    }
  }

  /* ---- Qi status report (0x01) 信息读取 ----
   * 规范 6 字节：status1, status2, power LE, version LE
   * 扩展 ≥13 字节：额外电压/电流/温度/FOD/故障/降额 */
  if (frame->cmd == QI_CMD_STATUS_REPORT)
  {
    uint8_t status_byte;

    if (frame->data_len < 6U)
    {
      return;
    }

    status_byte = frame->data[0];

    /* decode charge state from status bits */
    if ((status_byte & QI_STATUS_CHARGING) != 0U)
    {
      g_qi_charge_state = QI_CHARGE_CHARGING;
      g_qi_device_present = 1U;
    }
    else if ((status_byte & QI_STATUS_FULL) != 0U)
    {
      g_qi_charge_state = QI_CHARGE_COMPLETE;
      g_qi_device_present = 1U;
    }
    else if ((status_byte & QI_STATUS_PING) != 0U)
    {
      g_qi_charge_state = QI_CHARGE_DEVICE_DETECTED;
      g_qi_device_present = 1U;
    }
    else
    {
      /* no device-related status bits set */
      if (g_qi_charger_enable != 0U)
      {
        g_qi_charge_state = QI_CHARGE_STANDBY;
      }
      else
      {
        g_qi_charge_state = QI_CHARGE_DISABLED;
      }
      g_qi_device_present = 0U;
    }

    /* protection/fault bits */
    if ((status_byte & QI_STATUS_FOD) != 0U)
    {
      g_qi_fod_status = 0x02U;  /* confirmed */
      g_qi_fault_code = 0x06U;  /* FOD fault */
      if (g_qi_charge_state == QI_CHARGE_CHARGING)
      {
        g_qi_charge_state = QI_CHARGE_SUSPENDED_FOD;
      }
    }
    else if ((status_byte & QI_STATUS_OTP) != 0U)
    {
      g_qi_fault_code = 0x07U;  /* coil over-temp */
      if (g_qi_charge_state == QI_CHARGE_CHARGING)
      {
        g_qi_charge_state = QI_CHARGE_SUSPENDED_THERMAL;
      }
    }
    else if ((status_byte & (QI_STATUS_OVP | QI_STATUS_UVP | QI_STATUS_OCP)) != 0U)
    {
      if ((status_byte & QI_STATUS_OVP) != 0U)
      {
        g_qi_fault_code = 0x04U;  /* input over-voltage */
      }
      else if ((status_byte & QI_STATUS_UVP) != 0U)
      {
        g_qi_fault_code = 0x05U;  /* input under-voltage */
      }
      else
      {
        g_qi_fault_code = 0x0AU;  /* input over-current */
      }
      g_qi_charge_state = QI_CHARGE_FAULT;
    }

    /* 帧布局（docs/4. IAP数据通信协议规范.md §2.1，0 基帧偏移）：
     *   data[0-1] = status1/status2（帧偏移 4-5）
     *   data[2-3] = 实时功率 LE mW（帧偏移 6-7）
     *   data[4-5] = 版本号 LE（帧偏移 8-9，与 DID 0x2133 一致）
     * 扩展帧 (≥13B) 额外字段：
     *   data[6]   = voltage, data[7] = current, data[8] = temp
     *   data[9]   = FOD, data[10] = reserved, data[11] = fault
     *   data[12]  = thermal derate
     *
     * NOTE: 曾按 data[2-3]=version/data[4-5]=power 解析（与规格书
     *       装反），2026-09-22 对齐规格书修正。 */
    g_qi_output_power_mw = (uint16_t)frame->data[2]
                         | ((uint16_t)frame->data[3] << 8);
    g_qi_fw_version = (uint16_t)frame->data[4]
                    | ((uint16_t)frame->data[5] << 8);

    /* 扩展帧额外字段 */
    if (frame->data_len >= 13U)
    {
      g_qi_voltage_raw = frame->data[6];
      g_qi_current_raw = frame->data[7];
      g_qi_pcb_temp = frame->data[8];
      if (frame->data[9] != 0x00U)
      {
        g_qi_fod_status = frame->data[9];
      }
      if (frame->data[11] != 0x00U)
      {
        g_qi_fault_code = frame->data[11];
      }
      g_qi_thermal_derate = frame->data[12];
    }

    return;
  }
}

/* ========================================================================== */
/*  Main UDS message dispatcher                                              */
/* ========================================================================== */

/**
 * @brief  process a complete UDS message (ISO-TP payload already extracted)
 * @note   called from isotp_message_received() after reassembly.
 * @param  data: pointer to UDS payload (first byte is SID)
 * @param  len:  UDS payload length
 * @retval none
 */
static void uds_process_message(uint8_t *data, uint16_t len)
{
  uint8_t service_id;

  if ((data == (uint8_t *)0) || (len == 0U))
  {
    return;
  }

  /* any diagnostic request refreshes S3 (ISO 14229) and the 6 min bus idle */
  last_tester_present_tick = timer_get_tick();
  can_lp_mark_uds();

  service_id = data[0];

  switch (service_id)
  {
    case UDS_SID_DIAG_SESSION_CTRL:
      handle_diag_session_ctrl(data, len);
      break;

    case UDS_SID_ECU_RESET:
      handle_ecu_reset(data, len);
      break;

    case UDS_SID_READ_DATA_BY_ID:
      handle_read_data_by_id(data, len);
      break;

    case UDS_SID_WRITE_DATA_BY_ID:
      handle_write_data_by_id(data, len);
      break;

    case UDS_SID_SECURITY_ACCESS:
      handle_security_access(data, len);
      break;

    case UDS_SID_ROUTINE_CONTROL:
      handle_routine_control(data, len);
      break;

    case UDS_SID_REQUEST_DOWNLOAD:
      ota_dl_handle_request_download(data, len);
      break;

    case UDS_SID_TRANSFER_DATA:
      ota_dl_handle_transfer_data(data, len);
      break;

    case UDS_SID_TRANSFER_EXIT:
      ota_dl_handle_transfer_exit(data, len);
      break;

    case UDS_SID_TRANSFER_SIGNATURE:
      proto_send_nrc(service_id, UDS_NRC_SERVICE_NOT_SUPPORTED);
      break;

    case UDS_SID_TESTER_KEEPALIVE:
      handle_tester_present(data, len);
      break;

    default:
      /* unsupported service */
      proto_send_nrc(service_id, UDS_NRC_SERVICE_NOT_SUPPORTED);
      break;
  }
}

/* ========================================================================== */
/*  ISO-TP callback and CAN RX handler                                       */
/* ========================================================================== */

/**
 * @brief  ISO-TP completion callback
 * @note   invoked when a complete UDS message has been reassembled.
 *         checks session timeout before forwarding to uds_process_message().
 * @param  data: pointer to complete UDS payload
 * @param  len:  payload length in bytes
 * @retval none
 */
static void isotp_message_received(uint8_t *data, uint16_t len)
{
  uint32_t now;

  /* session timeout check: if in non-default session and no TesterPresent
   * received within SESSION_TIMEOUT_MS, fall back to default session */
  if (current_session != SESSION_DEFAULT)
  {
    now = timer_get_tick();
    if ((now - last_tester_present_tick) >= SESSION_TIMEOUT_MS)
    {
      session_reset_to_default();
    }
  }

  uds_process_message(data, len);
}

/**
 * @brief  CAN RX callback for UDS protocol handling
 * @note   called from can_driver_poll() in main loop context.
 * @param  id:   29-bit extended identifier of received frame
 * @param  data: pointer to received data buffer
 * @param  len:  data length (0~8)
 * @retval none
 */
static void can_protocol_rx_handler(uint32_t id, uint8_t *data, uint8_t len)
{
  /* physical request or functional broadcast */
  if ((id != CAN_PROTO_UDS_REQUEST) &&
      ((id & 0x1FFFFF00U) != 0x18DB3300U))
  {
    return;
  }

  if (len == 0)
  {
    return;
  }

  can_lp_mark_uds();
  isotp_rx_process(data, len);
}

/* ========================================================================== */
/*  Exported functions                                                       */
/* ========================================================================== */

/**
 * @brief  initialize CAN protocol module
 * @param  none
 * @retval none
 */
void can_protocol_init(void)
{
  current_session          = SESSION_DEFAULT;
  security_unlocked        = 0;
  last_tester_present_tick = timer_get_tick();
  g_can_awake              = 0U;
  g_need_lifecycle_announce = 0U;
  g_uds_last_ms            = timer_get_tick();

  isotp_init(isotp_message_received);
  can_driver_register_rx_callback(can_protocol_rx_handler);
  qi_protocol_register_callback(qi_iap_frame_cb);

  /* load persistent Qi config from NVM (nvm_drv_init already called in main) */
  qi_nvm_load_config();

  /* CAN_LP_STANDBY_ENABLE=1：sit1145_init() 已进 Standby。OTA trial 必须在
   * SysTick 中断起来后再 enter_normal（harvest / sit1145_wait_cts / wait_tx_idle
   * 都看 timer_get_tick）。其余上电保持 Standby。
   * CAN_LP_STANDBY_ENABLE=0（回归调试期临时禁用）：上电/复位一律不进 Standby，
   * 与 trial 同路径置 g_lp_need_online，首次 can_protocol_poll 统一延时
   * can_lp_enter_normal（SysTick 已就绪），CAN 常在线；trial confirm 流程
   * 本就要求 CAN 在线，行为只会更可靠，流程本身不受影响。 */
  if (can_lp_trial_needs_normal() != 0U)
  {
    g_lp_need_online = 1U;
  }
  else
  {
#if (CAN_LP_STANDBY_ENABLE != 0U)
    g_lp_need_online = 0U;
    can_lp_hold_standby();
#else
    /* 临时禁用：非 trial 上电同样延时 enter_normal，保持 CAN 在线 */
    g_lp_need_online = 1U;
#endif
  }
}

/**
 * @brief  Qi IAP ACK poll: non-blocking check for Qi chip UART ACK
 * @note   Called from can_protocol_poll() when state == WAIT_ACK.
 *         On ACK: sends deferred UDS positive response, resumes IAP_IN_PROGRESS.
 *         On NAK: sends NRC 0x72, resumes IAP_IN_PROGRESS (allow host retry).
 *         On timeout: sends NRC 0x72, resumes IAP_IN_PROGRESS (allow host retry).
 */
static void qi_iap_ack_poll(void)
{
  uint32_t now;

  if (g_qi_iap_state != QI_IAP_WAIT_ACK)
  {
    return;
  }

  /* flush any pending UART bytes from Qi chip */
  qi_protocol_poll();

  now = timer_get_tick();

  /* check if callback already received an ACK or FAILED */
  if ((g_qi_iap_state == QI_IAP_IN_PROGRESS) || (g_qi_iap_state == QI_IAP_SUCCESS))
  {
    /* ACK received (callback left WAIT_ACK)
     * Send deferred positive response */
    uint8_t resp[3];
    resp[0] = UDS_SID_WRITE_DATA_BY_ID + UDS_POSITIVE_RESPONSE_OFFSET;
    resp[1] = g_qi_iap_pending_did[0];
    resp[2] = g_qi_iap_pending_did[1];
    proto_send_response(resp, 3);
    return;
  }
  if (g_qi_iap_state == QI_IAP_FAILED)
  {
    /* NAK from Qi chip — allow host retry */
    g_qi_iap_state = QI_IAP_IN_PROGRESS;
    proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
    return;
  }

  /* timeout: no response from Qi chip */
  if ((now - g_qi_iap_wait_start_ms) >= g_qi_iap_ack_timeout_ms)
  {
    g_qi_iap_state = QI_IAP_IN_PROGRESS;
    g_qi_iap_pending_chunk = 0U;
    proto_send_nrc(UDS_SID_WRITE_DATA_BY_ID, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
  }
}

/**
 * @brief  Qi 版本问询超时兜底（DID 0x2013 延迟应答）
 * @note   Called from can_protocol_poll()。回复到达走帧回调直发；
 *         此处只处理 500ms 超时：缓存非 0 回缓存值（产线不断流），
 *         缓存为 0 回 NRC 0x22（如需严格问询语义可去掉缓存兜底）。
 */
static void qi_ver_query_poll(void)
{
  if (g_qi_ver_q_state != 1U)
  {
    return;
  }
  if ((timer_get_tick() - g_qi_ver_q_start_ms) < QI_VER_QUERY_TIMEOUT_MS)
  {
    return;
  }
  g_qi_ver_q_state = 0U;
  if (g_qi_fw_version != 0U)
  {
    uint8_t resp[5];
    resp[0] = UDS_SID_READ_DATA_BY_ID + UDS_POSITIVE_RESPONSE_OFFSET;
    resp[1] = 0x20U;
    resp[2] = 0x13U;
    resp[3] = (uint8_t)(g_qi_fw_version & 0xFFU);
    resp[4] = (uint8_t)(g_qi_fw_version >> 8);
    proto_send_response(resp, 5);
  }
  else
  {
    proto_send_nrc(UDS_SID_READ_DATA_BY_ID, UDS_NRC_CONDITIONS_NOT_CORRECT);
  }
}

void can_protocol_poll(void)
{
  uint32_t now;
  static uint32_t sit_last;

  /* Drain Qi UART so 0x01 reports don't overflow the 64B RX buffer.
   * Skip while WAIT_ACK: qi_iap_ack_poll() owns the parser then, otherwise
   * the ACK would be consumed here and the deferred UDS response never sent. */
  if (g_qi_iap_state != QI_IAP_WAIT_ACK)
  {
    qi_protocol_poll();
  }

  if (g_lp_need_online != 0U)
  {
    g_lp_need_online = 0U;
    can_lp_enter_normal();
  }

  now = timer_get_tick();

  /* Qi IAP ACK poll: non-blocking check for Qi chip UART ACK */
  qi_iap_ack_poll();
  qi_ver_query_poll();
  ota_dl_poll();

  /* Qi IAP auto-complete: if all data sent and no ACK within timeout,
   * assume success — but only if Qi chip has not reported FAILED.
   * Poll UART once more to flush any pending FAILED ACK before
   * overwriting state. This closes the race where can_protocol_poll()
   * runs before qi_uart_poll() on the same loop iteration. */
  if ((g_qi_iap_state == QI_IAP_IN_PROGRESS) &&
      (g_qi_iap_total > 0U) &&
      (g_qi_iap_sent >= g_qi_iap_total) &&
      (g_qi_iap_last_tx_ms != 0U) &&
      ((now - g_qi_iap_last_tx_ms) >= QI_IAP_DONE_TIMEOUT_MS))
  {
    /* flush any pending UART bytes so FAILED ACK is not missed */
    qi_protocol_poll();
    if (g_qi_iap_state == QI_IAP_IN_PROGRESS)
    {
      g_qi_iap_state    = QI_IAP_SUCCESS;
      g_qi_iap_progress = 100U;
    }
  }

  if (g_can_awake == 0U)
  {
    if ((now - g_standby_since_ms) >= CAN_LP_WAKE_INHIBIT_MS)
    {
      uint8_t src = sit1145_wakeup_pending();
      if (src != 0U)
      {
        g_lp_last_wake_src = src;
        can_lp_enter_normal();
      }
    }
  }

  if ((g_can_awake != 0U) && (g_need_lifecycle_announce != 0U) &&
      ((int32_t)(now - g_announce_due_ms) >= 0))
  {
    g_need_lifecycle_announce = 0U;
    if (g_lp_woke_from_standby != 0U)
    {
      /* 01 41 57 4B cnt src secL secH — 与上电 BOOTUP 01 41 00 区分 */
      can_lp_tx_marker(LIFECYCLE_BOOTUP, 0x57U, 0x4BU, g_lp_wup_count,
                       g_lp_last_wake_src,
                       (uint8_t)(g_lp_last_standby_sec & 0xFFU),
                       (uint8_t)((g_lp_last_standby_sec >> 8) & 0xFFU));
    }
    else
    {
      lifecycle_set_state(LIFECYCLE_BOOTUP);
    }
    lifecycle_set_state(LIFECYCLE_OPERATIONAL);
  }

  if (g_can_awake == 0U)
  {
    return;
  }

  /* Flash erase (trial confirm / NVM) stalls this single-bank MCU; CAN error
   * IRQ is missed and the controller sits in bus-off while g_can_awake=1.
   * Host then sees 0x34 timeout and no 57 4B ident (we never re-enter Normal). */
  if (can_busoff_get(CAN1) != RESET)
  {
    proto_can_busoff_recover();
    (void)sit1145_normal_mode_set();
  }

  proto_flush_pending_tx();
  isotp_poll();

  if ((now - sit_last) >= 500U)
  {
    sit_last = now;
    (void)sit1145_normal_mode_set();
    if (can_busoff_get(CAN1) != RESET)
    {
      proto_can_busoff_recover();
    }
  }

#if (CAN_LP_STANDBY_ENABLE != 0U) && (CAN_LP_IDLE_TIMEOUT_MS > 0U)
  if ((now - g_uds_last_ms) >= CAN_LP_IDLE_TIMEOUT_MS)
  {
    can_lp_enter_standby();
    return;
  }
#endif

  if (current_session != SESSION_DEFAULT)
  {
    if ((now - last_tester_present_tick) >= SESSION_TIMEOUT_MS)
    {
      session_reset_to_default();
    }
  }
}

uint8_t can_protocol_is_bus_awake(void)
{
  return g_can_awake;
}

uint8_t can_protocol_lifecycle_tx_ready(void)
{
  if (g_can_awake == 0U)
  {
    return 0U;
  }
  if ((g_need_lifecycle_announce != 0U) &&
      ((int32_t)(timer_get_tick() - g_announce_due_ms) < 0))
  {
    return 0U;
  }
  return 1U;
}

/**
 * @brief  get current diagnostic session
 * @retval SESSION_DEFAULT, SESSION_PROGRAMMING, or SESSION_EXTENDED
 */
uint8_t can_protocol_get_session(void)
{
  return current_session;
}

/**
 * @brief  check if security access Level 1 is unlocked
 * @retval 1 = unlocked, 0 = locked
 */
uint8_t can_protocol_is_security_unlocked(void)
{
  return security_unlocked;
}
