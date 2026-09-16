/**
  **************************************************************************
  * @file     lifecycle.h
  * @brief    Lifecycle broadcast module for peripheral status reporting
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

/* define to prevent recursive inclusion -------------------------------------*/
#ifndef __LIFECYCLE_H
#define __LIFECYCLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* includes ------------------------------------------------------------------*/
#include "at32f422_426.h"

/* ========================================================================== */
/*  Lifecycle state definitions (per 通用CAN协议规范 section 10)              */
/* ========================================================================== */

#define LIFECYCLE_BOOTUP       0x01U   /*!< powered on or reset */
#define LIFECYCLE_INIT         0x02U   /*!< initializing HW/SW */
#define LIFECYCLE_OPERATIONAL  0x03U   /*!< ready for normal operation */
#define LIFECYCLE_DEGRADED     0x04U   /*!< operating with degraded capability */
#define LIFECYCLE_FAULT        0x05U   /*!< critical fault present */
#define LIFECYCLE_SHUTDOWN     0x06U   /*!< preparing for power-off */

/* ========================================================================== */
/*  Exported functions                                                       */
/* ========================================================================== */

/**
 * @brief  initialize lifecycle module and send BOOTUP broadcast
 * @note   must be called after can_driver_init().
 *         sends the BOOTUP broadcast immediately.
 * @param  none
 * @retval none
 */
void lifecycle_init(void);

/**
 * @brief  set lifecycle state and send immediate broadcast
 * @note   call this whenever the device state changes.
 *         for OPERATIONAL and DEGRADED, also enables periodic broadcast.
 * @param  state: new lifecycle state (LIFECYCLE_BOOTUP .. LIFECYCLE_SHUTDOWN)
 * @retval none
 */
void lifecycle_set_state(uint8_t state);

/**
 * @brief  poll lifecycle for periodic broadcast
 * @note   must be called from the main loop.
 *         sends periodic broadcast every 1000ms when in OPERATIONAL or DEGRADED.
 * @param  none
 * @retval none
 */
void lifecycle_poll(void);

/**
 * @brief  get current lifecycle state
 * @retval current lifecycle state value
 */
uint8_t lifecycle_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* __LIFECYCLE_H */
