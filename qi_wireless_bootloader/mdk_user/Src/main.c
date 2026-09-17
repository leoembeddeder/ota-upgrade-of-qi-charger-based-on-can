/**
  **************************************************************************
  * @file     main.c
  * @brief    Bootloader main program for QI Charger OTA upgrade
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
#include "at32f422_426_clock.h"
#include "at32f422_426_conf.h"
#include "timer_drv.h"
#include "boot_metadata.h"
#include "boot_safe_mode.h"
#include "boot_trial.h"
#include "boot_jump.h"

/* exported functions --------------------------------------------------------*/

/**
 * @brief  bootloader main entry point
 * @note   boot sequence:
 *         1. system clock configuration
 *         2. initialize timer
 *         3. load OTA metadata
 *         4. detect boot reason
 *         5. process trial boot state machine
 *         6. select and verify boot slot
 *         7. jump to application or enter safe mode
 * @param  none
 * @retval none (should never return)
 */
int main(void)
{
  uint8_t boot_slot;
  uint8_t other_slot;
  int8_t  boot_result;

  /* step 1: configure system clock (180MHz from HEXT via PLL) */
  system_clock_config();
  nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);

  /* step 2: initialize drivers */
  timer_drv_init();

  /* step 3: load and validate OTA metadata */
  boot_metadata_init(&g_meta);

  /* step 4: detect and record boot reason */
  g_meta.last_boot_reason = detect_boot_reason();

  /* Download is APP-only. Boot only selects a slot and jumps.
   * ota_state=DOWNLOADING is ignored (legacy metadata). */

  /* step 5: process trial boot state machine */
  process_trial_state(&g_meta);

  /* step 6: PENDING/ACTIVE → trial_slot, else active_slot */
  if (select_boot_slot(&g_meta, &boot_slot) != 0)
  {
    enter_safe_mode();
  }

  /* trial 10s window is enforced in APP (ota_trial_poll). */

  /* step 7: verify then jump. Jump does not return, so save rollback
   * metadata before jumping to the fallback slot. */
  boot_result = try_boot_slot(boot_slot, &g_meta);
  if (boot_result == 0)
  {
    boot_jump_to_app(boot_metadata_slot_addr(boot_slot) + IMAGE_HEADER_SIZE);
  }

  other_slot = (boot_slot == SLOT_A) ? SLOT_B : SLOT_A;
  boot_result = try_boot_slot(other_slot, &g_meta);
  if (boot_result == 0)
  {
    if (g_meta.trial_state == TRIAL_STATE_ACTIVE)
    {
      g_meta.trial_state       = TRIAL_STATE_IDLE;
      g_meta.pending_slot      = SLOT_NONE;
      g_meta.trial_retry_count = 0U;
      g_meta.rollback_count++;
      g_meta.last_boot_reason  = BOOT_REASON_ROLLBACK;
    }
    g_meta.active_slot = other_slot;
    (void)boot_metadata_save(&g_meta);
    boot_jump_to_app(boot_metadata_slot_addr(other_slot) + IMAGE_HEADER_SIZE);
  }

  enter_safe_mode();
  while (1)
  {
    __NOP();
  }
}
