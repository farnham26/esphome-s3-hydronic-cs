#pragma once

#include "esphome/components/canbus/canbus.h"
#include "esphome/components/spi/spi.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "mcp2515_irq_defs.h"

#include <algorithm>
#include <atomic>

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

class MCP2515 final : public canbus::Canbus,
                      public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW, spi::CLOCK_PHASE_LEADING,
                                            spi::DATA_RATE_8MHZ> {
 public:
  MCP2515(){};
  void set_mcp_clock(CanClock clock) { this->mcp_clock_ = clock; };
  void set_mcp_mode(const CanctrlReqopMode mode) { this->mcp_mode_ = mode; }
  void set_interrupt_pin(InternalGPIOPin *pin) { this->interrupt_pin_ = pin; }
  void set_rx_queue_len(uint16_t len) { this->rx_queue_len_ = len; }

  /// Overflow events in the MCP2515 (both RX buffers full, >= 1 frame lost each): reader too slow.
  uint32_t rx_overflows() const { return this->rx_overflows_; }
  /// Frames lost because the RX queue to the main loop was full: main loop too slow.
  uint32_t queue_drops() const { return this->queue_drops_; }
  /// Most frames ever waiting in the RX queue.
  uint32_t queue_high_water() const { return this->queue_high_water_; }
  uint32_t frames_read() const { return this->frames_read_; }
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
  bool setup_internal() override;
  canbus::Error set_mode_(CanctrlReqopMode mode);

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
