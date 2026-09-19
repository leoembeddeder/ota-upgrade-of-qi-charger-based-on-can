/**
  **************************************************************************
  * @file     boot_trial.c
  * @brief    Trial boot state machine implementation
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
#include "boot_trial.h"
#include "boot_verify.h"
#include "boot_jump.h"
#include "boot_safe_mode.h"   /* Boot 诊断标记 M2/M3（观察不干预） */
#include "at32f422_426_conf.h"

/* private variables ---------------------------------------------------------*/

/** @brief  OTA metadata instance */
ota_metadata_t g_meta;

/* exported functions --------------------------------------------------------*/

/**
 * @brief  detect reset source and return boot reason code
 * @param  none
 * @retval boot reason code
 */
uint8_t detect_boot_reason(void)
{
  uint8_t reason = BOOT_REASON_POWER_ON;
  flag_status wdt  = crm_flag_get(CRM_WDT_RESET_FLAG);
  flag_status wwdt = crm_flag_get(CRM_WWDT_RESET_FLAG);
  flag_status sw   = crm_flag_get(CRM_SW_RESET_FLAG);
  flag_status por  = crm_flag_get(CRM_POR_RESET_FLAG);

  /* snapshot first: crm_flag_clear(RSTFC) clears every reset flag */
  if ((wdt != RESET) || (wwdt != RESET))
  {
    reason = BOOT_REASON_WDG;
  }
  else if (sw != RESET)
  {
    reason = BOOT_REASON_SW;
  }
  else if (por != RESET)
  {
    reason = BOOT_REASON_POWER_ON;
  }

  crm_flag_clear(CRM_ALL_RESET_FLAG);
  return reason;
}

/**
 * @brief  select the slot to boot from based on metadata
 * @param  meta: pointer to metadata
 * @param  slot: output, selected slot index (0=A, 1=B)
 * @retval 0 on success (slot A or B), -1 if the chosen index is invalid
 */
int8_t select_boot_slot(const ota_metadata_t *meta, uint8_t *slot)
{
  uint8_t s;

  /* PENDING is turned into ACTIVE in process_trial_state() before this
   * runs. Both must still boot trial_slot; active_slot stays the last
   * confirmed image until APP confirms. */
  if ((meta->trial_state == TRIAL_STATE_PENDING) ||
      (meta->trial_state == TRIAL_STATE_ACTIVE))
  {
    s = meta->trial_slot;
  }
  else
  {
    s = meta->active_slot;
  }

  if ((s != SLOT_A) && (s != SLOT_B))
  {
    boot_diag_m2(-1, s);   /* 诊断 M2：失败路径，slot=metadata 实际槽字节 */
    return -1;
  }
  *slot = s;
  boot_diag_m2(0, s);      /* 诊断 M2：选中槽（观察不干预） */
  return 0;
}

/**
 * @brief  perform trial boot state machine processing
 * @param  meta: pointer to metadata (mutable)
 * @retval none
 */
void process_trial_state(ota_metadata_t *meta)
{
  uint8_t other_slot;

  switch (meta->trial_state)
  {
    case TRIAL_STATE_IDLE:
      /* no trial in progress, nothing to do */
      break;

    case TRIAL_STATE_PENDING:
      /* trial requested: activate it */
      meta->trial_state       = TRIAL_STATE_ACTIVE;
      meta->trial_retry_count++;
      meta->last_boot_reason  = BOOT_REASON_OTA_ACT;
      boot_metadata_save(meta);
      break;

    case TRIAL_STATE_ACTIVE:
      /* APP did not confirm (timeout NVIC_SystemReset, crash, or WDG). */
      meta->trial_retry_count++;
      if (meta->trial_retry_count > meta->trial_max_retries)
      {
        meta->rollback_count++;
        meta->trial_state       = TRIAL_STATE_IDLE;
        meta->pending_slot      = SLOT_NONE;
        meta->trial_retry_count = 0;
        meta->last_boot_reason  = BOOT_REASON_ROLLBACK;

        other_slot = (meta->trial_slot == SLOT_A) ? SLOT_B : SLOT_A;
        if ((other_slot == SLOT_A && meta->slot_a_valid) ||
            (other_slot == SLOT_B && meta->slot_b_valid))
        {
          meta->active_slot = other_slot;
        }

        boot_metadata_save(meta);
      }
      else
      {
        boot_metadata_save(meta);
      }
      break;

    case TRIAL_STATE_CONFIRMED:
      /* APP already set active_slot to the trial slot */
      meta->trial_state       = TRIAL_STATE_IDLE;
      meta->pending_slot      = SLOT_NONE;
      meta->trial_retry_count = 0;
      boot_metadata_save(meta);
      break;

    default:
      /* invalid state, reset to idle */
      meta->trial_state = TRIAL_STATE_IDLE;
      boot_metadata_save(meta);
      break;
  }
}

/**
 * @brief  attempt to boot from a given slot
 * @param  slot: slot index (0=A, 1=B)
 * @param  meta: pointer to metadata
 * @retval 0 if the image verified (caller jumps), -1 on failure
 */
int8_t try_boot_slot(uint8_t slot, ota_metadata_t *meta)
{
  uint32_t slot_addr;
  uint32_t slot_size;
  uint8_t  *valid_flag;

  slot_addr = boot_metadata_slot_addr(slot);
  slot_size = boot_metadata_slot_size(slot);

  if ((slot_addr == 0) || (slot_size == 0))
  {
    return -1;
  }

  if (slot == SLOT_A)
  {
    valid_flag = &meta->slot_a_valid;
  }
  else
  {
    valid_flag = &meta->slot_b_valid;
  }

  if ((boot_verify_image(slot_addr, slot_size) != 0) ||
      (boot_jump_vectors_ok(slot_addr + IMAGE_HEADER_SIZE) != 0))
  {
    if (*valid_flag != 0U)
    {
      *valid_flag = 0U;
      (void)boot_metadata_save(meta);
    }
    boot_diag_m3(0U, g_verify_fail_step, slot); /* 诊断 M3：验签/向量失败 */
    return -1;
  }

  if (*valid_flag == 0U)
  {
    *valid_flag = 1U;
    (void)boot_metadata_save(meta);
  }

  boot_diag_m3(1U, g_verify_fail_step, slot);   /* 诊断 M3：过验（观察不干预） */
  /* caller jumps after it has saved any trial/rollback metadata */
  return 0;
}
