/**
  **************************************************************************
  * @file     timer_drv.c
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

/* includes ------------------------------------------------------------------*/
#include "timer_drv.h"

/* private define ------------------------------------------------------------*/
#define TIMER_STATE_STOPPED           0x00
#define TIMER_STATE_RUNNING           0x01
#define TIMER_STATE_EXPIRED           0x02

/* private typedef -----------------------------------------------------------*/
/**
 * @brief  software timer control block
 */
typedef struct
{
  volatile uint8_t  state;        /*!< timer state: stopped / running / expired */
  uint8_t           auto_reload;  /*!< 0 = one-shot, 1 = auto-reload */
  uint16_t          reserved;     /*!< padding for alignment */
  volatile uint32_t counter;      /*!< current count in ms */
  uint32_t          period;       /*!< timer period in ms */
  void              (*callback)(void); /*!< callback function pointer */
} timer_ctrl_t;

/* private variables ---------------------------------------------------------*/
static volatile uint32_t sys_tick_ms = 0;               /*!< system tick counter in ms */
static timer_ctrl_t      timer_pool[TIMER_DRV_MAX_TIMERS]; /*!< static timer pool */

/* exported functions --------------------------------------------------------*/

/**
 * @brief  initialize software timer module and configure SysTick to 1ms interrupt
 * @param  none
 * @retval none
 */
void timer_drv_init(void)
{
  uint8_t i;

  /* reset system tick */
  sys_tick_ms = 0;

  /* reset timer pool */
  for (i = 0; i < TIMER_DRV_MAX_TIMERS; i++)
  {
    timer_pool[i].state       = TIMER_STATE_STOPPED;
    timer_pool[i].auto_reload = 0;
    timer_pool[i].counter     = 0;
    timer_pool[i].period      = 0;
    timer_pool[i].callback    = (void (*)(void))0;
  }

  /* configure SysTick to generate 1ms interrupt using CMSIS standard API */
  SysTick_Config(system_core_clock / 1000);
}

/**
 * @brief  create a software timer
 * @param  period_ms: timer period in milliseconds
 * @param  cb: callback function pointer (called when timer expires)
 * @param  auto_reload: 0 = one-shot, 1 = auto-reload (periodic)
 * @retval timer id (0 ~ TIMER_DRV_MAX_TIMERS-1) on success, TIMER_INVALID_ID on failure
 */
uint8_t timer_create(uint32_t period_ms, void (*cb)(void), uint8_t auto_reload)
{
  uint8_t id;

  /* validate parameters */
  if ((period_ms == 0) || (cb == (void (*)(void))0))
  {
    return TIMER_INVALID_ID;
  }

  /* find a free slot */
  for (id = 0; id < TIMER_DRV_MAX_TIMERS; id++)
  {
    if (timer_pool[id].callback == (void (*)(void))0)
    {
      timer_pool[id].state       = TIMER_STATE_STOPPED;
      timer_pool[id].auto_reload = auto_reload;
      timer_pool[id].counter     = 0;
      timer_pool[id].period      = period_ms;
      timer_pool[id].callback    = cb;

      return id;
    }
  }

  /* no free slot available */
  return TIMER_INVALID_ID;
}

/**
 * @brief  start a software timer
 * @param  timer_id: timer id returned by timer_create()
 * @retval none
 */
void timer_start(uint8_t timer_id)
{
  if ((timer_id >= TIMER_DRV_MAX_TIMERS) ||
      (timer_pool[timer_id].callback == (void (*)(void))0))
  {
    return;
  }

  timer_pool[timer_id].counter = 0;
  timer_pool[timer_id].state   = TIMER_STATE_RUNNING;
}

/**
 * @brief  stop a software timer
 * @param  timer_id: timer id returned by timer_create()
 * @retval none
 */
void timer_stop(uint8_t timer_id)
{
  if (timer_id >= TIMER_DRV_MAX_TIMERS)
  {
    return;
  }

  timer_pool[timer_id].state = TIMER_STATE_STOPPED;
}

/**
 * @brief  reset timer counter to zero (restart counting from now)
 * @param  timer_id: timer id returned by timer_create()
 * @retval none
 */
void timer_reset(uint8_t timer_id)
{
  if (timer_id >= TIMER_DRV_MAX_TIMERS)
  {
    return;
  }

  timer_pool[timer_id].counter = 0;
}

/**
 * @brief  check if a timer is currently running
 * @param  timer_id: timer id returned by timer_create()
 * @retval 1 = running, 0 = stopped or invalid
 */
uint8_t timer_is_running(uint8_t timer_id)
{
  if (timer_id >= TIMER_DRV_MAX_TIMERS)
  {
    return 0;
  }

  return (timer_pool[timer_id].state == TIMER_STATE_RUNNING) ? 1 : 0;
}

/**
 * @brief  get current system tick count in milliseconds
 * @param  none
 * @retval system tick in ms (wraps around at 0xFFFFFFFF)
 */
uint32_t timer_get_tick(void)
{
  return sys_tick_ms;
}

/**
 * @brief  increment system tick counter, called from SysTick_Handler
 * @param  none
 * @retval none
 */
void timer_tick_inc(void)
{
  uint8_t i;

  sys_tick_ms++;

  /* increment running timers and mark expired ones */
  for (i = 0; i < TIMER_DRV_MAX_TIMERS; i++)
  {
    if (timer_pool[i].state == TIMER_STATE_RUNNING)
    {
      timer_pool[i].counter++;
      if (timer_pool[i].counter >= timer_pool[i].period)
      {
        /* mark as expired, callback will be executed in timer_poll() */
        timer_pool[i].state = TIMER_STATE_EXPIRED;
      }
    }
  }
}

/**
 * @brief  poll all software timers, execute callbacks for expired timers
 * @note   must be called periodically from main loop
 * @param  none
 * @retval none
 */
void timer_poll(void)
{
  uint8_t i;

  for (i = 0; i < TIMER_DRV_MAX_TIMERS; i++)
  {
    if (timer_pool[i].state == TIMER_STATE_EXPIRED)
    {
      /* execute callback in main loop context */
      if (timer_pool[i].callback != (void (*)(void))0)
      {
        timer_pool[i].callback();
      }

      if (timer_pool[i].auto_reload)
      {
        /* auto-reload: reset counter and continue running */
        timer_pool[i].counter = 0;
        timer_pool[i].state   = TIMER_STATE_RUNNING;
      }
      else
      {
        /* one-shot: stop the timer */
        timer_pool[i].state = TIMER_STATE_STOPPED;
      }
    }
  }
}
