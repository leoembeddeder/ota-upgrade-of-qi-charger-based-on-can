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
#include "at32f422_426.h"

void enter_safe_mode(uint8_t cause)
{
  uint32_t id;
  uint8_t  data[CAN_DRIVER_MAX_DATA_LEN];
  uint8_t  len;
  uint8_t  step = g_verify_fail_step;

  /* 现场落盘：只写 reserved 字段，ota_metadata_t 仍 272B、META_VERSION=1
   * 不变（version 严格相等校验，结构/版本改动会失效双副本并要求两工程
   * 联烧；reserved 字段 APP 侧写 0/不读，零漂移）。
   *   reserved1      = [cause(高字节) | fail_step(低字节)]
   *   reserved2[0]   = last_boot_reason 进 safe mode 前快照
   *   reserved2[1]   = 0xA5 safe-mode-entered 标记
   * boot_metadata_save 内部已关中断（boot_metadata.c 咽喉点），CRC 重算。 */
  g_meta.reserved1    = (uint16_t)(((uint16_t)cause << 8) | step);
  g_meta.reserved2[0] = g_meta.last_boot_reason;
  g_meta.reserved2[1] = 0xA5U;
  (void)boot_metadata_save(&g_meta);

  /* CAN 复用工程内已链接的 can_driver（md_k_can，含 0x18DA0D03 精确
   * 过滤器）。Boot 上电路径此前从未初始化 CAN，此处属上电初始化语义，
   * can_driver_init 合法；运行期/恢复路径仍禁止调用（固件运行时规则 1）。
   * 16KB Boot 区约束：不新增驱动，仅本文件新增轮询逻辑（约 0.4KB，
   * CAN 驱动链首次带入另占约 3~6KB，编译后须核对 .map 剩余空间）。 */
  can_driver_init();

  while (1)
  {
    while (can_driver_recv(&id, data, &len) == 0)
    {
      if (id != CAN_ID_UDS_REQUEST)
      {
        continue;
      }

      /* 探测帧匹配：ISO-TP SF (03 22 21 13) 或裸 UDS (22 21 13) */
      if (((len >= 4U) && (data[0] == 0x03U) && (data[1] == 0x22U) &&
           (data[2] == 0x21U) && (data[3] == 0x13U)) ||
          ((len >= 3U) && (data[0] == 0x22U) && (data[1] == 0x21U) &&
           (data[2] == 0x13U)))
      {
        /* ISO-TP SF: ZCANPRO uds_request 认 PCI，不能发裸 62 21 13 FE */
        uint8_t sf[8];

        sf[0] = 0x05U;
        sf[1] = 0x62U;
        sf[2] = 0x21U;
        sf[3] = 0x13U;
        sf[4] = 0xFEU;
        sf[5] = step;
        sf[6] = 0xCCU;
        sf[7] = 0xCCU;
        (void)can_driver_send(CAN_ID_UDS_RESPONSE, sf, 8U);
      }
    }
  }
}
