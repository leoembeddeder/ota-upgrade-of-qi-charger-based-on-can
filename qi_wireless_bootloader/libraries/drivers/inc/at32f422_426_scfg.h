/**
  **************************************************************************
  * @file     at32f422_426_scfg.h
  * @brief    at32f422_426 system config header file
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
#ifndef __AT32F422_426_SCFG_H
#define __AT32F422_426_SCFG_H

#ifdef __cplusplus
extern "C" {
#endif


/* Includes ------------------------------------------------------------------*/
#include "at32f422_426.h"

/** @addtogroup AT32F422_426_periph_driver
  * @{
  */

/** @addtogroup SCFG
  * @{
  */

#define SCFG_REG(value)                  PERIPH_REG(SCFG_CMP_BASE, value)
#define SCFG_REG_BIT(value)              PERIPH_REG_BIT(value)

/** @addtogroup scfg_dma_remap_definition
  * @brief dma remap
  * @{
  */

#define SCFG_ADC_DMA_RMP                 ((uint32_t)0x00000100) /*!< adc dma request remap to dma1 channel2 */
#define SCFG_USART1_TX_DMA_RMP           ((uint32_t)0x00000200) /*!< usart1 tx dma request remap to dma1 channel4 */
#define SCFG_USART1_RX_DMA_RMP           ((uint32_t)0x00000400) /*!< usart1 rx dma request remap to dma1 channel5 */
#define SCFG_TMR16_DMA_RMP               ((uint32_t)0x00000800) /*!< tmr16_ch1/tmr16_overflow dma request remap to dma1 channel4 */
#define SCFG_TMR17_DMA_RMP               ((uint32_t)0x00001000) /*!< tmr17_ch1/tmr17_overflow dma request remap to dma1 channel2 */
#define SCFG_TMR16_DMA_RMP2              ((uint32_t)0x00002000) /*!< tmr16_ch1/tmr16_overflow dma request remap to dma1 channel6 */
#define SCFG_TMR17_DMA_RMP2              ((uint32_t)0x00004000) /*!< tmr17_ch1/tmr17_overflow dma request remap to dma1 channel7 */
#define SCFG_SPI2_DMA_RMP                ((uint32_t)0x01000000) /*!< spi2 rx/tx dma request remap to dma1 channel6/7 */
#define SCFG_USART2_DMA_RMP              ((uint32_t)0x02000000) /*!< usart2 rx/tx dma request remap to dma1 channel6/7 */
#define SCFG_I2C1_DMA_RMP                ((uint32_t)0x08000000) /*!< i2c1 rx/tx dma request remap to dma1 channel7/6 */
#define SCFG_TMR1_DMA_RMP                ((uint32_t)0x10000000) /*!< tmr1_ch1/tmr1_ch2/tmr1_ch3 dma request remap to dma1 channel6 */
#define SCFG_TMR3_DMA_RMP                ((uint32_t)0x40000000) /*!< tmr3_ch1/tmr3_trig dma request remap to dma1 channel6 */

/**
  * @}
  */

/** @defgroup SCFG_exported_types
  * @{
  */

/**
  * @brief scfg pa11 pa12 pin remap type
  */
typedef enum
{
  SCFG_PA11PA12_NO_REMAP                 = 0x00, /* pa11 pa12 pin no remap */
  SCFG_PA11PA12_TO_PA9PA10               = 0x01, /* pa11 pa12 pin remap pa9 pa10*/
} scfg_pa11pa12_remap_type;

/**
  * @brief scfg adc dma channel remap type
  */
typedef enum
{
  SCFG_ADC_TO_DMA_CHANNEL_1              = 0x00, /* adc config to dma channel 1 */
  SCFG_ADC_TO_DMA_CHANNEL_2              = 0x01, /* adc config to dma channel 2*/
} scfg_adc_dma_remap_type;

/**
  * @brief scfg usart1 tx dma channel remap type
  */
typedef enum
{
  SCFG_USART1_TX_TO_DMA_CHANNEL_2        = 0x00, /* usart1 tx config to dma channel 2 */
  SCFG_USART1_TX_TO_DMA_CHANNEL_4        = 0x01, /* usart1 tx config to dma channel 4 */
} scfg_usart1_tx_dma_remap_type;

/**
  * @brief scfg usart1 rx dma channel remap type
  */
typedef enum
{
  SCFG_USART1_RX_TO_DMA_CHANNEL_3        = 0x00, /* usart1 rx config to dma channel 3 */
  SCFG_USART1_RX_TO_DMA_CHANNEL_5        = 0x01, /* usart1 rx config to dma channel 5 */
} scfg_usart1_rx_dma_remap_type;

/**
  * @brief scfg usart2 dma remap type
  */
typedef enum
{
  SCFG_USART2_DMA_NO_REMAP               = 0x00, /* usart2 config to dma no remap */
  SCFG_USART2_DMA_REMAP                  = 0x01, /* usart2 config to dma remap */
} scfg_usart2_dma_remap_type;

/**
  * @brief scfg tmr1 dma remap type
  */
typedef enum
{
  SCFG_TMR1_DMA_NO_REMAP                 = 0x00, /* tmr1 config to dma no remap */
  SCFG_TMR1_DMA_REMAP                    = 0x01, /* tmr1 config to dma remap */
} scfg_tmr1_dma_remap_type;

/**
  * @brief scfg tmr3 dma remap type
  */
typedef enum
{
  SCFG_TMR3_DMA_NO_REMAP                 = 0x00, /* tmr3 config to dma no remap */
  SCFG_TMR3_DMA_REMAP                    = 0x01, /* tmr3 config to dma remap */
} scfg_tmr3_dma_remap_type;

/**
  * @brief scfg tmr16 dma channel remap type
  */
typedef enum
{
  SCFG_TMR16_TO_DMA_CHANNEL_3            = 0x00, /* tmr16 config to dma channel 3 */
  SCFG_TMR16_TO_DMA_CHANNEL_4            = 0x01, /* tmr16 config to dma channel 4 */
} scfg_tmr16_dma_remap_type;

/**
  * @brief scfg tmr16 dma channel remap2 type
  */
typedef enum
{
  SCFG_TMR16_DMA_NO_REMAP                = 0x00, /* tmr16 config to dma no remap */
  SCFG_TMR16_DMA_REMAP                   = 0x01, /* tmr16 config to dma remap */
} scfg_tmr16_dma_remap2_type;

/**
  * @brief scfg tmr17 dma channel remap type
  */
typedef enum
{
  SCFG_TMR17_TO_DMA_CHANNEL_1            = 0x00, /* tmr17 config to dma channel 1 */
  SCFG_TMR17_TO_DMA_CHANNEL_2            = 0x01, /* tmr17 config to dma channel 2 */
} scfg_tmr17_dma_remap_type;

/**
  * @brief scfg tmr17 dma channel remap2 type
  */
typedef enum
{
  SCFG_TMR17_DMA_NO_REMAP                = 0x00, /* tmr17 config to dma no remap */
  SCFG_TMR17_DMA_REMAP                   = 0x01, /* tmr17 config to dma remap */
} scfg_tmr17_dma_remap2_type;

/**
  * @brief scfg infrared modulation signal source selecting type
  */
typedef enum
{
  SCFG_IR_SOURCE_TMR16                   = 0x00, /* infrared signal source select tmr16 */
  SCFG_IR_SOURCE_USART1                  = 0x01, /* infrared signal source select usart1 */
  SCFG_IR_SOURCE_USART2                  = 0x02  /* infrared signal source select usart2 */
} scfg_ir_source_type;

/**
  * @brief scfg infrared output polarity selecting type
  */
typedef enum
{
  SCFG_IR_POLARITY_NO_AFFECTE            = 0x00, /* infrared output polarity no affecte */
  SCFG_IR_POLARITY_REVERSE               = 0x01  /* infrared output polarity reverse */
} scfg_ir_polarity_type;

/**
  * @brief scfg spi2 dma remap type
  */
typedef enum
{
  SCFG_SPI2_DMA_NO_REMAP                 = 0x00, /* spi2 config to dma no remap */
  SCFG_SPI2_DMA_REMAP                    = 0x01, /* spi2 config to dma remap */
} scfg_spi2_dma_remap_type;

/**
  * @brief scfg i2c1 dma remap type
  */
typedef enum
{
  SCFG_I2C1_DMA_NO_REMAP                 = 0x00, /* i2c1 config to dma no remap */
  SCFG_I2C1_DMA_REMAP                    = 0x01, /* i2c1 config to dma remap */
} scfg_i2c1_dma_remap_type;

/**
  * @brief scfg memory address mapping selecting type
  */
typedef enum
{
  SCFG_MEM_MAP_MAIN_MEMORY               = 0x00, /* 0x00000000 address mapping from main memory */
  SCFG_MEM_MAP_BOOT_MEMORY               = 0x01, /* 0x00000000 address mapping from boot memory */
  SCFG_MEM_MAP_INTERNAL_SRAM             = 0x03, /* 0x00000000 address mapping from internal sram */
} scfg_mem_map_type;

/**
  * @brief scfg can1 tst sel type
  */
typedef enum
{
  SCFG_CAN_TIMESTAMP_TMR3                = 0x00, /* can1 timestamp counter source tmr3 */
  SCFG_CAN_TIMESTAMP_TMR4                = 0x01, /* can1 timestamp counter source tmr4 */
} scfg_can_timestamp_source_type;

/**
  * @brief scfg can index
  */
typedef enum
{
  SCFG_CAN1                              = 0x00  /* can1 */
} scfg_can_type;

/**
  * @brief scfg nrst pin remap ststus type
  */
typedef enum
{
  SCFG_NRST_RMP_NRST                     = 0x00,
  SCFG_NRST_RMP_PF2                      = 0x01
} scfg_nrst_rmp_sts_type;

/**
  * @brief scfg pin source type
  */
typedef enum
{
  SCFG_PINS_SOURCE0                      = 0x00,
  SCFG_PINS_SOURCE1                      = 0x01,
  SCFG_PINS_SOURCE2                      = 0x02,
  SCFG_PINS_SOURCE3                      = 0x03,
  SCFG_PINS_SOURCE4                      = 0x04,
  SCFG_PINS_SOURCE5                      = 0x05,
  SCFG_PINS_SOURCE6                      = 0x06,
  SCFG_PINS_SOURCE7                      = 0x07,
  SCFG_PINS_SOURCE8                      = 0x08,
  SCFG_PINS_SOURCE9                      = 0x09,
  SCFG_PINS_SOURCE10                     = 0x0A,
  SCFG_PINS_SOURCE11                     = 0x0B,
  SCFG_PINS_SOURCE12                     = 0x0C,
  SCFG_PINS_SOURCE13                     = 0x0D,
  SCFG_PINS_SOURCE14                     = 0x0E,
  SCFG_PINS_SOURCE15                     = 0x0F
} scfg_pins_source_type;

/**
  * @brief gpio port source type
  */
typedef enum
{
  SCFG_PORT_SOURCE_GPIOA                 = 0x00,
  SCFG_PORT_SOURCE_GPIOB                 = 0x01,
  SCFG_PORT_SOURCE_GPIOC                 = 0x02,
  SCFG_PORT_SOURCE_GPIOF                 = 0x05,

} scfg_port_source_type;

/**
  * @brief type define system config register all
  */
typedef struct
{
  /**
    * @brief scfg cfg1 register, offset:0x00
    */
  union
  {
    __IO uint32_t cfg1;
    struct
    {
      __IO uint32_t mem_map_sel          : 2; /* [1:0] */
      __IO uint32_t reserved1            : 2; /* [3:2] */
      __IO uint32_t pa11_12_rmp          : 1; /* [4] */
      __IO uint32_t ir_pol               : 1; /* [5] */
      __IO uint32_t ir_src_sel           : 2; /* [7:6] */
      __IO uint32_t adc_dma_rmp          : 1; /* [8] */
      __IO uint32_t usart1_tx_dma_rmp    : 1; /* [9] */
      __IO uint32_t usart1_rx_dma_rmp    : 1; /* [10] */
      __IO uint32_t tmr16_dma_rmp        : 1; /* [11] */
      __IO uint32_t tmr17_dma_rmp        : 1; /* [12] */
      __IO uint32_t tmr16_dma_rmp2       : 1; /* [13] */
      __IO uint32_t tmr17_dma_rmp2       : 1; /* [14] */
      __IO uint32_t reserved2            : 8; /* [23:15] */
      __IO uint32_t spi2_dma_rmp         : 1; /* [24] */
      __IO uint32_t usart2_dma_rmp       : 1; /* [25] */
      __IO uint32_t reserved3            : 1; /* [26] */
      __IO uint32_t i2c1_dma_rmp         : 1; /* [27] */
      __IO uint32_t tmr1_dma_rmp         : 1; /* [28] */
      __IO uint32_t reserved4            : 1; /* [29] */
      __IO uint32_t tmr3_dma_rmp         : 1; /* [30] */
      __IO uint32_t reserved5            : 1; /* [31] */
    } cfg1_bit;
  };

  /**
    * @brief scfg cfg2 register, offset:0x04
    */
  union
  {
    __IO uint32_t cfg2;
    struct
    {
      __IO uint32_t reserved1            : 24; /*[23:0] */
      __IO uint32_t can1_tst_sel         : 1; /* [24] */
      __IO uint32_t reserved2            : 7; /*[31:25] */
    } cfg2_bit;
  };

  /**
    * @brief scfg exintc1 register, offset:0x08
    */
  union
  {
    __IO uint32_t exintc1;
    struct
    {
      __IO uint32_t exint0               : 4; /* [3:0] */
      __IO uint32_t exint1               : 4; /* [7:4] */
      __IO uint32_t exint2               : 4; /* [11:8] */
      __IO uint32_t exint3               : 4; /* [15:12] */
      __IO uint32_t reserved1            : 16;/* [31:16] */
    } exintc1_bit;
  };

  /**
    * @brief scfg exintc2 register, offset:0x0C
    */
  union
  {
    __IO uint32_t exintc2;
    struct
    {
      __IO uint32_t exint4               : 4; /* [3:0] */
      __IO uint32_t exint5               : 4; /* [7:4] */
      __IO uint32_t exint6               : 4; /* [11:8] */
      __IO uint32_t exint7               : 4; /* [15:12] */
      __IO uint32_t reserved1            : 16;/* [31:16] */
    } exintc2_bit;
  };

  /**
    * @brief scfg exintc3 register, offset:0x10
    */
  union
  {
    __IO uint32_t exintc3;
    struct
    {
      __IO uint32_t exint8               : 4; /* [3:0] */
      __IO uint32_t exint9               : 4; /* [7:4] */
      __IO uint32_t exint10              : 4; /* [11:8] */
      __IO uint32_t exint11              : 4; /* [15:12] */
      __IO uint32_t reserved1            : 16;/* [31:16] */
    } exintc3_bit;
  };

  /**
    * @brief scfg exintc4 register, offset:0x14
    */
  union
  {
    __IO uint32_t exintc4;
    struct
    {
      __IO uint32_t exint12              : 4; /* [3:0] */
      __IO uint32_t exint13              : 4; /* [7:4] */
      __IO uint32_t exint14              : 4; /* [11:8] */
      __IO uint32_t exint15              : 4; /* [15:12] */
      __IO uint32_t reserved1            : 16;/* [31:16] */
    } exintc4_bit;
  };
  
  /**
    * @brief scfg iocfg register, offset:0x18
    */
  union
  {
    __IO uint32_t iocfg;
    struct
    {
      __IO uint32_t nrst_rmp             : 1; /* [0] */
      __IO uint32_t reserved1            : 31;/* [31:1] */
    } iocfg_bit;
  };

} scfg_type;

/**
  * @}
  */

#define SCFG                             ((scfg_type *) SCFG_CMP_BASE)

/** @defgroup SCFG_exported_functions
  * @{
  */

void scfg_reset(void);
void scfg_infrared_config(scfg_ir_source_type source, scfg_ir_polarity_type polarity);
uint8_t scfg_mem_map_get(void);
void scfg_pa11pa12_pin_remap(scfg_pa11pa12_remap_type pin_remap);
void scfg_adc_dma_channel_remap(scfg_adc_dma_remap_type dma_channel);
void scfg_usart1_tx_dma_channel_remap(scfg_usart1_tx_dma_remap_type dma_channel);
void scfg_usart1_rx_dma_channel_remap(scfg_usart1_rx_dma_remap_type dma_channel);
void scfg_tmr1_dma_channel_remap(scfg_tmr1_dma_remap_type dma_channel);
void scfg_tmr3_dma_channel_remap(scfg_tmr3_dma_remap_type dma_channel);
void scfg_tmr16_dma_channel_remap(scfg_tmr16_dma_remap_type dma_channel);
void scfg_tmr16_dma_channel_remap2(scfg_tmr16_dma_remap2_type dma_channel);
void scfg_tmr17_dma_channel_remap(scfg_tmr17_dma_remap_type dma_channel);
void scfg_tmr17_dma_channel_remap2(scfg_tmr17_dma_remap2_type dma_channel);
void scfg_spi2_dma_channel_remap(scfg_spi2_dma_remap_type dma_channel);
void scfg_usart2_dma_channel_remap(scfg_usart2_dma_remap_type dma_channel);
void scfg_i2c1_dma_channel_remap(scfg_i2c1_dma_remap_type dma_channel);
void scfg_can_timestamp_source_set(scfg_can_type can_index, scfg_can_timestamp_source_type source);
void scfg_exint_line_config(scfg_port_source_type port_source, scfg_pins_source_type pin_source);
uint8_t scfg_nrst_rmp_sts_get(void);
void scfg_dma_channel_remap(uint32_t dma_channel, confirm_state new_state);

/**
  * @}
  */

/**
  * @}
  */

/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif
