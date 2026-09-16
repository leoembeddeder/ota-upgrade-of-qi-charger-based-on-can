/**
  **************************************************************************
  * @file     boot_safe_mode.c
  * @brief    No UDS download in bootloader. Hang if no bootable slot.
  **************************************************************************
  */

#include "boot_safe_mode.h"
#include "at32f422_426.h"

void enter_safe_mode(void)
{
  /* Field OTA lives in APP. Empty chip / both slots invalid: stay here.
   * Factory image: merge_prod_bin.py from 0x08000000. */
  while (1)
  {
    __NOP();
  }
}
