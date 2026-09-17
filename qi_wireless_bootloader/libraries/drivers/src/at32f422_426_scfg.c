/**
  **************************************************************************
  * @file     at32f422_426_scfg.c
  * @brief    contains all the functions for the system config firmware library
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

#include "at32f422_426_conf.h"

/** @addtogroup AT32F422_426_periph_driver
  * @{
  */

/** @defgroup SCFG
  * @brief SCFG driver modules
  * @{
  */

#ifdef SCFG_MODULE_ENABLED

/** @defgroup SCFG_private_functions
  * @{
  */

/**
  * @brief  scfg reset
  * @param  none
  * @retval none
  */
void scfg_reset(void)
{
  crm_periph_reset(CRM_SCFG_PERIPH_RESET, TRUE);
  crm_periph_reset(CRM_SCFG_PERIPH_RESET, FALSE);
}

/**
  * @brief  scfg infrared config
  * @param  source
  *         this parameter can be one of the following values:
  *         - SCFG_IR_SOURCE_TMR16
  *         - SCFG_IR_SOURCE_USART1
  *         - SCFG_IR_SOURCE_USART2
  * @param  polarity
  *         this parameter can be one of the following values:
  *         - SCFG_IR_POLARITY_NO_AFFECTE
  *         - SCFG_IR_POLARITY_REVERSE
  * @retval none
  */
void scfg_infrared_config(scfg_ir_source_type source, scfg_ir_polarity_type polarity)
{
  SCFG->cfg1_bit.ir_src_sel = source;
  SCFG->cfg1_bit.ir_pol = polarity;
}

/**
  * @brief  scfg memory address mapping get
  * @param  none
  * @retval return parameter can be one of the following values:
  *         - SCFG_MEM_MAP_MAIN_MEMORY
  *         - SCFG_MEM_MAP_BOOT_MEMORY
  *         - SCFG_MEM_MAP_INTERNAL_SRAM
  */
uint8_t scfg_mem_map_get(void)
{
  return (uint8_t)SCFG->cfg1_bit.mem_map_sel ;
}

/**
  * @brief  scfg pa11/12 pin remap
  * @param  pin_remap
  *         this parameter can be one of the following values:
  *         - SCFG_PA11PA12_NO_REMAP
  *         - SCFG_PA11PA12_TO_PA9PA10
  * @retval none
  */
void scfg_pa11pa12_pin_remap(scfg_pa11pa12_remap_type pin_remap)
{
  SCFG->cfg1_bit.pa11_12_rmp = pin_remap;
}

/**
  * @brief  scfg adc dma channel remap
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_ADC_TO_DMA_CHANNEL_1
  *         - SCFG_ADC_TO_DMA_CHANNEL_2
  * @retval none
  */
void scfg_adc_dma_channel_remap(scfg_adc_dma_remap_type dma_channel)
{
  SCFG->cfg1_bit.adc_dma_rmp = dma_channel;
}

/**
  * @brief  scfg usart1 tx dma channel remap
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_USART1_TX_TO_DMA_CHANNEL_2
  *         - SCFG_USART1_TX_TO_DMA_CHANNEL_4
  * @retval none
  */
void scfg_usart1_tx_dma_channel_remap(scfg_usart1_tx_dma_remap_type dma_channel)
{
  SCFG->cfg1_bit.usart1_tx_dma_rmp = dma_channel;
}

/**
  * @brief  scfg usart1 rx dma channel remap
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_USART1_RX_TO_DMA_CHANNEL_3
  *         - SCFG_USART1_RX_TO_DMA_CHANNEL_5
  * @retval none
  */
void scfg_usart1_rx_dma_channel_remap(scfg_usart1_rx_dma_remap_type dma_channel)
{
  SCFG->cfg1_bit.usart1_rx_dma_rmp = dma_channel;
}

/**
  * @brief  scfg tmr1 dma channel remap
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_TMR1_DMA_NO_REMAP (tmr1_ch1,tmr1_ch2 and tmr1_ch3 dma channel on dma1 channel2/3/5)
  *         - SCFG_TMR1_DMA_REMAP (tmr1_ch1,tmr1_ch2 and tmr1_ch3 dma channel remap to dma1 channe6)
  * @retval none
  */
void scfg_tmr1_dma_channel_remap(scfg_tmr1_dma_remap_type dma_channel)
{
  SCFG->cfg1_bit.tmr1_dma_rmp = dma_channel;
}

/**
  * @brief  scfg tmr3 dma channel remap
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_TMR3_DMA_NO_REMAP (tmr3_ch1 and tmr3_trig dma channel on dma1 channel4)
  *         - SCFG_TMR3_DMA_REMAP (tmr3_ch1 and tmr3_trig dma channel remap to dma1 channe6)
  * @retval none
  */
void scfg_tmr3_dma_channel_remap(scfg_tmr3_dma_remap_type dma_channel)
{
  SCFG->cfg1_bit.tmr3_dma_rmp = dma_channel;
}

/**
  * @brief  scfg tmr16 dma channel remap
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_TMR16_TO_DMA_CHANNEL_3
  *         - SCFG_TMR16_TO_DMA_CHANNEL_4
  * @retval none
  */
void scfg_tmr16_dma_channel_remap(scfg_tmr16_dma_remap_type dma_channel)
{
  SCFG->cfg1_bit.tmr16_dma_rmp = dma_channel;
}

/**
  * @brief  scfg tmr16 dma channel remap2
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_TMR16_DMA_NO_REMAP (tmr16_ch1 and tmr16_overflow dma channel depends on tmr16_dma_rmp bit)
  *         - SCFG_TMR16_DMA_REMAP (tmr16_ch1 and tmr16_overflow dma channel remap to dma1 channel6)
  * @retval none
  */
void scfg_tmr16_dma_channel_remap2(scfg_tmr16_dma_remap2_type dma_channel)
{
  SCFG->cfg1_bit.tmr16_dma_rmp2 = dma_channel;
}

/**
  * @brief  scfg tmr17 dma channel remap
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_TMR17_TO_DMA_CHANNEL_1
  *         - SCFG_TMR17_TO_DMA_CHANNEL_2
  * @retval none
  */
void scfg_tmr17_dma_channel_remap(scfg_tmr17_dma_remap_type dma_channel)
{
  SCFG->cfg1_bit.tmr17_dma_rmp = dma_channel;
}

/**
  * @brief  scfg tmr17 dma channel remap2
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_TMR17_DMA_NO_REMAP (tmr17_ch1 and tmr17_overflow dma channel depends on tmr17_dma_rmp bit)
  *         - SCFG_TMR17_DMA_REMAP (tmr17_ch1 and tmr17_overflow dma channel remap to dma1 channel7)
  * @retval none
  */
void scfg_tmr17_dma_channel_remap2(scfg_tmr17_dma_remap2_type dma_channel)
{
  SCFG->cfg1_bit.tmr17_dma_rmp2 = dma_channel;
}

/**
  * @brief  scfg spi2 dma channel remap
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_SPI2_DMA_NO_REMAP (spi2 rx/tx dma channel on dma1 channel4/5)
  *         - SCFG_SPI2_DMA_REMAP (spi2 rx/tx dma channel remap to dma1 channel6/7)
  * @retval none
  */
void scfg_spi2_dma_channel_remap(scfg_spi2_dma_remap_type dma_channel)
{
  SCFG->cfg1_bit.spi2_dma_rmp = dma_channel;
}

/**
  * @brief  scfg usart2 dma channel remap
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_USART2_DMA_NO_REMAP (usart2 rx/tx dma channel on dma1 channel5/4)
  *         - SCFG_USART2_DMA_REMAP (usart2 rx/tx dma channel remap to dma1 channel6/7)
  * @retval none
  */
void scfg_usart2_dma_channel_remap(scfg_usart2_dma_remap_type dma_channel)
{
  SCFG->cfg1_bit.usart2_dma_rmp = dma_channel;
}

/**
  * @brief  scfg i2c1 dma channel remap
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_I2C1_DMA_NO_REMAP (i2c1 rx/tx dma channel on dma1 channel3/2)
  *         - SCFG_I2C1_DMA_REMAP (i2c1 rx/tx dma channel remap to dma1 channel7/6)
  * @retval none
  */
void scfg_i2c1_dma_channel_remap(scfg_i2c1_dma_remap_type dma_channel)
{
  SCFG->cfg1_bit.i2c1_dma_rmp = dma_channel;
}

/** 
  * @brief  can timestamp counting source set 
  * @param  usart_index
  *         this parameter can be one of the following values:
  *         - SCFG_CAN1
  * @param  count_source
  *         this parameter can be one of the following values:
  *         - SCFG_CAN_TIMESTAMP_TMR3
  *         - SCFG_CAN_TIMESTAMP_TMR4
  * @retval none
  */
void scfg_can_timestamp_source_set(scfg_can_type can_index, scfg_can_timestamp_source_type source)
{
  switch (can_index)
  {
    case SCFG_CAN1:
      SCFG->cfg2_bit.can1_tst_sel = source;
      break;
  }
}

/**
  * @brief  scfg nrst remap status get
  * @param  none
  * @retval return parameter can be one of the following values:
  *         - SCFG_NRST_RMP_NRST
  *         - SCFG_NRST_RMP_PF2
  */
uint8_t scfg_nrst_rmp_sts_get(void)
{
  return (uint8_t)SCFG->iocfg_bit.nrst_rmp;
}

/**
  * @brief  select the gpio pin used as exint line.
  * @param  port_source:
  *         select the gpio port to be used as source for exint lines.
  *         this parameter can be one of the following values:
  *         - SCFG_PORT_SOURCE_GPIOA
  *         - SCFG_PORT_SOURCE_GPIOB
  *         - SCFG_PORT_SOURCE_GPIOC
  *         - SCFG_PORT_SOURCE_GPIOF
  * @param  pin_source:
  *         specifies the exint line to be configured.
  *         this parameter can be one of the following values:
  *         - SCFG_PINS_SOURCE0
  *         - SCFG_PINS_SOURCE1
  *         - SCFG_PINS_SOURCE2
  *         - SCFG_PINS_SOURCE3
  *         - SCFG_PINS_SOURCE4
  *         - SCFG_PINS_SOURCE5
  *         - SCFG_PINS_SOURCE6
  *         - SCFG_PINS_SOURCE7
  *         - SCFG_PINS_SOURCE8
  *         - SCFG_PINS_SOURCE9
  *         - SCFG_PINS_SOURCE10
  *         - SCFG_PINS_SOURCE11
  *         - SCFG_PINS_SOURCE12
  *         - SCFG_PINS_SOURCE13
  *         - SCFG_PINS_SOURCE14
  *         - SCFG_PINS_SOURCE15
  * @retval none
  */
void scfg_exint_line_config(scfg_port_source_type port_source, scfg_pins_source_type pin_source)
{
  uint32_t tmp = 0x00;
  tmp = ((uint32_t)0x0F) << (0x04 * (pin_source & (uint8_t)0x03));

  switch (pin_source >> 0x02)
  {
    case 0:
      SCFG->exintc1 &= ~tmp;
      SCFG->exintc1 |= (((uint32_t)port_source) << (0x04 * (pin_source & (uint8_t)0x03)));
      break;

    case 1:
      SCFG->exintc2 &= ~tmp;
      SCFG->exintc2 |= (((uint32_t)port_source) << (0x04 * (pin_source & (uint8_t)0x03)));
      break;

    case 2:
      SCFG->exintc3 &= ~tmp;
      SCFG->exintc3 |= (((uint32_t)port_source) << (0x04 * (pin_source & (uint8_t)0x03)));
      break;

    case 3:
      SCFG->exintc4 &= ~tmp;
      SCFG->exintc4 |= (((uint32_t)port_source) << (0x04 * (pin_source & (uint8_t)0x03)));
      break;

    default:
      break;
  }
}

/**
  * @brief  scfg dma channel remap
  * @param  dma_channel
  *         this parameter can be one of the following values:
  *         - SCFG_ADC_DMA_RMP (adc dma request remap to dma1 channel2)
  *         - SCFG_USART1_TX_DMA_RMP (usart1 tx dma request remap to dma1 channel4)
  *         - SCFG_USART1_RX_DMA_RMP (usart1 rx dma request remap to dma1 channel5)
  *         - SCFG_TMR16_DMA_RMP (tmr16_ch1/tmr16_overflow dma request remap to dma1 channel4)
  *         - SCFG_TMR17_DMA_RMP (tmr17_ch1/tmr17_overflow dma request remap to dma1 channel2)
  *         - SCFG_TMR16_DMA_RMP2 (tmr16_ch1/tmr16_overflow dma request remap to dma1 channel6)
  *         - SCFG_TMR17_DMA_RMP2 (tmr17_ch1/tmr17_overflow dma request remap to dma1 channel7)
  *         - SCFG_SPI2_DMA_RMP (spi2 rx/tx dma request remap to dma1 channel6/7)
  *         - SCFG_USART2_DMA_RMP (usart2 rx/tx dma request remap to dma1 channel6/7)
  *         - SCFG_I2C1_DMA_RMP (i2c1 rx/tx dma request remap to dma1 channel7/6)
  *         - SCFG_TMR1_DMA_RMP (tmr1_ch1/tmr1_ch2/tmr1_ch3 dma request remap to dma1 channel6)
  *         - SCFG_TMR3_DMA_RMP (tmr3_ch1/tmr3_trig dma request remap to dma1 channel6)
  * @param  new_state: new state of the dma remap.
  *         this parameter can be: TRUE or FALSE.
  * @retval none
  */
void scfg_dma_channel_remap(uint32_t dma_channel, confirm_state new_state)
{
  if(new_state == TRUE)
    SCFG->cfg1 |=  dma_channel;
  else
    SCFG->cfg1 &= ~dma_channel;
}

/**
  * @}
  */

#endif

/**
  * @}
  */

/**
  * @}
  */
