/**
  **************************************************************************
  * @file     ota_download.h
  * @brief    APP-side UDS download (0x31/0x34/0x36/0x37) into the inactive slot
  **************************************************************************
  */
#ifndef __OTA_DOWNLOAD_H
#define __OTA_DOWNLOAD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OTA_DL_MAX_BLOCK_LEN  256U

void     ota_dl_abort(void);
uint8_t  ota_dl_target_slot(void);
uint8_t  ota_dl_erased(void);
uint8_t  ota_dl_active(void);
uint8_t  ota_dl_trial_ready(void);

void     ota_dl_handle_erase(uint8_t *data, uint16_t len);
void     ota_dl_handle_request_download(uint8_t *data, uint16_t len);
void     ota_dl_handle_transfer_data(uint8_t *data, uint16_t len);
void     ota_dl_handle_transfer_exit(uint8_t *data, uint16_t len);
void     ota_dl_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_DOWNLOAD_H */
