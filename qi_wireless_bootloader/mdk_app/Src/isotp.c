/**
  **************************************************************************
  * @file     isotp.c
  * @brief    ISO 15765-2 (ISO-TP) transport layer for CAN UDS communication
  **************************************************************************
  */

#include "isotp.h"
#include "can_driver.h"
#include "timer_drv.h"

static isotp_rx_ctx_t rx_ctx;
static isotp_complete_cb_t complete_callback = (isotp_complete_cb_t)0;

static volatile uint8_t g_fc_flag = 0;
static uint8_t g_fc_fs = 0;
static uint8_t g_fc_bs = 0;
static uint8_t g_fc_stmin = 0;

static uint8_t g_stash_valid = 0;
static uint8_t g_stash_len = 0;
static uint8_t g_stash[ISOTP_CAN_FRAME_SIZE];

static int8_t isotp_can_send(uint32_t can_id, uint8_t *frame, uint8_t len)
{
  uint32_t start = timer_get_tick();
  while ((timer_get_tick() - start) < ISOTP_N_AS_TIMEOUT_MS)
  {
    if (can_driver_send(can_id, frame, len) == 0)
    {
      /* PTB 优先于 STB。CF 若未等上一帧上总线就塞进 STB，SN 会乱序，
       * 主机 ISO-TP 组帧失败（ZCANPRO 表现为 22 F195 无应答）。 */
      (void)can_driver_wait_tx_idle(ISOTP_N_AS_TIMEOUT_MS);
      return 0;
    }
  }
  return -1;
}

static void isotp_send_fc(uint32_t can_id, uint8_t status, uint8_t bs, uint8_t stmin)
{
  uint8_t fc_data[ISOTP_CAN_FRAME_SIZE];
  uint8_t i;

  fc_data[0] = ISOTP_PCI_TYPE_FC | (status & 0x0FU);
  fc_data[1] = bs;
  fc_data[2] = stmin;
  for (i = 3U; i < ISOTP_CAN_FRAME_SIZE; i++)
  {
    fc_data[i] = 0xCCU;
  }
  (void)isotp_can_send(can_id, fc_data, ISOTP_CAN_FRAME_SIZE);
}

static uint8_t isotp_stmin_to_ms(uint8_t stmin)
{
  if (stmin <= 0x7FU)
  {
    return (stmin == 0U) ? 0U : stmin;
  }
  return 1U;
}

void isotp_init(isotp_complete_cb_t cb)
{
  rx_ctx.state        = ISOTP_RX_STATE_IDLE;
  rx_ctx.total_len    = 0;
  rx_ctx.received_len = 0;
  rx_ctx.expected_sn  = 0;
  rx_ctx.last_cf_ms   = 0;
  complete_callback   = cb;
  g_fc_flag           = 0;
  g_stash_valid       = 0;
}

void isotp_poll(void)
{
  if (rx_ctx.state != ISOTP_RX_STATE_RX_IN_PROGRESS)
  {
    return;
  }
  if ((timer_get_tick() - rx_ctx.last_cf_ms) >= ISOTP_N_CR_TIMEOUT_MS)
  {
    rx_ctx.state = ISOTP_RX_STATE_IDLE;
  }
}

void isotp_rx_process(uint8_t *data, uint8_t len)
{
  uint8_t pci_type;
  uint16_t payload_len;
  uint16_t copy_len;
  uint16_t i;

  if ((data == (uint8_t *)0) || (len == 0U))
  {
    return;
  }

  pci_type = data[0] & ISOTP_PCI_TYPE_MASK;

  switch (pci_type)
  {
    case ISOTP_PCI_TYPE_SF:
    {
      uint8_t sf_dl = data[0] & 0x0FU;
      if ((sf_dl == 0U) || (sf_dl > (uint8_t)(len - 1U)))
      {
        return;
      }
      rx_ctx.state = ISOTP_RX_STATE_IDLE;
      if (complete_callback != (isotp_complete_cb_t)0)
      {
        complete_callback(&data[1], (uint16_t)sf_dl);
      }
      break;
    }

    case ISOTP_PCI_TYPE_FF:
    {
      if (len < 2U)
      {
        return;
      }

      payload_len = ((uint16_t)(data[0] & 0x0FU) << 8) | (uint16_t)data[1];
      if (payload_len <= 7U)
      {
        return;
      }
      if (payload_len > ISOTP_MAX_PAYLOAD)
      {
        isotp_send_fc(CAN_ID_UDS_RESPONSE, ISOTP_FC_STATUS_OVERFLOW, 0U, 0U);
        return;
      }

      rx_ctx.total_len    = payload_len;
      rx_ctx.received_len = 0;
      rx_ctx.expected_sn  = 1;
      rx_ctx.state        = ISOTP_RX_STATE_RX_IN_PROGRESS;
      rx_ctx.last_cf_ms   = timer_get_tick();

      if (len > 2U)
      {
        copy_len = (uint16_t)(len - 2U);
        if (copy_len > payload_len)
        {
          copy_len = payload_len;
        }
        for (i = 0; i < copy_len; i++)
        {
          rx_ctx.buf[i] = data[2 + i];
        }
        rx_ctx.received_len = copy_len;
      }

      isotp_send_fc(CAN_ID_UDS_RESPONSE, ISOTP_FC_STATUS_CTS,
                    ISOTP_FC_DEFAULT_BS, ISOTP_FC_DEFAULT_STMIN);
      break;
    }

    case ISOTP_PCI_TYPE_CF:
    {
      if (rx_ctx.state != ISOTP_RX_STATE_RX_IN_PROGRESS)
      {
        return;
      }

      {
        uint8_t sn = data[0] & 0x0FU;
        if (sn != rx_ctx.expected_sn)
        {
          rx_ctx.state = ISOTP_RX_STATE_IDLE;
          return;
        }
      }

      rx_ctx.last_cf_ms = timer_get_tick();
      copy_len = (uint16_t)(len - 1U);
      if (copy_len > (rx_ctx.total_len - rx_ctx.received_len))
      {
        copy_len = rx_ctx.total_len - rx_ctx.received_len;
      }
      for (i = 0; i < copy_len; i++)
      {
        rx_ctx.buf[rx_ctx.received_len + i] = data[1 + i];
      }
      rx_ctx.received_len += copy_len;
      rx_ctx.expected_sn = (rx_ctx.expected_sn + 1U) & 0x0FU;

      if (rx_ctx.received_len >= rx_ctx.total_len)
      {
        rx_ctx.state = ISOTP_RX_STATE_IDLE;
        if (complete_callback != (isotp_complete_cb_t)0)
        {
          complete_callback(rx_ctx.buf, rx_ctx.total_len);
        }
      }
      break;
    }

    case ISOTP_PCI_TYPE_FC:
    {
      g_fc_fs = data[0] & 0x0FU;
      g_fc_bs = (len > 1U) ? data[1] : 0U;
      g_fc_stmin = (len > 2U) ? data[2] : 0U;
      g_fc_flag = 1;
      break;
    }

    default:
      break;
  }
}

static void isotp_delay_ms(uint32_t ms)
{
  uint32_t start = timer_get_tick();
  if (ms == 0U)
  {
    return;
  }
  while ((timer_get_tick() - start) < ms)
  {
  }
}

static void isotp_stash_frame(uint8_t *data, uint8_t n)
{
  uint8_t i;
  if (g_stash_valid != 0U)
  {
    return;
  }
  g_stash_len = n;
  if (g_stash_len > ISOTP_CAN_FRAME_SIZE)
  {
    g_stash_len = ISOTP_CAN_FRAME_SIZE;
  }
  for (i = 0; i < g_stash_len; i++)
  {
    g_stash[i] = data[i];
  }
  g_stash_valid = 1;
}

static int8_t isotp_wait_cts(uint8_t *out_bs, uint8_t *out_stmin)
{
  uint32_t start = timer_get_tick();
  uint32_t id;
  uint8_t data[ISOTP_CAN_FRAME_SIZE];
  uint8_t n;

  g_fc_flag = 0;

  while ((timer_get_tick() - start) < ISOTP_N_BS_TIMEOUT_MS)
  {
    if (g_fc_flag != 0U)
    {
      g_fc_flag = 0;
      if (g_fc_fs == ISOTP_FC_STATUS_CTS)
      {
        *out_bs = g_fc_bs;
        *out_stmin = g_fc_stmin;
        return 0;
      }
      if (g_fc_fs == ISOTP_FC_STATUS_OVERFLOW)
      {
        return -1;
      }
      start = timer_get_tick();
      continue;
    }

    if (can_driver_recv(&id, data, &n) != 0)
    {
      continue;
    }
    if ((n == 0U) || ((data[0] & ISOTP_PCI_TYPE_MASK) != ISOTP_PCI_TYPE_FC))
    {
      isotp_stash_frame(data, n);
      continue;
    }
    isotp_rx_process(data, n);
  }
  return -1;
}

int8_t isotp_tx_send(uint32_t can_id, uint8_t *payload, uint16_t len)
{
  uint8_t frame[ISOTP_CAN_FRAME_SIZE];
  uint16_t offset;
  uint8_t sn;
  uint8_t copy_len;
  uint8_t bs;
  uint8_t stmin;
  uint8_t cf_in_block;

  if ((payload == (uint8_t *)0) || (len == 0U) || (len > ISOTP_MAX_PAYLOAD))
  {
    return -1;
  }

  if (len <= 7U)
  {
    uint16_t i;
    frame[0] = ISOTP_PCI_TYPE_SF | (uint8_t)len;
    for (i = 0; i < len; i++)
    {
      frame[1 + i] = payload[i];
    }
    for (i = 1U + len; i < ISOTP_CAN_FRAME_SIZE; i++)
    {
      frame[i] = 0xCCU;
    }
    return isotp_can_send(can_id, frame, ISOTP_CAN_FRAME_SIZE);
  }

  frame[0] = ISOTP_PCI_TYPE_FF | (uint8_t)((len >> 8) & 0x0FU);
  frame[1] = (uint8_t)(len & 0xFFU);
  {
    uint8_t i;
    for (i = 0; i < 6U; i++)
    {
      frame[2 + i] = payload[i];
    }
  }
  if (isotp_can_send(can_id, frame, ISOTP_CAN_FRAME_SIZE) != 0)
  {
    return -1;
  }

  bs = 0;
  stmin = ISOTP_FC_DEFAULT_STMIN;
  if (isotp_wait_cts(&bs, &stmin) != 0)
  {
    return -1;
  }

  offset = 6U;
  sn = 1U;
  cf_in_block = 0;

  while (offset < len)
  {
    uint8_t i;

    if ((bs != 0U) && (cf_in_block >= bs))
    {
      if (isotp_wait_cts(&bs, &stmin) != 0)
      {
        return -1;
      }
      cf_in_block = 0;
    }

    {
      uint32_t gap = isotp_stmin_to_ms(stmin);
      if (gap < 1U)
      {
        gap = 1U;
      }
      isotp_delay_ms(gap);
    }

    frame[0] = ISOTP_PCI_TYPE_CF | (sn & 0x0FU);
    copy_len = (uint8_t)(len - offset);
    if (copy_len > 7U)
    {
      copy_len = 7U;
    }
    for (i = 0; i < copy_len; i++)
    {
      frame[1 + i] = payload[offset + i];
    }
    for (i = 1U + copy_len; i < ISOTP_CAN_FRAME_SIZE; i++)
    {
      frame[i] = 0xCCU;
    }
    if (isotp_can_send(can_id, frame, ISOTP_CAN_FRAME_SIZE) != 0)
    {
      return -1;
    }

    offset += copy_len;
    sn = (sn + 1U) & 0x0FU;
    cf_in_block++;
  }

  if (g_stash_valid != 0U)
  {
    g_stash_valid = 0;
    isotp_rx_process(g_stash, g_stash_len);
  }

  return 0;
}
