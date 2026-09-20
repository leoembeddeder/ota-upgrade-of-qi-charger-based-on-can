/**
  **************************************************************************
  * @file     timer_drv.h
  * @brief    software timer driver based on SysTick 1ms tick
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
#ifndef __TIMER_DRV_H
#define __TIMER_DRV_H

#ifdef __cplusplus
extern "C" {
#endif

/* includes ------------------------------------------------------------------*/
#include "at32f422_426.h"

/* exported constants --------------------------------------------------------*/
#define TIMER_DRV_MAX_TIMERS          16    /*!< max number of software timers */

/* exported macro ------------------------------------------------------------*/
#define TIMER_INVALID_ID              0xFF  /*!< invalid timer id */

/* exported functions -------------------------------------------------------*/

/**
 * @brief  initialize software timer module and configure SysTick to 1ms interrupt
 * @param  none
 * @retval none
 */
void timer_drv_init(void);

/**
 * @brief  create a software timer
 * @param  period_ms: timer period in milliseconds
 * @param  cb: callback function pointer (called when timer expires)
 * @param  auto_reload: 0 = one-shot, 1 = auto-reload (periodic)
 * @retval timer id (0 ~ TIMER_DRV_MAX_TIMERS-1) on success, TIMER_INVALID_ID on failure
 */
uint8_t timer_create(uint32_t period_ms, void (*cb)(void), uint8_t auto_reload);

/**
 * @brief  start a software timer
 * @param  timer_id: timer id returned by timer_create()
 * @retval none
 */
void timer_start(uint8_t timer_id);

/**
 * @brief  stop a software timer
 * @param  timer_id: timer id returned by timer_create()
 * @retval none
 */
void timer_stop(uint8_t timer_id);

/**
 * @brief  reset timer counter to zero (restart counting from now)
 * @param  timer_id: timer id returned by timer_create()
 * @retval none
 */
void timer_reset(uint8_t timer_id);

/**
 * @brief  check if a timer is currently running
 * @param  timer_id: timer id returned by timer_create()
 * @retval 1 = running, 0 = stopped or invalid
 */
uint8_t timer_is_running(uint8_t timer_id);

/**
 * @brief  get current system tick count in milliseconds
 * @param  none
 * @retval system tick in ms (wraps around at 0xFFFFFFFF)
 */
uint32_t timer_get_tick(void);

/**
 * @brief  increment system tick counter, called from SysTick_Handler
 * @param  none
 * @retval none
 */
void timer_tick_inc(void);

/**
 * @brief  poll all software timers, execute callbacks for expired timers
 * @note   must be called periodically from main loop
 * @param  none
 * @retval none
 */
void timer_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* __TIMER_DRV_H */
