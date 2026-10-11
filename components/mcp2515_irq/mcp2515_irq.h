#pragma once

#include "esphome/components/canbus/canbus.h"
#include "esphome/components/spi/spi.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "mcp2515_irq_defs.h"
#ifdef USE_MCP2515_IRQ_SAFE_MODE
#include "esphome/components/safe_mode/safe_mode.h"
#endif

#include <algorithm>
#include <atomic>

#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace esphome::mcp2515_irq {
static const uint32_t SPI_CLOCK = 10000000;  // 10MHz

static const int N_TXBUFFERS = 3;
static const int N_RXBUFFERS = 2;
enum CanClock { MCP_20MHZ, MCP_16MHZ, MCP_12MHZ, MCP_8MHZ };
enum MASK { MASK0, MASK1 };
enum RXF { RXF0 = 0, RXF1 = 1, RXF2 = 2, RXF3 = 3, RXF4 = 4, RXF5 = 5 };
enum RXBn { RXB0 = 0, RXB1 = 1 };
enum TXBn { TXB0 = 0, TXB1 = 1, TXB2 = 2 };

enum CanClkOut {
  CLKOUT_DISABLE = -1,
  CLKOUT_DIV1 = 0x0,
  CLKOUT_DIV2 = 0x1,
  CLKOUT_DIV4 = 0x2,
  CLKOUT_DIV8 = 0x3,
};

enum CANINTF : uint8_t {
  CANINTF_RX0IF = 0x01,
  CANINTF_RX1IF = 0x02,
  CANINTF_TX0IF = 0x04,
  CANINTF_TX1IF = 0x08,
  CANINTF_TX2IF = 0x10,
  CANINTF_ERRIF = 0x20,
  CANINTF_WAKIF = 0x40,
  CANINTF_MERRF = 0x80
};

enum EFLG : uint8_t {
  EFLG_RX1OVR = (1 << 7),
  EFLG_RX0OVR = (1 << 6),
  EFLG_TXBO = (1 << 5),
  EFLG_TXEP = (1 << 4),
  EFLG_RXEP = (1 << 3),
  EFLG_TXWAR = (1 << 2),
  EFLG_RXWAR = (1 << 1),
  EFLG_EWARN = (1 << 0)
};

enum STAT : uint8_t { STAT_RX0IF = (1 << 0), STAT_RX1IF = (1 << 1) };

static const uint8_t STAT_RXIF_MASK = STAT_RX0IF | STAT_RX1IF;
static const uint8_t EFLG_ERRORMASK = EFLG_RX1OVR | EFLG_RX0OVR | EFLG_TXBO | EFLG_TXEP | EFLG_RXEP;

using SPIBase =
    spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW, spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_8MHZ>;

class MCP2515 final : public canbus::Canbus, public SPIBase {
 public:
  void spi_setup() override;
  MCP2515(){};
  void set_mcp_clock(CanClock clock) { this->mcp_clock_ = clock; };
  void set_mcp_mode(const CanctrlReqopMode mode) { this->mcp_mode_ = mode; }
  void set_interrupt_pin(InternalGPIOPin *pin) { this->interrupt_pin_ = pin; }
  void set_rx_queue_len(uint16_t len) { this->rx_queue_len_ = len; }
  /// Keeps the controller in config mode (no reception) for this long after setup.
  void set_start_delay(uint32_t ms) { this->start_delay_ms_ = ms; }
#ifdef USE_MCP2515_IRQ_SAFE_MODE
  /// At the start: marks the boot successful and syncs preferences first, so these flash
  /// writes (cache off, interrupts masked: the reader cannot run) fall before reception.
  void set_safe_mode(safe_mode::SafeModeComponent *sm) { this->safe_mode_ = sm; }
#endif

  /// Overflow events in the MCP2515 (both RX buffers full, >= 1 frame lost each): reader too slow.
  uint32_t rx_overflows() const { return this->rx_overflows_; }
  /// Frames lost because the RX queue to the main loop was full: main loop too slow.
  uint32_t queue_drops() const { return this->queue_drops_; }
  /// Most frames ever waiting in the RX queue.
  uint32_t queue_high_water() const { return this->queue_high_water_; }
  uint32_t frames_read() const { return this->frames_read_; }
  /// Snapshot of the controller state, for diagnosing a stalled reader or a faulty bus.
  void read_diag(uint8_t &intf, uint8_t &eflg, uint8_t &rec, uint8_t &tec, bool &int_low);
  /// Worst INT-edge-to-reader-wake latency and drain duration since the last call, in us.
  /// Switches between listen-only (no ACK, no TX) and normal mode at runtime,
  /// e.g. for a diagnostic request. Returns false if the controller refused.
  bool set_listen_only(bool listen_only);
  /// Logs the reader timeline frozen at the last RX overflow (once per overflow).
  void log_overflow_trace();
  void take_latency_max(uint32_t &wake_us, uint32_t &drain_us, uint32_t &frame_us) {
    wake_us = this->max_wake_us_.exchange(0);
    drain_us = this->max_drain_us_.exchange(0);
    frame_us = this->max_frame_us_.exchange(0);
  }
  static const struct TxBnRegs {
    REGISTER CTRL;
    REGISTER SIDH;
    REGISTER DATA;
  } TXB[N_TXBUFFERS];

  static const struct RxBnRegs {
    REGISTER CTRL;
    REGISTER SIDH;
    REGISTER DATA;
    CANINTF CANINTF_RXnIF;
  } RXB[N_RXBUFFERS];

 protected:
  CanClock mcp_clock_{MCP_8MHZ};
  CanctrlReqopMode mcp_mode_ = CANCTRL_REQOP_NORMAL;
  uint32_t start_delay_ms_{0};
  bool started_{false};  // controller out of config mode; guarded by spi_mutex_ once the reader runs
#ifdef USE_MCP2515_IRQ_SAFE_MODE
  safe_mode::SafeModeComponent *safe_mode_{nullptr};
#endif
  void start_();
  bool setup_internal() override;
  canbus::Error set_mode_(CanctrlReqopMode mode);

  // The driver talks to IDF directly through its own device on the bus ESPHome
  // set up, so the hot path is IRAM-resident (IDF polling transactions, inline
  // GPIO register access) and the reader can hold the bus across several
  // MCP2515 instructions (see drain_()). CS is driven here. These hide SPIBase's.
  void enable();
  void disable();
  void set_cs_(bool level);
  void transfer_(size_t len, bool read);
  GPIOPin *own_cs_{nullptr};
  uint8_t cs_gpio_{0};
  uint8_t int_gpio_{0};
  spi_device_handle_t dev_{nullptr};
  bool bus_held_{false};  // only changed by the reader, under spi_mutex_
  uint8_t read_intf_();

  // Every SPI transaction goes through this buffer. The ESP32's SPI DMA needs
  // 4-byte aligned buffers (otherwise IDF mallocs a bounce buffer per
  // transaction) and RX lengths that are multiples of 4 (otherwise DMA may write
  // out of bounds). Guarded by spi_mutex_ once the reader runs.
  static constexpr size_t XFER_LEN = 20;
  alignas(4) uint8_t xfer_[XFER_LEN]{};
  void spi_write_(size_t len);  // TX only: xfer_[0, len)
  void spi_read_(size_t len);   // full duplex in place, padded to a multiple of 4

  // Interrupt-driven reception: the ISR wakes reader_task_, which drains the RX
  // buffers into rx_queue_; read_message() pops from it in the main loop.
  static void gpio_isr_(MCP2515 *self);
  static void reader_task_(void *arg);
  void drain_();
  void receive_(RXBn rxbn);
  bool read_rx_buffer_(RXBn rxbn, canbus::CanFrame *frame);

  InternalGPIOPin *interrupt_pin_{nullptr};
  uint16_t rx_queue_len_{256};
  TaskHandle_t reader_task_handle_{nullptr};
  QueueHandle_t rx_queue_{nullptr};
  SemaphoreHandle_t spi_mutex_{nullptr};  // serialises reader task and main-loop TX
  // Written by the reader task, read by the main loop.
  std::atomic<uint32_t> rx_overflows_{0};
  std::atomic<uint32_t> queue_drops_{0};
  std::atomic<uint32_t> queue_high_water_{0};
  std::atomic<uint32_t> frames_read_{0};
  std::atomic<uint32_t> max_wake_us_{0};
  std::atomic<uint32_t> max_drain_us_{0};
  std::atomic<uint32_t> max_frame_us_{0};  // one frame: CANINTF read + RX buffer read + queue
  volatile int64_t isr_us_{0};  // time of the last INT edge

  // Reader timeline: recent events, frozen at an RX overflow for post-mortem.
  enum TraceType : uint8_t { TRACE_ISR, TRACE_WAKE, TRACE_RXB0, TRACE_RXB1, TRACE_DRAIN_END, TRACE_OVERFLOW };
  struct TraceEvent {
    uint32_t us;  // esp_timer_get_time(), truncated
    TraceType type;
    uint8_t intf;
  };
  static constexpr size_t TRACE_LEN = 32;
  void trace_(TraceType type, uint8_t intf, int64_t us);
  TraceEvent trace_buf_[TRACE_LEN]{};
  size_t trace_head_{0};
  TraceEvent frozen_[TRACE_LEN]{};
  size_t frozen_head_{0};
  std::atomic<bool> frozen_pending_{false};

  uint8_t read_register_(REGISTER reg);
  void read_registers_(REGISTER reg, uint8_t values[], uint8_t n);
  void set_register_(REGISTER reg, uint8_t value);
  void set_registers_(REGISTER reg, uint8_t values[], uint8_t n);
  void modify_register_(REGISTER reg, uint8_t mask, uint8_t data);

  void prepare_id_(uint8_t *buffer, bool extended, uint32_t id);
  canbus::Error reset_();
  canbus::Error set_clk_out_(CanClkOut divisor);
  canbus::Error set_bitrate_(canbus::CanSpeed can_speed);
  canbus::Error set_bitrate_(canbus::CanSpeed can_speed, CanClock can_clock);
  canbus::Error set_filter_mask_(MASK mask, bool extended, uint32_t ul_data);
  canbus::Error set_filter_(RXF num, bool extended, uint32_t ul_data);
  canbus::Error send_message_(TXBn txbn, struct canbus::CanFrame *frame);
  canbus::Error send_message(struct canbus::CanFrame *frame) override;
  canbus::Error read_message_(RXBn rxbn, struct canbus::CanFrame *frame);
  canbus::Error read_message(struct canbus::CanFrame *frame) override;
  bool check_receive_();
  bool check_error_();
  uint8_t get_error_flags_();
  void clear_rx_n_ovr_flags_();
  uint8_t get_int_();
  uint8_t get_int_mask_();
  void clear_int_();
  void clear_tx_int_();
  uint8_t get_status_();
  void clear_rx_n_ovr_();
  void clear_merr_();
  void clear_errif_();
};
}  // namespace esphome::mcp2515_irq
