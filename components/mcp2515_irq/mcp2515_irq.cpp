// Interrupt-driven fork of ESPHome core mcp2515 (2026.9.1): reader task + RX queue.
#include "mcp2515_irq.h"
#include "esphome/core/log.h"

namespace esphome::mcp2515_irq {

static const char *const TAG = "mcp2515_irq";

// Above the main loop task, so a pending frame preempts rendering on the same core.
static constexpr UBaseType_t READER_PRIORITY = 18;
static constexpr uint32_t READER_STACK = 3072;
// SIDH SIDL EID8 EID0 DLC D0..D7, as streamed by READ RX BUFFER.
static constexpr size_t RXB_LEN = 13;
static constexpr uint8_t SIDL_SRR = 0x10;  // standard frame RTR (IDE = 0)
static constexpr uint8_t DLC_RTR = 0x40;   // extended frame RTR (IDE = 1)

const struct MCP2515::TxBnRegs MCP2515::TXB[N_TXBUFFERS] = {{MCP_TXB0CTRL, MCP_TXB0SIDH, MCP_TXB0DATA},
                                                            {MCP_TXB1CTRL, MCP_TXB1SIDH, MCP_TXB1DATA},
                                                            {MCP_TXB2CTRL, MCP_TXB2SIDH, MCP_TXB2DATA}};

const struct MCP2515::RxBnRegs MCP2515::RXB[N_RXBUFFERS] = {{MCP_RXB0CTRL, MCP_RXB0SIDH, MCP_RXB0DATA, CANINTF_RX0IF},
                                                            {MCP_RXB1CTRL, MCP_RXB1SIDH, MCP_RXB1DATA, CANINTF_RX1IF}};

bool MCP2515::setup_internal() {
  this->spi_setup();


  if (this->reset_() != canbus::ERROR_OK)
    return false;
  if (this->set_bitrate_(this->bit_rate_, this->mcp_clock_) != canbus::ERROR_OK)
    return false;

  // setup hardware filter RXF0 accepting all standard CAN IDs
  if (this->set_filter_(RXF::RXF0, false, 0) != canbus::ERROR_OK) {
    return false;
  }
  if (this->set_filter_mask_(MASK::MASK0, false, 0) != canbus::ERROR_OK) {
    return false;
  }

  // setup hardware filter RXF1 accepting all extended CAN IDs
  if (this->set_filter_(RXF::RXF1, true, 0) != canbus::ERROR_OK) {
    return false;
  }
  if (this->set_filter_mask_(MASK::MASK1, true, 0) != canbus::ERROR_OK) {
    return false;
  }

  if (this->set_mode_(this->mcp_mode_) != canbus::ERROR_OK)
    return false;
  uint8_t err_flags = this->get_error_flags_();
  ESP_LOGD(TAG, "mcp2515 setup done, error_flags = %02X", err_flags);


  if (this->interrupt_pin_ != nullptr) {
    this->rx_queue_ = xQueueCreate(this->rx_queue_len_, sizeof(canbus::CanFrame));
    this->spi_mutex_ = xSemaphoreCreateMutex();
    if (this->rx_queue_ == nullptr || this->spi_mutex_ == nullptr ||
        xTaskCreatePinnedToCore(reader_task_, "mcp2515_rx", READER_STACK, this, READER_PRIORITY,
                                &this->reader_task_handle_, 1) != pdPASS) {
      ESP_LOGE(TAG, "Failed to start interrupt reader");
      return false;
    }
    this->interrupt_pin_->setup();
    this->interrupt_pin_->attach_interrupt(MCP2515::gpio_isr_, this, gpio::INTERRUPT_FALLING_EDGE);
    // INT may already be low: frames that arrived before the edge interrupt was armed.
    xTaskNotifyGive(this->reader_task_handle_);
  }
  return true;
}

void IRAM_ATTR MCP2515::gpio_isr_(MCP2515 *self) {
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(self->reader_task_handle_, &woken);
  portYIELD_FROM_ISR(woken);
}

void MCP2515::reader_task_(void *arg) {
  auto *self = static_cast<MCP2515 *>(arg);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // INT is level-active: drain until it releases, so an edge that fell while
    // draining is never missed.
    do {
      self->drain_();
    } while (!self->interrupt_pin_->digital_read());
  }
}

void MCP2515::drain_() {
  xSemaphoreTake(this->spi_mutex_, portMAX_DELAY);
  for (;;) {
    uint8_t intf = this->read_register_(MCP_CANINTF);
    if ((intf & (CANINTF_RX0IF | CANINTF_RX1IF | CANINTF_ERRIF | CANINTF_MERRF)) == 0)
      break;
    // With rollover (BUKT), RXB1 only fills while RXB0 is full. A frame that
    // rolled into RXB1 while RXB0 was being read is older than the next RXB0
    // frame, so re-check RXB1 right after freeing RXB0 (as Linux mcp251x does).
    if (intf & CANINTF_RX0IF) {
      this->receive_(RXB0);
      if ((intf & CANINTF_RX1IF) == 0)
        intf |= this->read_register_(MCP_CANINTF) & (CANINTF_RX1IF | CANINTF_ERRIF | CANINTF_MERRF);
    }
    if (intf & CANINTF_RX1IF)
      this->receive_(RXB1);
    if (intf & CANINTF_ERRIF) {
      if (this->read_register_(MCP_EFLG) & (EFLG_RX0OVR | EFLG_RX1OVR)) {
        this->rx_overflows_++;
        this->modify_register_(MCP_EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0);
      }
      this->modify_register_(MCP_CANINTF, CANINTF_ERRIF, 0);
    }
    if (intf & CANINTF_MERRF)
      this->modify_register_(MCP_CANINTF, CANINTF_MERRF, 0);
  }
  xSemaphoreGive(this->spi_mutex_);
}

void MCP2515::receive_(RXBn rxbn) {
  canbus::CanFrame frame;
  if (!this->read_rx_buffer_(rxbn, &frame))
    return;
  this->frames_read_++;
  if (xQueueSend(this->rx_queue_, &frame, 0) != pdTRUE) {
    this->queue_drops_++;
    return;
  }
  const uint32_t waiting = uxQueueMessagesWaiting(this->rx_queue_);
  if (waiting > this->queue_high_water_)
    this->queue_high_water_ = waiting;
}

bool MCP2515::read_rx_buffer_(RXBn rxbn, canbus::CanFrame *frame) {
  uint8_t buf[RXB_LEN];
  this->enable();
  this->write_byte(rxbn == RXB0 ? INSTRUCTION_READ_RX0 : INSTRUCTION_READ_RX1);
  this->read_array(buf, RXB_LEN);
  this->disable();  // raising CS after READ RX BUFFER clears RXnIF

  const uint8_t dlc = buf[MCP_DLC] & DLC_MASK;
  if (dlc > canbus::CAN_MAX_DATA_LENGTH)
    return false;
  uint32_t id = (buf[MCP_SIDH] << 3) | (buf[MCP_SIDL] >> 5);
  const bool ext = buf[MCP_SIDL] & SIDL_EXIDE_MASK;
  if (ext)
    id = (id << 18) | ((buf[MCP_SIDL] & 0x03) << 16) | (buf[MCP_EID8] << 8) | buf[MCP_EID0];
  frame->can_id = id;
  frame->use_extended_id = ext;
  frame->remote_transmission_request = ext ? (buf[MCP_DLC] & DLC_RTR) : (buf[MCP_SIDL] & SIDL_SRR);
  frame->can_data_length_code = dlc;
  std::copy(buf + 5, buf + 5 + dlc, frame->data);
  return true;
}

canbus::Error MCP2515::reset_() {
  this->enable();
  this->transfer_byte(INSTRUCTION_RESET);
  this->disable();
  ESP_LOGV(TAG, "reset_()");
  delay(10);

  ESP_LOGV(TAG, "reset() CLEAR ALL TXB registers");

  uint8_t zeros[14];
  memset(zeros, 0, sizeof(zeros));
  set_registers_(MCP_TXB0CTRL, zeros, 14);
  set_registers_(MCP_TXB1CTRL, zeros, 14);
  set_registers_(MCP_TXB2CTRL, zeros, 14);
  ESP_LOGV(TAG, "reset() CLEARED TXB registers");

  set_register_(MCP_RXB0CTRL, 0);
  set_register_(MCP_RXB1CTRL, 0);

  set_register_(MCP_CANINTE, CANINTF_RX0IF | CANINTF_RX1IF | CANINTF_ERRIF | CANINTF_MERRF);

  modify_register_(MCP_RXB0CTRL, RXB_CTRL_RXM_MASK | RXB_0_CTRL_BUKT, RXB_CTRL_RXM_STDEXT | RXB_0_CTRL_BUKT);
  modify_register_(MCP_RXB1CTRL, RXB_CTRL_RXM_MASK, RXB_CTRL_RXM_STDEXT);

  return canbus::ERROR_OK;
}

uint8_t MCP2515::read_register_(const REGISTER reg) {
  this->enable();
  this->transfer_byte(INSTRUCTION_READ);
  this->transfer_byte(reg);
  uint8_t ret = this->transfer_byte(0x00);
  this->disable();

  return ret;
}

void MCP2515::read_registers_(const REGISTER reg, uint8_t values[], const uint8_t n) {
  this->enable();
  this->transfer_byte(INSTRUCTION_READ);
  this->transfer_byte(reg);
  // this->transfer_array(values, n);
  // mcp2515 has auto - increment of address - pointer
  for (uint8_t i = 0; i < n; i++) {
    values[i] = this->transfer_byte(0x00);
  }
  this->disable();
}

void MCP2515::set_register_(const REGISTER reg, const uint8_t value) {
  this->enable();
  this->transfer_byte(INSTRUCTION_WRITE);
  this->transfer_byte(reg);
  this->transfer_byte(value);
  this->disable();
}

void MCP2515::set_registers_(const REGISTER reg, uint8_t values[], const uint8_t n) {
  this->enable();
  this->transfer_byte(INSTRUCTION_WRITE);
  this->transfer_byte(reg);
  // this->transfer_array(values, n);
  for (uint8_t i = 0; i < n; i++) {
    this->transfer_byte(values[i]);
  }
  this->disable();
}

void MCP2515::modify_register_(const REGISTER reg, const uint8_t mask, const uint8_t data) {
  this->enable();
  this->transfer_byte(INSTRUCTION_BITMOD);
  this->transfer_byte(reg);
  this->transfer_byte(mask);
  this->transfer_byte(data);
  this->disable();
}

uint8_t MCP2515::get_status_() {
  this->enable();
  this->transfer_byte(INSTRUCTION_READ_STATUS);
  uint8_t i = this->transfer_byte(0x00);
  this->disable();

  return i;
}

canbus::Error MCP2515::set_mode_(const CanctrlReqopMode mode) {
  modify_register_(MCP_CANCTRL, CANCTRL_REQOP, mode);

  uint32_t start_time = millis();
  while (millis() - start_time < 10) {
    if ((read_register_(MCP_CANSTAT) & CANSTAT_OPMOD) == mode)
      return canbus::ERROR_OK;
  }
  ESP_LOGE(TAG, "Failed to set mode %02X: CANSTAT=%02X CANCTRL=%02X", mode, read_register_(MCP_CANSTAT),
           read_register_(MCP_CANCTRL));
  return canbus::ERROR_FAIL;
}

canbus::Error MCP2515::set_clk_out_(const CanClkOut divisor) {
  if (divisor == CLKOUT_DISABLE) {
    /* Turn off CLKEN */
    modify_register_(MCP_CANCTRL, CANCTRL_CLKEN, 0x00);

    /* Turn on CLKOUT for SOF */
    modify_register_(MCP_CNF3, CNF3_SOF, CNF3_SOF);
    return canbus::ERROR_OK;
  }

  /* Set the prescaler (CLKPRE) */
  modify_register_(MCP_CANCTRL, CANCTRL_CLKPRE, divisor);

  /* Turn on CLKEN */
  modify_register_(MCP_CANCTRL, CANCTRL_CLKEN, CANCTRL_CLKEN);

  /* Turn off CLKOUT for SOF */
  modify_register_(MCP_CNF3, CNF3_SOF, 0x00);
  return canbus::ERROR_OK;
}

void MCP2515::prepare_id_(uint8_t *buffer, const bool extended, const uint32_t id) {
  uint16_t canid = (uint16_t) (id & 0x0FFFF);

  if (extended) {
    buffer[MCP_EID0] = (uint8_t) (canid & 0xFF);
    buffer[MCP_EID8] = (uint8_t) (canid >> 8);
    canid = (uint16_t) (id >> 16);
    buffer[MCP_SIDL] = (uint8_t) (canid & 0x03);
    buffer[MCP_SIDL] += (uint8_t) ((canid & 0x1C) << 3);
    buffer[MCP_SIDL] |= SIDL_EXIDE_MASK;
    buffer[MCP_SIDH] = (uint8_t) (canid >> 5);
  } else {
    buffer[MCP_SIDH] = (uint8_t) (canid >> 3);
    buffer[MCP_SIDL] = (uint8_t) ((canid & 0x07) << 5);
    buffer[MCP_EID0] = 0;
    buffer[MCP_EID8] = 0;
  }
}

canbus::Error MCP2515::set_filter_mask_(const MASK mask, const bool extended, const uint32_t ul_data) {
  canbus::Error res = set_mode_(CANCTRL_REQOP_CONFIG);
  if (res != canbus::ERROR_OK) {
    return res;
  }

  uint8_t tbufdata[4];
  prepare_id_(tbufdata, extended, ul_data);

  REGISTER reg;
  switch (mask) {
    case MASK0:
      reg = MCP_RXM0SIDH;
      break;
    case MASK1:
      reg = MCP_RXM1SIDH;
      break;
    default:
      return canbus::ERROR_FAIL;
  }

  set_registers_(reg, tbufdata, 4);

  return canbus::ERROR_OK;
}

canbus::Error MCP2515::set_filter_(const RXF num, const bool extended, const uint32_t ul_data) {
  canbus::Error res = set_mode_(CANCTRL_REQOP_CONFIG);
  if (res != canbus::ERROR_OK) {
    return res;
  }

  REGISTER reg;

  switch (num) {
    case RXF0:
      reg = MCP_RXF0SIDH;
      break;
    case RXF1:
      reg = MCP_RXF1SIDH;
      break;
    case RXF2:
      reg = MCP_RXF2SIDH;
      break;
    case RXF3:
      reg = MCP_RXF3SIDH;
      break;
    case RXF4:
      reg = MCP_RXF4SIDH;
      break;
    case RXF5:
      reg = MCP_RXF5SIDH;
      break;
    default:
      return canbus::ERROR_FAIL;
  }

  uint8_t tbufdata[4];
  prepare_id_(tbufdata, extended, ul_data);
  set_registers_(reg, tbufdata, 4);

  return canbus::ERROR_OK;
}

canbus::Error MCP2515::send_message_(TXBn txbn, struct canbus::CanFrame *frame) {
  const struct TxBnRegs *txbuf = &TXB[txbn];

  uint8_t data[13];

  prepare_id_(data, frame->use_extended_id, frame->can_id);
  data[MCP_DLC] =
      frame->remote_transmission_request ? (frame->can_data_length_code | RTR_MASK) : frame->can_data_length_code;
  memcpy(&data[MCP_DATA], frame->data, frame->can_data_length_code);
  set_registers_(txbuf->SIDH, data, 5 + frame->can_data_length_code);
  modify_register_(txbuf->CTRL, TXB_TXREQ, TXB_TXREQ);

  return canbus::ERROR_OK;
}

canbus::Error MCP2515::send_message(struct canbus::CanFrame *frame) {
  if (frame->can_data_length_code > canbus::CAN_MAX_DATA_LENGTH) {
    return canbus::ERROR_FAILTX;
  }
  if (this->spi_mutex_ != nullptr)
    xSemaphoreTake(this->spi_mutex_, portMAX_DELAY);
  TXBn tx_buffers[N_TXBUFFERS] = {TXB0, TXB1, TXB2};
  canbus::Error rc = canbus::ERROR_ALLTXBUSY;

  for (auto &tx_buffer : tx_buffers) {
    const struct TxBnRegs *txbuf = &TXB[tx_buffer];
    uint8_t ctrlval = read_register_(txbuf->CTRL);
    if ((ctrlval & TXB_TXREQ) == 0) {
      rc = send_message_(tx_buffer, frame);
      break;
    }
  }

  if (this->spi_mutex_ != nullptr)
    xSemaphoreGive(this->spi_mutex_);
  return rc;
}

canbus::Error MCP2515::read_message_(RXBn rxbn, struct canbus::CanFrame *frame) {
  const struct RxBnRegs *rxb = &RXB[rxbn];

  uint8_t tbufdata[5];

  read_registers_(rxb->SIDH, tbufdata, 5);

  uint32_t id = (tbufdata[MCP_SIDH] << 3) + (tbufdata[MCP_SIDL] >> 5);
  bool use_extended_id = false;
  bool remote_transmission_request = false;

  if ((tbufdata[MCP_SIDL] & SIDL_EXIDE_MASK) == SIDL_EXIDE_MASK) {
    id = (id << 2) + (tbufdata[MCP_SIDL] & 0x03);
    id = (id << 8) + tbufdata[MCP_EID8];
    id = (id << 8) + tbufdata[MCP_EID0];
    // id |= canbus::CAN_EFF_FLAG;
    use_extended_id = true;
  }

  uint8_t dlc = (tbufdata[MCP_DLC] & DLC_MASK);
  if (dlc > canbus::CAN_MAX_DATA_LENGTH) {
    return canbus::ERROR_FAIL;
  }

  uint8_t ctrl = read_register_(rxb->CTRL);
  if (ctrl & RXB_CTRL_RTR) {
    // id |= canbus::CAN_RTR_FLAG;
    remote_transmission_request = true;
  }

  frame->can_id = id;
  frame->can_data_length_code = dlc;
  frame->use_extended_id = use_extended_id;
  frame->remote_transmission_request = remote_transmission_request;

  read_registers_(rxb->DATA, frame->data, dlc);

  modify_register_(MCP_CANINTF, rxb->CANINTF_RXnIF, 0);

  return canbus::ERROR_OK;
}

canbus::Error MCP2515::read_message(struct canbus::CanFrame *frame) {
  if (this->rx_queue_ != nullptr)
    return xQueueReceive(this->rx_queue_, frame, 0) == pdTRUE ? canbus::ERROR_OK : canbus::ERROR_NOMSG;
  canbus::Error rc;
  uint8_t stat = get_status_();

  if (stat & STAT_RX0IF) {
    rc = read_message_(RXB0, frame);
  } else if (stat & STAT_RX1IF) {
    rc = read_message_(RXB1, frame);
  } else {
    rc = canbus::ERROR_NOMSG;
  }

#ifdef ESPHOME_LOG_HAS_DEBUG
  uint8_t err = get_error_flags_();
  // The receive flowchart in the datasheet says that if rollover is set (BUKT), RX1OVR flag will be set
  // once both buffers are full. However, the RX0OVR flag is actually set instead.
  // We can just check for both though because it doesn't break anything.
  if (err & (EFLG_RX0OVR | EFLG_RX1OVR)) {
    ESP_LOGD(TAG, "receive buffer overrun");
    clear_rx_n_ovr_flags_();
  }
#endif

  return rc;
}

bool MCP2515::check_receive_() {
  uint8_t res = get_status_();
  return (res & STAT_RXIF_MASK) != 0;
}

bool MCP2515::check_error_() {
  uint8_t eflg = get_error_flags_();
  return (eflg & EFLG_ERRORMASK) != 0;
}

uint8_t MCP2515::get_error_flags_() { return read_register_(MCP_EFLG); }

void MCP2515::clear_rx_n_ovr_flags_() { modify_register_(MCP_EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0); }

uint8_t MCP2515::get_int_() { return read_register_(MCP_CANINTF); }

void MCP2515::clear_int_() { set_register_(MCP_CANINTF, 0); }

uint8_t MCP2515::get_int_mask_() { return read_register_(MCP_CANINTE); }

void MCP2515::clear_tx_int_() { modify_register_(MCP_CANINTF, (CANINTF_TX0IF | CANINTF_TX1IF | CANINTF_TX2IF), 0); }

void MCP2515::clear_rx_n_ovr_() {
  uint8_t eflg = get_error_flags_();
  if (eflg != 0) {
    clear_rx_n_ovr_flags_();
    clear_int_();
    // modify_register_(MCP_CANINTF, CANINTF_ERRIF, 0);
  }
}

void MCP2515::clear_merr_() {
  // modify_register_(MCP_EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0);
  // clear_int_();
  modify_register_(MCP_CANINTF, CANINTF_MERRF, 0);
}

void MCP2515::clear_errif_() {
  // modify_register_(MCP_EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0);
  // clear_int_();
  modify_register_(MCP_CANINTF, CANINTF_ERRIF, 0);
}

canbus::Error MCP2515::set_bitrate_(canbus::CanSpeed can_speed) { return this->set_bitrate_(can_speed, MCP_16MHZ); }

canbus::Error MCP2515::set_bitrate_(canbus::CanSpeed can_speed, CanClock can_clock) {
  canbus::Error error = set_mode_(CANCTRL_REQOP_CONFIG);
  if (error != canbus::ERROR_OK) {
    return error;
  }

  uint8_t set, cfg1, cfg2, cfg3;
  set = 1;
  switch (can_clock) {
    case (MCP_8MHZ):
      switch (can_speed) {
        case (canbus::CAN_5KBPS):  //   5KBPS
          cfg1 = MCP_8MHZ_5KBPS_CFG1;
          cfg2 = MCP_8MHZ_5KBPS_CFG2;
          cfg3 = MCP_8MHZ_5KBPS_CFG3;
          break;
        case (canbus::CAN_10KBPS):  //  10KBPS
          cfg1 = MCP_8MHZ_10KBPS_CFG1;
          cfg2 = MCP_8MHZ_10KBPS_CFG2;
          cfg3 = MCP_8MHZ_10KBPS_CFG3;
          break;
        case (canbus::CAN_20KBPS):  //  20KBPS
          cfg1 = MCP_8MHZ_20KBPS_CFG1;
          cfg2 = MCP_8MHZ_20KBPS_CFG2;
          cfg3 = MCP_8MHZ_20KBPS_CFG3;
          break;
        case (canbus::CAN_31K25BPS):  //  31.25KBPS
          cfg1 = MCP_8MHZ_31K25BPS_CFG1;
          cfg2 = MCP_8MHZ_31K25BPS_CFG2;
          cfg3 = MCP_8MHZ_31K25BPS_CFG3;
          break;
        case (canbus::CAN_33KBPS):  //  33.333KBPS
          cfg1 = MCP_8MHZ_33K3BPS_CFG1;
          cfg2 = MCP_8MHZ_33K3BPS_CFG2;
          cfg3 = MCP_8MHZ_33K3BPS_CFG3;
          break;
        case (canbus::CAN_40KBPS):  //  40Kbps
          cfg1 = MCP_8MHZ_40KBPS_CFG1;
          cfg2 = MCP_8MHZ_40KBPS_CFG2;
          cfg3 = MCP_8MHZ_40KBPS_CFG3;
          break;
        case (canbus::CAN_50KBPS):  //  50Kbps
          cfg1 = MCP_8MHZ_50KBPS_CFG1;
          cfg2 = MCP_8MHZ_50KBPS_CFG2;
          cfg3 = MCP_8MHZ_50KBPS_CFG3;
          break;
        case (canbus::CAN_80KBPS):  //  80Kbps
          cfg1 = MCP_8MHZ_80KBPS_CFG1;
          cfg2 = MCP_8MHZ_80KBPS_CFG2;
          cfg3 = MCP_8MHZ_80KBPS_CFG3;
          break;
        case (canbus::CAN_100KBPS):  // 100Kbps
          cfg1 = MCP_8MHZ_100KBPS_CFG1;
          cfg2 = MCP_8MHZ_100KBPS_CFG2;
          cfg3 = MCP_8MHZ_100KBPS_CFG3;
          break;
        case (canbus::CAN_125KBPS):  // 125Kbps
          cfg1 = MCP_8MHZ_125KBPS_CFG1;
          cfg2 = MCP_8MHZ_125KBPS_CFG2;
          cfg3 = MCP_8MHZ_125KBPS_CFG3;
          break;
        case (canbus::CAN_200KBPS):  // 200Kbps
          cfg1 = MCP_8MHZ_200KBPS_CFG1;
          cfg2 = MCP_8MHZ_200KBPS_CFG2;
          cfg3 = MCP_8MHZ_200KBPS_CFG3;
          break;
        case (canbus::CAN_250KBPS):  // 250Kbps
          cfg1 = MCP_8MHZ_250KBPS_CFG1;
          cfg2 = MCP_8MHZ_250KBPS_CFG2;
          cfg3 = MCP_8MHZ_250KBPS_CFG3;
          break;
        case (canbus::CAN_500KBPS):  // 500Kbps
          cfg1 = MCP_8MHZ_500KBPS_CFG1;
          cfg2 = MCP_8MHZ_500KBPS_CFG2;
          cfg3 = MCP_8MHZ_500KBPS_CFG3;
          break;
        case (canbus::CAN_1000KBPS):  //   1Mbps
          cfg1 = MCP_8MHZ_1000KBPS_CFG1;
          cfg2 = MCP_8MHZ_1000KBPS_CFG2;
          cfg3 = MCP_8MHZ_1000KBPS_CFG3;
          break;
        default:
          set = 0;
          break;
      }
      break;

    case (MCP_12MHZ):
      switch (can_speed) {
        case (canbus::CAN_5KBPS):  //   5Kbps
          cfg1 = MCP_12MHZ_5KBPS_CFG1;
          cfg2 = MCP_12MHZ_5KBPS_CFG2;
          cfg3 = MCP_12MHZ_5KBPS_CFG3;
          break;
        case (canbus::CAN_10KBPS):  //  10Kbps
          cfg1 = MCP_12MHZ_10KBPS_CFG1;
          cfg2 = MCP_12MHZ_10KBPS_CFG2;
          cfg3 = MCP_12MHZ_10KBPS_CFG3;
          break;
        case (canbus::CAN_20KBPS):  //  20Kbps
          cfg1 = MCP_12MHZ_20KBPS_CFG1;
          cfg2 = MCP_12MHZ_20KBPS_CFG2;
          cfg3 = MCP_12MHZ_20KBPS_CFG3;
          break;
        case (canbus::CAN_33KBPS):  //  33.333Kbps
          cfg1 = MCP_12MHZ_33K3BPS_CFG1;
          cfg2 = MCP_12MHZ_33K3BPS_CFG2;
          cfg3 = MCP_12MHZ_33K3BPS_CFG3;
          break;
        case (canbus::CAN_40KBPS):  //  40Kbps
          cfg1 = MCP_12MHZ_40KBPS_CFG1;
          cfg2 = MCP_12MHZ_40KBPS_CFG2;
          cfg3 = MCP_12MHZ_40KBPS_CFG3;
          break;
        case (canbus::CAN_50KBPS):  //  50Kbps
          cfg1 = MCP_12MHZ_50KBPS_CFG1;
          cfg2 = MCP_12MHZ_50KBPS_CFG2;
          cfg3 = MCP_12MHZ_50KBPS_CFG3;
          break;
        case (canbus::CAN_80KBPS):  //  80Kbps
          cfg1 = MCP_12MHZ_80KBPS_CFG1;
          cfg2 = MCP_12MHZ_80KBPS_CFG2;
          cfg3 = MCP_12MHZ_80KBPS_CFG3;
          break;
        case (canbus::CAN_100KBPS):  // 100Kbps
          cfg1 = MCP_12MHZ_100KBPS_CFG1;
          cfg2 = MCP_12MHZ_100KBPS_CFG2;
          cfg3 = MCP_12MHZ_100KBPS_CFG3;
          break;
        case (canbus::CAN_125KBPS):  // 125Kbps
          cfg1 = MCP_12MHZ_125KBPS_CFG1;
          cfg2 = MCP_12MHZ_125KBPS_CFG2;
          cfg3 = MCP_12MHZ_125KBPS_CFG3;
          break;
        case (canbus::CAN_200KBPS):  // 200Kbps
          cfg1 = MCP_12MHZ_200KBPS_CFG1;
          cfg2 = MCP_12MHZ_200KBPS_CFG2;
          cfg3 = MCP_12MHZ_200KBPS_CFG3;
          break;
        case (canbus::CAN_250KBPS):  // 250Kbps
          cfg1 = MCP_12MHZ_250KBPS_CFG1;
          cfg2 = MCP_12MHZ_250KBPS_CFG2;
          cfg3 = MCP_12MHZ_250KBPS_CFG3;
          break;
        case (canbus::CAN_500KBPS):  // 500Kbps
          cfg1 = MCP_12MHZ_500KBPS_CFG1;
          cfg2 = MCP_12MHZ_500KBPS_CFG2;
          cfg3 = MCP_12MHZ_500KBPS_CFG3;
          break;
        case (canbus::CAN_1000KBPS):  //   1Mbps
          cfg1 = MCP_12MHZ_1000KBPS_CFG1;
          cfg2 = MCP_12MHZ_1000KBPS_CFG2;
          cfg3 = MCP_12MHZ_1000KBPS_CFG3;
          break;
        default:
          set = 0;
          break;
      }
      break;

    case (MCP_16MHZ):
      switch (can_speed) {
        case (canbus::CAN_5KBPS):  //   5Kbps
          cfg1 = MCP_16MHZ_5KBPS_CFG1;
          cfg2 = MCP_16MHZ_5KBPS_CFG2;
          cfg3 = MCP_16MHZ_5KBPS_CFG3;
          break;
        case (canbus::CAN_10KBPS):  //  10Kbps
          cfg1 = MCP_16MHZ_10KBPS_CFG1;
          cfg2 = MCP_16MHZ_10KBPS_CFG2;
          cfg3 = MCP_16MHZ_10KBPS_CFG3;
          break;
        case (canbus::CAN_20KBPS):  //  20Kbps
          cfg1 = MCP_16MHZ_20KBPS_CFG1;
          cfg2 = MCP_16MHZ_20KBPS_CFG2;
          cfg3 = MCP_16MHZ_20KBPS_CFG3;
          break;
        case (canbus::CAN_33KBPS):  //  33.333Kbps
          cfg1 = MCP_16MHZ_33K3BPS_CFG1;
          cfg2 = MCP_16MHZ_33K3BPS_CFG2;
          cfg3 = MCP_16MHZ_33K3BPS_CFG3;
          break;
        case (canbus::CAN_40KBPS):  //  40Kbps
          cfg1 = MCP_16MHZ_40KBPS_CFG1;
          cfg2 = MCP_16MHZ_40KBPS_CFG2;
          cfg3 = MCP_16MHZ_40KBPS_CFG3;
          break;
        case (canbus::CAN_50KBPS):  //  50Kbps
          cfg1 = MCP_16MHZ_50KBPS_CFG1;
          cfg2 = MCP_16MHZ_50KBPS_CFG2;
          cfg3 = MCP_16MHZ_50KBPS_CFG3;
          break;
        case (canbus::CAN_80KBPS):  //  80Kbps
          cfg1 = MCP_16MHZ_80KBPS_CFG1;
          cfg2 = MCP_16MHZ_80KBPS_CFG2;
          cfg3 = MCP_16MHZ_80KBPS_CFG3;
          break;
        case (canbus::CAN_83K3BPS):  //  83.333Kbps
          cfg1 = MCP_16MHZ_83K3BPS_CFG1;
          cfg2 = MCP_16MHZ_83K3BPS_CFG2;
          cfg3 = MCP_16MHZ_83K3BPS_CFG3;
          break;
        case (canbus::CAN_100KBPS):  // 100Kbps
          cfg1 = MCP_16MHZ_100KBPS_CFG1;
          cfg2 = MCP_16MHZ_100KBPS_CFG2;
          cfg3 = MCP_16MHZ_100KBPS_CFG3;
          break;
        case (canbus::CAN_125KBPS):  // 125Kbps
          cfg1 = MCP_16MHZ_125KBPS_CFG1;
          cfg2 = MCP_16MHZ_125KBPS_CFG2;
          cfg3 = MCP_16MHZ_125KBPS_CFG3;
          break;
        case (canbus::CAN_200KBPS):  // 200Kbps
          cfg1 = MCP_16MHZ_200KBPS_CFG1;
          cfg2 = MCP_16MHZ_200KBPS_CFG2;
          cfg3 = MCP_16MHZ_200KBPS_CFG3;
          break;
        case (canbus::CAN_250KBPS):  // 250Kbps
          cfg1 = MCP_16MHZ_250KBPS_CFG1;
          cfg2 = MCP_16MHZ_250KBPS_CFG2;
          cfg3 = MCP_16MHZ_250KBPS_CFG3;
          break;
        case (canbus::CAN_500KBPS):  // 500Kbps
          cfg1 = MCP_16MHZ_500KBPS_CFG1;
          cfg2 = MCP_16MHZ_500KBPS_CFG2;
          cfg3 = MCP_16MHZ_500KBPS_CFG3;
          break;
        case (canbus::CAN_1000KBPS):  //   1Mbps
          cfg1 = MCP_16MHZ_1000KBPS_CFG1;
          cfg2 = MCP_16MHZ_1000KBPS_CFG2;
          cfg3 = MCP_16MHZ_1000KBPS_CFG3;
          break;
        default:
          set = 0;
          break;
      }
      break;

    case (MCP_20MHZ):
      switch (can_speed) {
        case (canbus::CAN_33KBPS):  //  33.333Kbps
          cfg1 = MCP_20MHZ_33K3BPS_CFG1;
          cfg2 = MCP_20MHZ_33K3BPS_CFG2;
          cfg3 = MCP_20MHZ_33K3BPS_CFG3;
          break;
        case (canbus::CAN_40KBPS):  //  40Kbps
          cfg1 = MCP_20MHZ_40KBPS_CFG1;
          cfg2 = MCP_20MHZ_40KBPS_CFG2;
          cfg3 = MCP_20MHZ_40KBPS_CFG3;
          break;
        case (canbus::CAN_50KBPS):  //  50Kbps
          cfg1 = MCP_20MHZ_50KBPS_CFG1;
          cfg2 = MCP_20MHZ_50KBPS_CFG2;
          cfg3 = MCP_20MHZ_50KBPS_CFG3;
          break;
        case (canbus::CAN_80KBPS):  //  80Kbps
          cfg1 = MCP_20MHZ_80KBPS_CFG1;
          cfg2 = MCP_20MHZ_80KBPS_CFG2;
          cfg3 = MCP_20MHZ_80KBPS_CFG3;
          break;
        case (canbus::CAN_83K3BPS):  //  83.333Kbps
          cfg1 = MCP_20MHZ_83K3BPS_CFG1;
          cfg2 = MCP_20MHZ_83K3BPS_CFG2;
          cfg3 = MCP_20MHZ_83K3BPS_CFG3;
          break;
        case (canbus::CAN_100KBPS):  // 100Kbps
          cfg1 = MCP_20MHZ_100KBPS_CFG1;
          cfg2 = MCP_20MHZ_100KBPS_CFG2;
          cfg3 = MCP_20MHZ_100KBPS_CFG3;
          break;
        case (canbus::CAN_125KBPS):  // 125Kbps
          cfg1 = MCP_20MHZ_125KBPS_CFG1;
          cfg2 = MCP_20MHZ_125KBPS_CFG2;
          cfg3 = MCP_20MHZ_125KBPS_CFG3;
          break;
        case (canbus::CAN_200KBPS):  // 200Kbps
          cfg1 = MCP_20MHZ_200KBPS_CFG1;
          cfg2 = MCP_20MHZ_200KBPS_CFG2;
          cfg3 = MCP_20MHZ_200KBPS_CFG3;
          break;
        case (canbus::CAN_250KBPS):  // 250Kbps
          cfg1 = MCP_20MHZ_250KBPS_CFG1;
          cfg2 = MCP_20MHZ_250KBPS_CFG2;
          cfg3 = MCP_20MHZ_250KBPS_CFG3;
          break;
        case (canbus::CAN_500KBPS):  // 500Kbps
          cfg1 = MCP_20MHZ_500KBPS_CFG1;
          cfg2 = MCP_20MHZ_500KBPS_CFG2;
          cfg3 = MCP_20MHZ_500KBPS_CFG3;
          break;
        case (canbus::CAN_1000KBPS):  //   1Mbps
          cfg1 = MCP_20MHZ_1000KBPS_CFG1;
          cfg2 = MCP_20MHZ_1000KBPS_CFG2;
          cfg3 = MCP_20MHZ_1000KBPS_CFG3;
          break;
        default:
          set = 0;
          break;
      }
      break;

    default:
      set = 0;
      break;
  }

  if (set) {
    set_register_(MCP_CNF1, cfg1);  // NOLINT
    set_register_(MCP_CNF2, cfg2);  // NOLINT
    set_register_(MCP_CNF3, cfg3);  // NOLINT
    return canbus::ERROR_OK;
  } else {
    ESP_LOGE(TAG, "Invalid frequency/bitrate combination: %d/%d", can_clock, can_speed);
    return canbus::ERROR_FAIL;
  }
}
}  // namespace esphome::mcp2515_irq
