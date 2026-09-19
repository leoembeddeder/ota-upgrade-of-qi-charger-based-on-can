/**
  **************************************************************************
  * @file     boot_safe_mode.h
  * @brief    Safe mode (UDS OTA download) interface
  **************************************************************************
  */

#ifndef __BOOT_SAFE_MODE_H
#define __BOOT_SAFE_MODE_H

#include <stdint.h>

/**
 * @brief  enter safe mode: persist failure context to metadata reserved
 *         fields, init CAN, answer safe-mode probe frames (frame format
 *         documented in boot_safe_mode.c, keep in sync with
 *         python_tools/zcanpro_ext_ota_auto.py)
 * @note   entered when no bootable slot remains. Does not return.
 * @param  cause: 0x01 = select_boot_slot found no valid slot,
 *                0x02 = both slots failed image/vector verification
 * @retval none (does not return)
 */
void enter_safe_mode(uint8_t cause);

/* ===== Boot 诊断标记帧接口（观察不干预，2026-09-20 用户授权诊断版）=====
 * 开机决策链四个标记点，每点发一帧 DLC=8 扩展帧，CAN ID=0x18FF480D
 * （协议未占用：0x18DA0D03/0x18DA030D/0x18DB33F1/0x18FF260D/0x18FF270D
 * 已占用），帧=[标记 0xA1~0xA4][数据...]，未用字节 0xCC 填充：
 *   M1 0xA1: [active_slot][meta_src 0主/1备/2默认][magic_ok][ver_ok]
 *            [crc_ok] —— boot_metadata_init 读完记录后
 *   M2 0xA2: [slot][ret] —— select_boot_slot 返回后；失败路径 slot=metadata
 *            实际槽字节（原始值），ret=0xFF（=返回值 -1）
 *   M3 0xA3: [pass][fail_step][slot] —— try_boot_slot 验签后（选中槽与
 *            回落对面槽两次都发）；pass=1过验/0失败，fail_step=
 *            g_verify_fail_step（过验=0；向量门失败时 verify 已过故=0）
 *   M4 0xA4: [addr LE 4B] —— boot_jump_to_app 跳转前（发送于 CAN 关闭前）
 * 发送=polling+有界等待（每帧 ≤3ms，失败/超时即弃帧继续决策链）；
 * 诊断绝不因 CAN 发不出去卡死 Boot 开机。safe mode 既有标记帧
 * （62 21 13 FE fail_step / ABT 心跳）零触碰。 */
#define BOOT_DIAG_CAN_ID  0x18FF480DU

/** @brief 诊断用 CAN 提前初始化（main.c step2，timer 之后/metadata 之前） */
void boot_diag_can_init(void);
/** @brief M1：metadata 读完记录后（meta=ota_metadata_t*） */
void boot_diag_m1(const void *meta);
/** @brief M2：select_boot_slot 返回后 */
void boot_diag_m2(int8_t ret, uint8_t slot);
/** @brief M3：try_boot_slot 验签后 */
void boot_diag_m3(uint8_t pass, uint8_t fail_step, uint8_t slot);
/** @brief M4：boot_jump_to_app 跳转前 */
void boot_diag_m4(uint32_t app_addr);

/** @brief boot_metadata_init 记录：0=主区生效 1=备区恢复 2=defaults；0xFF=未记录 */
extern uint8_t g_diag_meta_src;

#endif /* __BOOT_SAFE_MODE_H */
