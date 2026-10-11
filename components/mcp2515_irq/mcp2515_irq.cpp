// Interrupt-driven fork of ESPHome core mcp2515 (2026.9.1): reader task + RX queue.
#include "mcp2515_irq.h"
#include "esphome/core/log.h"

#include <esp_timer.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>

namespace esphome::mcp2515_irq {

static const char *const TAG = "mcp2515_irq";

// Above lwIP's tcpip task (18, unpinned) and esp_timer (22): at equal priority the
// reader only gets round-robin slices (1 ms tick) while tcpip pushes API traffic on
// this core, and the MCP2515's two RX buffers overflow in a frame burst. WiFi (23)
// is pinned to core 0.
static constexpr UBaseType_t READER_PRIORITY = 23;
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

void MCP2515::spi_setup() {
  if (this->dev_ != nullptr)
    return;
  // No ESPHome SPI delegate: an own IDF device on the same host. CS must be an
  // internal GPIO (driven by register access).
  this->own_cs_ = this->cs_;
  this->own_cs_->setup();
  this->own_cs_->digital_write(true);
  this->cs_gpio_ = static_cast<InternalGPIOPin *>(this->own_cs_)->get_pin();
  spi_device_interface_config_t cfg{};
  cfg.mode = 0;
  cfg.clock_speed_hz = static_cast<int>(this->data_rate_);
  cfg.spics_io_num = -1;
  cfg.queue_size = 1;
  if (spi_bus_add_device(this->parent_->get_interface(), &cfg, &this->dev_) != ESP_OK)
    ESP_LOGE(TAG, "Failed to add SPI device");
}

void IRAM_ATTR MCP2515::set_cs_(bool level) { gpio_ll_set_level(&GPIO, static_cast<gpio_num_t>(this->cs_gpio_), level); }

void IRAM_ATTR MCP2515::enable() {
  if (!this->bus_held_)
    spi_device_acquire_bus(this->dev_, portMAX_DELAY);
  this->set_cs_(false);
}

void IRAM_ATTR MCP2515::disable() {
  this->set_cs_(true);
  if (!this->bus_held_)
    spi_device_release_bus(this->dev_);
}

// Up to 4 bytes go through the transaction's own tx_data/rx_data (no DMA);
// longer ones use xfer_ (aligned, RX padded to a multiple of 4 by spi_read_).
void IRAM_ATTR MCP2515::transfer_(size_t len, bool read) {
  spi_transaction_t t{};
  t.length = len * 8;
  if (len <= 4) {
    t.flags = SPI_TRANS_USE_TXDATA | (read ? SPI_TRANS_USE_RXDATA : 0);
    memcpy(t.tx_data, this->xfer_, len);
  } else {
    t.tx_buffer = this->xfer_;
    t.rx_buffer = read ? this->xfer_ : nullptr;
  }
  spi_device_polling_transmit(this->dev_, &t);
  if (read && len <= 4)
    memcpy(this->xfer_, t.rx_data, len);
}

static constexpr size_t round4(size_t n) { return (n + 3) & ~static_cast<size_t>(3); }

void IRAM_ATTR MCP2515::spi_write_(size_t len) {
  this->enable();
  this->transfer_(len, false);
  this->disable();
}

void IRAM_ATTR MCP2515::spi_read_(size_t len) {
  // Padding bytes clock the auto-incremented address on: harmless for the
  // read instructions this is used with.
  const size_t padded = len <= 4 ? len : round4(len);
  for (size_t i = len; i < padded; i++)
    this->xfer_[i] = 0;
  this->enable();
  this->transfer_(padded, true);
  this->disable();
}

uint8_t IRAM_ATTR MCP2515::read_intf_() {
  this->xfer_[0] = INSTRUCTION_READ;
  this->xfer_[1] = MCP_CANINTF;
  this->spi_read_(3);
  return this->xfer_[2];
}

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

  if (this->start_delay_ms_ == 0) {
    if (this->set_mode_(this->mcp_mode_) != canbus::ERROR_OK)
      return false;
    this->started_ = true;
  }
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
    this->int_gpio_ = this->interrupt_pin_->get_pin();
    this->interrupt_pin_->attach_interrupt(MCP2515::gpio_isr_, this, gpio::INTERRUPT_FALLING_EDGE);
    // INT may already be low: frames that arrived before the edge interrupt was armed.
    xTaskNotifyGive(this->reader_task_handle_);
  }
  if (!this->started_)
    this->set_timeout(this->start_delay_ms_, [this]() { this->start_(); });
  return true;
}

void MCP2515::start_() {
#ifdef USE_MCP2515_IRQ_SAFE_MODE
  if (this->safe_mode_ != nullptr)
    this->safe_mode_->mark_successful();
#endif
  global_preferences->sync();
  if (this->spi_mutex_ != nullptr)
    xSemaphoreTake(this->spi_mutex_, portMAX_DELAY);
  const bool ok = this->set_mode_(this->mcp_mode_) == canbus::ERROR_OK;
  this->started_ = ok;
  if (this->spi_mutex_ != nullptr)
    xSemaphoreGive(this->spi_mutex_);
  ESP_LOGI(TAG, "reception started%s", ok ? "" : " FAILED");
}

bool MCP2515::set_listen_only(bool listen_only) {
  if (this->spi_mutex_ != nullptr)
    xSemaphoreTake(this->spi_mutex_, portMAX_DELAY);
  if (listen_only) {
    // An unacknowledged frame (nobody awake on the bus) would stay pending and
    // go out stale at the next switch to normal mode: abort it first.
    for (const auto &txb : TXB)
      this->modify_register_(txb.CTRL, TXB_TXREQ, 0);
  }
  const CanctrlReqopMode mode = listen_only ? CANCTRL_REQOP_LISTENONLY : CANCTRL_REQOP_NORMAL;
  bool ok = true;
  if (this->started_)
    ok = this->set_mode_(mode) == canbus::ERROR_OK;
  else
    this->mcp_mode_ = mode;  // applied by start_()
  if (this->spi_mutex_ != nullptr)
    xSemaphoreGive(this->spi_mutex_);
  ESP_LOGI(TAG, "%s mode%s", listen_only ? "listen-only" : "normal", ok ? "" : " FAILED");
  return ok;
}

void MCP2515::read_diag(uint8_t &intf, uint8_t &eflg, uint8_t &rec, bool &int_low) {
  if (this->spi_mutex_ != nullptr)
    xSemaphoreTake(this->spi_mutex_, portMAX_DELAY);
  intf = this->read_register_(MCP_CANINTF);
  eflg = this->read_register_(MCP_EFLG);
  rec = this->read_register_(MCP_REC);
  if (this->spi_mutex_ != nullptr)
    xSemaphoreGive(this->spi_mutex_);
  int_low = this->interrupt_pin_ != nullptr && !this->interrupt_pin_->digital_read();
}

void IRAM_ATTR MCP2515::trace_(TraceType type, uint8_t intf, int64_t us) {
  this->trace_buf_[this->trace_head_] = {static_cast<uint32_t>(us), type, intf};
  this->trace_head_ = (this->trace_head_ + 1) % TRACE_LEN;
}

void MCP2515::log_overflow_trace() {
  if (!this->frozen_pending_.exchange(false))
    return;
  static const char *const NAMES[] = {"isr", "wake", "rxb0", "rxb1", "end", "OVERFLOW"};
  const uint32_t t0 = this->frozen_[this->frozen_head_].us;
  ESP_LOGW(TAG, "reader timeline before overflow (us relative to oldest event):");
  for (size_t i = 0; i < TRACE_LEN; i++) {
    const TraceEvent &e = this->frozen_[(this->frozen_head_ + i) % TRACE_LEN];
    ESP_LOGW(TAG, "  %8u %-8s intf=%02X", (unsigned) (e.us - t0), NAMES[e.type], e.intf);
  }
}

void IRAM_ATTR MCP2515::gpio_isr_(MCP2515 *self) {
  self->isr_us_ = esp_timer_get_time();
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(self->reader_task_handle_, &woken);
  portYIELD_FROM_ISR(woken);
}

void IRAM_ATTR MCP2515::reader_task_(void *arg) {
  auto *self = static_cast<MCP2515 *>(arg);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const int64_t woke = esp_timer_get_time();
    const int64_t isr_us = self->isr_us_;
    if (isr_us != 0)
      self->trace_(TRACE_ISR, 0, isr_us);
    self->trace_(TRACE_WAKE, 0, woke);
    // isr_us_ == 0: the edge fell while draining and its frames are already read.
    if (isr_us != 0 && woke - isr_us > self->max_wake_us_)
      self->max_wake_us_ = woke - isr_us;
    // INT is level-active: drain until it releases, so an edge that fell while
    // draining is never missed.
    do {
      self->drain_();
    } while (!gpio_ll_get_level(&GPIO, static_cast<gpio_num_t>(self->int_gpio_)));
    self->isr_us_ = 0;
    self->trace_(TRACE_DRAIN_END, 0, esp_timer_get_time());
    const uint32_t drain_us = esp_timer_get_time() - woke;
    if (drain_us > self->max_drain_us_)
      self->max_drain_us_ = drain_us;
  }
}

void IRAM_ATTR MCP2515::drain_() {
  xSemaphoreTake(this->spi_mutex_, portMAX_DELAY);
  // One bus acquisition for the whole drain: per-instruction acquisition would
  // let a display transfer in before every read and the burst outruns the reader.
  spi_device_acquire_bus(this->dev_, portMAX_DELAY);
  this->bus_held_ = true;
  for (;;) {
    const int64_t t0 = esp_timer_get_time();
    uint8_t intf = this->read_intf_();
    if ((intf & (CANINTF_RX0IF | CANINTF_RX1IF | CANINTF_ERRIF | CANINTF_MERRF)) == 0)
      break;
    // With rollover (BUKT), RXB1 only fills while RXB0 is full. A frame that
    // rolled into RXB1 while RXB0 was being read is older than the next RXB0
    // frame, so re-check RXB1 right after freeing RXB0 (as Linux mcp251x does).
    if (intf & CANINTF_RX0IF) {
      this->receive_(RXB0);
      this->trace_(TRACE_RXB0, intf, esp_timer_get_time());
      const uint32_t frame_us = esp_timer_get_time() - t0;
      if (frame_us > this->max_frame_us_)
        this->max_frame_us_ = frame_us;
      if ((intf & CANINTF_RX1IF) == 0)
        intf |= this->read_intf_() & (CANINTF_RX1IF | CANINTF_ERRIF | CANINTF_MERRF);
    }
    if (intf & CANINTF_RX1IF) {
      this->receive_(RXB1);
      this->trace_(TRACE_RXB1, intf, esp_timer_get_time());
    }
    if (intf & CANINTF_ERRIF) {
      if (this->read_register_(MCP_EFLG) & (EFLG_RX0OVR | EFLG_RX1OVR)) {
        this->rx_overflows_++;
        this->trace_(TRACE_OVERFLOW, intf, esp_timer_get_time());
        if (!this->frozen_pending_) {
          std::copy(std::begin(this->trace_buf_), std::end(this->trace_buf_), std::begin(this->frozen_));
          this->frozen_head_ = this->trace_head_;
          this->frozen_pending_ = true;
        }
        ESP_LOGW(TAG, "RX overflow #%u (frames read %u)", (unsigned) this->rx_overflows_,
                 (unsigned) this->frames_read_);
        this->modify_register_(MCP_EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0);
      }
      this->modify_register_(MCP_CANINTF, CANINTF_ERRIF, 0);
    }
    if (intf & CANINTF_MERRF)
      this->modify_register_(MCP_CANINTF, CANINTF_MERRF, 0);
  }
  this->bus_held_ = false;
  spi_device_release_bus(this->dev_);
  xSemaphoreGive(this->spi_mutex_);
}

void IRAM_ATTR MCP2515::receive_(RXBn rxbn) {
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

bool IRAM_ATTR MCP2515::read_rx_buffer_(RXBn rxbn, canbus::CanFrame *frame) {
  this->xfer_[0] = rxbn == RXB0 ? INSTRUCTION_READ_RX0 : INSTRUCTION_READ_RX1;
  this->spi_read_(1 + RXB_LEN);  // raising CS after READ RX BUFFER clears RXnIF
  const uint8_t *buf = this->xfer_ + 1;

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
  this->xfer_[0] = INSTRUCTION_RESET;
  this->spi_write_(1);
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
  this->xfer_[0] = INSTRUCTION_READ;
  this->xfer_[1] = reg;
  this->spi_read_(3);
  return this->xfer_[2];
}

void MCP2515::read_registers_(const REGISTER reg, uint8_t values[], const uint8_t n) {
  // MCP2515 auto-increments the address pointer.
  this->xfer_[0] = INSTRUCTION_READ;
  this->xfer_[1] = reg;
  this->spi_read_(2 + n);
  std::copy(this->xfer_ + 2, this->xfer_ + 2 + n, values);
}

void MCP2515::set_register_(const REGISTER reg, const uint8_t value) {
  this->xfer_[0] = INSTRUCTION_WRITE;
  this->xfer_[1] = reg;
  this->xfer_[2] = value;
  this->spi_write_(3);
}

void MCP2515::set_registers_(const REGISTER reg, uint8_t values[], const uint8_t n) {
  this->xfer_[0] = INSTRUCTION_WRITE;
  this->xfer_[1] = reg;
  std::copy(values, values + n, this->xfer_ + 2);
  this->spi_write_(2 + n);
}

void MCP2515::modify_register_(const REGISTER reg, const uint8_t mask, const uint8_t data) {
  this->xfer_[0] = INSTRUCTION_BITMOD;
  this->xfer_[1] = reg;
  this->xfer_[2] = mask;
  this->xfer_[3] = data;
  this->spi_write_(4);
}

uint8_t MCP2515::get_status_() {
  this->xfer_[0] = INSTRUCTION_READ_STATUS;
  this->spi_read_(2);
  return this->xfer_[1];
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
  static const uint8_t LOAD[N_TXBUFFERS] = {INSTRUCTION_LOAD_TX0, INSTRUCTION_LOAD_TX1, INSTRUCTION_LOAD_TX2};
  static const uint8_t RTS[N_TXBUFFERS] = {INSTRUCTION_RTS_TX0, INSTRUCTION_RTS_TX1, INSTRUCTION_RTS_TX2};
  // LOAD TX BUFFER writes from TXBnSIDH on; RTS requests the transmission.
  uint8_t *data = this->xfer_ + 1;
  this->xfer_[0] = LOAD[txbn];
  prepare_id_(data, frame->use_extended_id, frame->can_id);
  data[MCP_DLC] =
      frame->remote_transmission_request ? (frame->can_data_length_code | RTR_MASK) : frame->can_data_length_code;
  memcpy(&data[MCP_DATA], frame->data, frame->can_data_length_code);
  this->spi_write_(1 + 5 + frame->can_data_length_code);
  this->xfer_[0] = RTS[txbn];
  this->spi_write_(1);

  return canbus::ERROR_OK;
}

canbus::Error MCP2515::send_message(struct canbus::CanFrame *frame) {
  if (frame->can_data_length_code > canbus::CAN_MAX_DATA_LENGTH) {
    return canbus::ERROR_FAILTX;
  }
  if (!this->started_)
    return canbus::ERROR_FAIL;
  if (this->spi_mutex_ != nullptr)
    xSemaphoreTake(this->spi_mutex_, portMAX_DELAY);
  TXBn tx_buffers[N_TXBUFFERS] = {TXB0, TXB1, TXB2};
  canbus::Error rc = canbus::ERROR_ALLTXBUSY;

  uint8_t failed = 0;
  for (auto &tx_buffer : tx_buffers) {
    const struct TxBnRegs *txbuf = &TXB[tx_buffer];
    uint8_t ctrlval = read_register_(txbuf->CTRL);
    if ((ctrlval & TXB_TXREQ) == 0) {
      rc = send_message_(tx_buffer, frame);
      break;
    }
    if (ctrlval & TXB_TXERR)
      failed |= 1 << tx_buffer;
  }
  // All buffers retrying without an ack (bus disconnected, other nodes unpowered):
  // drop those frames, else they go out stale whenever the bus comes back.
  if (rc == canbus::ERROR_ALLTXBUSY && failed != 0) {
    for (auto &tx_buffer : tx_buffers)
      if (failed & (1 << tx_buffer))
        modify_register_(TXB[tx_buffer].CTRL, TXB_TXREQ, 0);
    ESP_LOGW(TAG, "TX aborted (no ack), buffers %02X", failed);
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
