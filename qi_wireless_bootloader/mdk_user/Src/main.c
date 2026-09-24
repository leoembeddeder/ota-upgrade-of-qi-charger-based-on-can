/**
 **************************************************************************
 * @file     main.c
 * @brief    Bootloader main: backup->App copy + jump (OTA-ARCH-0920)
 **************************************************************************
 *
 * Boot sequence (single-App architecture, A/B slots removed):
 *   1. system clock + timer + diag CAN init
 *   2. load OTA metadata (dual-copy, CRC checked)
 *   3. if backup_valid flag set -> copy Backup region to App region
 *      (idempotent: flag cleared only after re-verify passes)
 *   4. verify App region image; on success jump to 0x08004100
 *   5. otherwise enter safe mode (CAN diagnostic frames)
 */

#include "at32f422_426_clock.h"
#include "at32f422_426_conf.h"
#include "timer_drv.h"
#include "boot_metadata.h"
#include "boot_safe_mode.h"
#include "boot_trial.h"
#include "boot_jump.h"

int main(void)
{
  int8_t copy_rc;

  /* step 1: clocks + drivers */
  system_clock_config();
  nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);
  timer_drv_init();
  boot_diag_can_init();

  /* step 2: metadata (primary -> backup -> defaults) + boot reason */
  boot_metadata_init(&g_meta);
  g_meta.last_boot_reason = detect_boot_reason();

  /* step 3: pending backup -> copy into App region */
  if (boot_backup_pending(&g_meta))
  {
    g_meta.last_boot_reason = BOOT_REASON_OTA_ACT;
    boot_diag_m2(0U, 0U); /* copy sequence starting */
    copy_rc = boot_copy_backup(&g_meta);
    if (copy_rc == 0)
    {
      boot_diag_m2(1U, 0U); /* copy committed */
    }
    else
    {
      /* flag left set -> retry next boot; try current App image anyway */
      boot_diag_m2(0xFFU, g_meta.copy_fail_step);
    }
  }
  else
  {
    boot_diag_m2(0U, 0xFFU); /* no pending copy */
  }

  /* step 4: verify App region and jump */
  if (boot_app_image_ok() == 0)
  {
    /* Persist last_boot_reason before jumping: its consumer reads it at
     * next power-on from metadata. The success path never saved it before
     * (only copy-fail / safe-mode / metadata-repair paths did), so the
     * stored value stayed stale forever. Save once here, after App verify
     * passes and before M4 / jump. */
    (void)boot_metadata_save(&g_meta);
    boot_jump_to_app(APP_ENTRY_ADDR); /* does not return */
  }

  /* step 5: no bootable App image */
  enter_safe_mode(g_meta.copy_fail_step != 0U ?
                  0x03U : 0x02U);
  while (1)
  {
    __NOP();
  }
}
