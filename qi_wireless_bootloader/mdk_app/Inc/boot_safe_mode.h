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

#endif /* __BOOT_SAFE_MODE_H */
