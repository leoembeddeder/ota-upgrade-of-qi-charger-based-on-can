/**
  **************************************************************************
  * @file     boot_safe_mode.c
  * @brief    Safe mode: no bootable slot. Persist failure context and
  *           answer minimal CAN diagnostic probe frames.
  **************************************************************************
  *
  * Field OTA download lives in APP. Boot only selects a slot and jumps.
  * Empty chip / both slots invalid: stay here and report why via CAN.
  * Factory image: merge_prod_bin.py from 0x08000000.
  *
  * ---- safe mode 诊断标记帧格式（与 python_tools/zcanpro_ext_ota_auto.py
  * ---- 头部 SAFE_MODE 注释严格一致，两侧勿改其一）----
  *
  *   探测请求: CAN ID 0x18DA0D03 (CAN_ID_UDS_REQUEST)
  *             数据 22 21 13 —— 兼容 ISO-TP SF (03 22 21 13 ...) 与裸
  *             UDS (22 21 13 ...) 两种写法
  *   Boot 应答: CAN ID 0x18DA030D (CAN_ID_UDS_RESPONSE)
  *             5 字节原始单帧，非 ISO-TP、无 PCI 字节，仅此一帧:
  *
  *             62 21 13 FE <fail_step>
  *
  *   fail_step 语义（提取自 boot_verify.c g_verify_fail_step）:
  *     0 = 未执行镜像校验 / select_boot_slot 无有效槽
  *     1 = 镜像 magic 校验失败
  *     2 = image_length 为 0 或超出槽范围
  *     3 = 镜像 CRC32 校验失败
  *     4 = Reset handler 不在槽内（跨槽链接镜像）
  *     5 = ECDSA 公钥缺失/无效（Device Info 与内置均不可用）
  *     6 = ECDSA P-256 验签失败
  *
  **************************************************************************
  */

#include "boot_safe_mode.h"
#include "boot_metadata.h"
#include "boot_trial.h"
#include "boot_verify.h"
#include "can_driver.h"
#include "sit1145.h"
#include "timer_drv.h"
#include "at32f422_426.h"
#include "at32f422_426_can.h"

static void safe_send_sf(const uint8_t *uds, uint8_t n)
{
  uint8_t sf[8];
  uint8_t i;

  if ((uds == (const uint8_t *)0) || (n == 0U) || (n > 7U))
  {
    return;
  }
  sf[0] = n;
  for (i = 0U; i < n; i++)
  {
    sf[1U + i] = uds[i];
  }
  for (i = (uint8_t)(1U + n); i < 8U; i++)
  {
    sf[i] = 0xCCU;
  }
  (void)can_driver_send(CAN_ID_UDS_RESPONSE, sf, 8U);
}

static void safe_heartbeat(uint8_t cause, uint8_t step)
{
  uint8_t d[8];

  d[0] = 0x01U;
  d[1] = 0x41U;
  d[2] = 0x42U; /* 'B' */
  d[3] = 0x54U; /* 'T' */
  d[4] = cause;
  d[5] = step;
  d[6] = 0xA5U;
  d[7] = 0x00U;
  (void)can_driver_send(CAN_ID_LIFECYCLE_BROADCAST, d, 8U);
}

void enter_safe_mode(uint8_t cause)
{
  uint32_t id;
  uint8_t  data[CAN_DRIVER_MAX_DATA_LEN];
  uint8_t  len;
  uint8_t  step = g_verify_fail_step;
  uint32_t last_hb;

  g_meta.reserved1    = (uint16_t)(((uint16_t)cause << 8) | step);
  g_meta.reserved2[0] = g_meta.last_boot_reason;
  g_meta.reserved2[1] = 0xA5U;
  (void)boot_metadata_save(&g_meta);

  can_driver_init();
  (void)sit1145_normal_mode_set();
  last_hb = timer_get_tick();
  safe_heartbeat(cause, step);

  while (1)
  {
    if (can_flag_get(CAN1, CAN_RIF_FLAG) != RESET)
    {
      can_driver_rx_irq_handler();
    }

    while (can_driver_recv(&id, data, &len) == 0)
    {
      uint8_t *u = data;
      uint8_t  un = len;

      if (id != CAN_ID_UDS_REQUEST)
      {
        continue;
      }
      if ((len >= 2U) && ((data[0] & 0xF0U) == 0x00U))
      {
        uint8_t pci_n = data[0] & 0x0FU;
        if ((pci_n >= 1U) && ((uint8_t)(1U + pci_n) <= len))
        {
          u = &data[1];
          un = pci_n;
        }
      }

      if ((un >= 2U) && (u[0] == 0x3EU))
      {
        uint8_t r[2];
        r[0] = 0x7EU;
        r[1] = (uint8_t)(u[1] & 0x7FU);
        if ((u[1] & 0x80U) == 0U)
        {
          safe_send_sf(r, 2U);
        }
      }
      else if ((un >= 3U) && (u[0] == 0x22U) && (u[1] == 0x21U) && (u[2] == 0x13U))
      {
        uint8_t r[5];
        r[0] = 0x62U;
        r[1] = 0x21U;
        r[2] = 0x13U;
        r[3] = 0xFEU;
        r[4] = step;
        safe_send_sf(r, 5U);
      }
      else if ((un >= 1U) && (u[0] == 0x22U))
      {
        uint8_t r[3];
        r[0] = 0x7FU;
        r[1] = 0x22U;
        r[2] = 0x11U;
        safe_send_sf(r, 3U);
      }
    }

    if ((timer_get_tick() - last_hb) >= 500U)
    {
      last_hb = timer_get_tick();
      (void)sit1145_normal_mode_set();
      safe_heartbeat(cause, step);
    }
  }
}
