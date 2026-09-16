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
 * @brief  enter safe mode: initialize CAN and wait for OTA download
 * @note   entered when no bootable slot remains. Does not return.
 * @param  none
 * @retval none (does not return)
 */
void enter_safe_mode(void);

#endif /* __BOOT_SAFE_MODE_H */
