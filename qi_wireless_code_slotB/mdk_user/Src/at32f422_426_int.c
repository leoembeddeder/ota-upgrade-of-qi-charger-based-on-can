/**
  **************************************************************************
  * @file     at32f422_426_int.c
  * @brief    main interrupt service routines.
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
#include "at32f422_426_int.h"
#include "timer_drv.h"
#include "can_driver.h"
#include "qi_uart.h"


/** @addtogroup AT32F426_periph_examples
  * @{
  */

/** @addtogroup 426_CAN_communication_mode
  * @{
  */

/**
  * @brief  this function handles nmi exception.
  * @param  none
  * @retval none
  */
void NMI_Handler(void)
{
}

/**
  * @brief  this function handles hard fault exception.
  * @param  none
  * @retval none
  */
void HardFault_Handler(void)
{
  *(volatile uint32_t *)0x20000000 = 0xFAUL;
  NVIC_SystemReset();
}

/**
  * @brief  this function handles memory manage exception.
  * @param  none
  * @retval none
  */
void MemManage_Handler(void)
{
  *(volatile uint32_t *)0x20000000 = 0xFAUL;
  NVIC_SystemReset();
}

/**
  * @brief  this function handles bus fault exception.
  * @param  none
  * @retval none
  */
void BusFault_Handler(void)
{
  *(volatile uint32_t *)0x20000000 = 0xFAUL;
  NVIC_SystemReset();
}

/**
  * @brief  this function handles usage fault exception.
  * @param  none
  * @retval none
  */
void UsageFault_Handler(void)
{
  *(volatile uint32_t *)0x20000000 = 0xFAUL;
  NVIC_SystemReset();
}

/**
  * @brief  this function handles svcall exception.
  * @param  none
  * @retval none
  */
void SVC_Handler(void)
{
}

/**
  * @brief  this function handles debug monitor exception.
  * @param  none
  * @retval none
  */
void DebugMon_Handler(void)
{
}

/**
  * @brief  this function handles pendsv_handler exception.
  * @param  none
  * @retval none
  */
void PendSV_Handler(void)
{
}

/**
  * @brief  this function handles systick handler.
  * @param  none
  * @retval none
  */
void SysTick_Handler(void)
{
  timer_tick_inc();
}

/**
  *  @brief  can1 interrupt function rx
  *  @param  none
  *  @retval none
  */
void CAN1_RX_IRQHandler(void)
{
  can_driver_rx_irq_handler();
}

/**
  *  @brief  can1 interrupt function error
  *  @param  none
  *  @retval none
  */
void CAN1_ERR_IRQHandler(void)
{
  can_driver_err_irq_handler();
}

/**
 * @brief  USART2 interrupt function (Qi UART RX)
 * @param  none
 * @retval none
 */
void USART2_IRQHandler(void)
{
  qi_uart_rx_irq_handler();
}



/**
  * @}
  */

/**
  * @}
  */


