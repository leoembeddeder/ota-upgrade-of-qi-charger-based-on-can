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
  * ---- safe mode 诊断帧格式（与 python_tools/zcanpro_ext_ota_auto.py
  * ---- 头部 SAFE_MODE 注释严格一致，两侧勿改其一）----
  *
  *   探测请求: CAN ID 0x18DA0D03 (CAN_ID_UDS_REQUEST)
  *             数据 22 21 13 —— RX 为自实现兼容解析（不依赖 ISO-TP
  *             协议栈，见下方 enter_safe_mode 接收循环），兼容 ISO-TP SF
  *             (03 22 21 13 ...) 与裸 UDS (22 21 13 ...) 两种写法
  *   22 2113 应答: CAN ID 0x18DA030D (CAN_ID_UDS_RESPONSE)
  *             ISO-TP 单帧，DLC=8（safe_send_sf 组帧，尾部 0xCC 填充）:
  *
  *             05 62 21 13 FE <fail_step> CC CC
  *
  *             PCI=0x05 表示单帧载荷 5 字节。主机侧解析在
  *             python_tools/zcanpro_ext_ota_auto.py _safe_mode_step，
  *             双格式兼容：历史裸帧 62 21 13 FE <fail_step> + 现行
  *             ISO-TP SF 05 62 21 13 FE <fail_step>
  *   3E 应答: suppress 位（sub bit7）为 0 时回 ISO-TP 单帧
  *             02 7E <子功能低 7 位>（同 0x18DA030D）；suppress 位
  *             为 1 不应答
  *   其他 22 DID: 回 NRC ISO-TP 单帧 7F 22 11（servicesNotSupported）
  *   心跳: 每 500ms 在 CAN ID 0x18FF260D (CAN_ID_LIFECYCLE_BROADCAST)
  *             发 01 41 42 54 cause fail_step A5 00（'ABT' 标记帧），
  *             并同周期重切 SIT1145 收发器 Normal
  *             （sit1145_normal_mode_set，见 enter_safe_mode 主循环）
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

/* ===== Boot 诊断标记帧实现（观察不干预，2026-09-20 用户授权诊断版）=====
 * 帧格式/CAN ID 见 boot_safe_mode.h BOOT_DIAG 注释；实现纪律：
 * ① 只报信不改决策——本节函数不写 metadata/不改任何选择逻辑；
 * ② 发送有界——can_driver_send 缓冲满直接 -1 返回，成功才等
 *    can_driver_wait_tx_idle(BOOT_DIAG_TX_TIMEOUT_MS)，总线挂死即弃帧；
 * ③ polling 发送，不依赖中断；safe mode 既有帧零触碰。 */
#define BOOT_DIAG_TX_TIMEOUT_MS  3U

uint8_t g_diag_meta_src = 0xFFU;

static void boot_diag_frame_send(const uint8_t *payload, uint8_t n)
{
  uint8_t f[8];
  uint8_t i;

  if ((payload == (const uint8_t *)0) || (n == 0U) || (n > 7U))
  {
    return;
  }
  f[0] = payload[0];
  for (i = 1U; i < 8U; i++)
  {
    f[i] = (uint8_t)((i < n) ? payload[i] : 0xCCU);
  }
  if (can_driver_send(BOOT_DIAG_CAN_ID, f, 8U) == 0)
  {
    (void)can_driver_wait_tx_idle(BOOT_DIAG_TX_TIMEOUT_MS);
  }
}

void boot_diag_can_init(void)
{
  /* 初始化语义同 enter_safe_mode 既有调用（can_driver_init 自含
   * sit1145_init→Normal）；提前到 main.c step2 仅为 M1~M4 可发帧。
   * 影响面：仅 CAN1/GPIOA/GPIOB/SPI1 时钟引脚+收发器 Normal，与
   * 决策链（flash 读写/CRC/ECDSA）零交集；enter_safe_mode 内原调用
   * 保留（路径语义不变，重复 init 幂等复位）。 */
  can_driver_init();
  (void)sit1145_normal_mode_set();
}

void boot_diag_m1(const void *meta)
{
  const ota_metadata_t *m = (const ota_metadata_t *)meta;
  uint8_t p[6];

  p[0] = 0xA1U;
  p[1] = (m != (const ota_metadata_t *)0) ? m->active_slot : 0xFFU;
  p[2] = g_diag_meta_src;
  if (m != (const ota_metadata_t *)0)
  {
    p[3] = (uint8_t)((m->magic == META_MAGIC) ? 1U : 0U);
    p[4] = (uint8_t)((m->version == META_VERSION) ? 1U : 0U);
    p[5] = (uint8_t)((boot_crc32((const void *)m,
                                 sizeof(ota_metadata_t) - 4U) == m->crc32) ? 1U : 0U);
  }
  else
  {
    p[3] = 0U;
    p[4] = 0U;
    p[5] = 0U;
  }
  boot_diag_frame_send(p, 6U);
}

void boot_diag_m2(int8_t ret, uint8_t slot)
{
  uint8_t p[3];

  p[0] = 0xA2U;
  p[1] = slot;
  p[2] = (uint8_t)ret;   /* 0=成功；0xFF=-1 失败（safe mode 0x01 路径） */
  boot_diag_frame_send(p, 3U);
}

void boot_diag_m3(uint8_t pass, uint8_t fail_step, uint8_t slot)
{
  uint8_t p[4];

  p[0] = 0xA3U;
  p[1] = pass;
  p[2] = fail_step;
  p[3] = slot;
  boot_diag_frame_send(p, 4U);
}

void boot_diag_m4(uint32_t app_addr)
{
  uint8_t p[5];

  p[0] = 0xA4U;
  p[1] = (uint8_t)(app_addr & 0xFFU);
  p[2] = (uint8_t)((app_addr >> 8) & 0xFFU);
  p[3] = (uint8_t)((app_addr >> 16) & 0xFFU);
  p[4] = (uint8_t)((app_addr >> 24) & 0xFFU);
  boot_diag_frame_send(p, 5U);
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
