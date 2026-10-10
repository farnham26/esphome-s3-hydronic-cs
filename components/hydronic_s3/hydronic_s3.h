#pragma once

#include <cmath>
#include <functional>
#include <cstdint>
#include <map>
#include <vector>

#include "esphome/components/canbus/canbus.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

namespace esphome::hydronic_s3 {

/// Last payload and timing of one CAN ID, as seen on the heater bus.
struct IdState {
  uint8_t data[8]{};
  uint8_t len{0};
  bool ext{false};
  uint8_t changed{0};       // bytes that changed since the last dump
  uint8_t changes[8]{};     // per-byte change count, saturating
  uint32_t count{0};
  uint32_t last_ms{0};
  float period_ms{0};       // smoothed inter-frame interval, at processing time (approximate)
};

/// Values read by UDS ReadDataByIdentifier (see docs/research.md "DID correlation").
/// (?) = plausible, not confirmed.
enum DidSensor : uint8_t {
  DID_INLET_TEMPERATURE,      // FD01 w2, v/100 - 50 °C
  DID_FLAME_SENSOR_TEMP,      // FD01 w1
  DID_SUPPLY_VOLTAGE,         // FD04 0.1 V (26.0 vs. 26.14 V measured)
  DID_BLOWER_SPEED,           // FD09 rpm (?)
  DID_GLOW_PLUG_POWER,        // FD0A 0.1 W (?)
  DID_GLOW_PLUG_RAW,          // FD0C, non-zero while glowing (?)
  DID_METERING_PUMP_RATE,     // FD0D 0.1 Hz (?)
  DID_OPERATING_PHASE,        // FD10 B1: 0x0C start, 0x14 burn, 0x1F run-down, 0x28 pause
  DID_FUEL_CONSUMPTION,       // FD11 B1 µl/s (= 29.3 µl/stroke x FD0D; 41 at the 0.15 l/h minimum load)
  DID_HEATING_POWER,          // FD11 B1 x D6L datasheet heat flow per fuel (estimate)
  DID_FUEL_TOTAL,             // FD11 B2-5 litres + B6-9 nl (rollover at 1e9 assumed)
  DID_HOURS_TOTAL,            // FD17 counters, minutes: total, power bands, pause, altitude
  DID_HOURS_POWER_0_25,
  DID_HOURS_POWER_25_50,
  DID_HOURS_POWER_50_75,
  DID_HOURS_POWER_75_100,
  DID_HOURS_PAUSE,
  DID_HOURS_ALTITUDE,
  DID_FD08_RAW,               // u16: ~23100 burn, ~24650 run-down, 20000 pause (water pump?)
  DID_FD35_RAW,               // u16: 10000..10600, all phases
  DID_SENSOR_COUNT,
};

/// D6L datasheet: 5600 W at 0.65 l/h, 1300 W at 0.15 l/h (linear within 1 %).
static constexpr float HEAT_FLOW_W_PER_L_H = 5600.0f / 0.65f;

/// Eberspächer Hydronic S3 on CAN (EasyStart protocol, 11-bit, 500 kbit/s).
/// Listen-only: decodes what is known, and logs new IDs and changing bytes so
/// the rest can be decoded from logs.
class HydronicS3 : public Component {
 public:
  void set_canbus(canbus::Canbus *bus) { this->canbus_ = bus; }
#ifdef USE_SENSOR
  void set_outlet_temperature_sensor(sensor::Sensor *s) { this->outlet_temperature_ = s; }
  void set_state_code_sensor(sensor::Sensor *s) { this->state_code_ = s; }
  void set_controller_temperature_sensor(sensor::Sensor *s) { this->controller_temperature_ = s; }
  void set_did_sensor(DidSensor which, sensor::Sensor *s) { this->did_sensors_[which] = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_state_text_sensor(text_sensor::TextSensor *s) { this->state_text_ = s; }
  void set_fault_text_sensor(text_sensor::TextSensor *s) { this->fault_text_ = s; }
  void set_event_text_sensor(text_sensor::TextSensor *s) { this->event_text_ = s; }
  void set_fault_codes_text_sensor(text_sensor::TextSensor *s) { this->fault_codes_text_ = s; }
  void set_diag_response_text_sensor(text_sensor::TextSensor *s) { this->diag_response_text_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_on_request_binary_sensor(binary_sensor::BinarySensor *s) { this->on_request_ = s; }
  void set_flame_binary_sensor(binary_sensor::BinarySensor *s) { this->flame_ = s; }
#endif

  /// Last decoded values; NAN / -1 until received.
  float outlet_temperature() const { return this->outlet_temp_c_; }
  int state() const { return this->state_; }
  int on_request() const { return this->on_request_raw_; }
  float controller_temperature() const { return this->controller_temp_c_; }
  /// 0x2C4 B2 while in fault or its ~26 s aftermath, 0 otherwise; -1 until received.
  int fault_code() const { return this->fault_code_; }
  static const char *state_name(int state);
  /// Sends UDS ReadDTCInformation (19 02 <mask>) as the EasyStart's error-memory
  /// readout does. The bus controller must be out of listen-only mode.
  void request_fault_codes(uint8_t status_mask);
  /// Reads DIDs first..last one by one with UDS ReadDataByIdentifier (22); positive
  /// answers go to the diag_response text sensor. The bus controller is taken out of
  /// listen-only mode for the scan through the listen-only hook.
  void scan_dids(uint16_t first, uint16_t last);
  /// Same for KWP2000-style ReadDataByLocalIdentifier (21 <id>), ids first..last.
  void scan_local_ids(uint8_t first, uint8_t last);
  /// Sends one UDS request given as hex ("19 04 00 02 52 FF"); the answer goes to the
  /// diag_response text sensor. Normal mode for 3 s via the listen-only hook.
  void send_uds_request(const std::string &hex);
  /// Reads the given DIDs in a sweep every interval_ms until stop_polling(); answers
  /// go to the diag_response text sensor. Normal mode while polling.
  void start_polling(std::vector<uint16_t> dids, uint32_t interval_ms);
  void stop_polling();
  /// The DIDs behind the DidSensor values and the flame flag.
  static std::vector<uint16_t> sensor_dids() {
    return {0xFD01, 0xFD04, 0xFD08, 0xFD09, 0xFD0A, 0xFD0C, 0xFD0D, 0xFD0F, 0xFD10, 0xFD11, 0xFD17, 0xFD35};
  }
  /// Switches the bus controller between listen-only (true) and normal mode.
  void set_listen_only_hook(std::function<bool(bool)> hook) { this->listen_only_hook_ = std::move(hook); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /// Key: CAN ID, bit 31 set for extended IDs.
  const std::map<uint32_t, IdState> &ids() const { return this->ids_; }
  uint32_t frames() const { return this->frames_; }

 protected:
  void on_frame_(uint32_t can_id, bool ext, bool rtr, const std::vector<uint8_t> &data);
  void dump_ids_();
  void decode_(uint32_t can_id, const std::vector<uint8_t> &data);
  void event_(const char *what, int from, int to);
  void diag_response_(const std::vector<uint8_t> &data);
  void uds_(const std::vector<uint8_t> &payload);
  void decode_did_(uint16_t did, const uint8_t *d, size_t n);

  canbus::Canbus *canbus_{nullptr};
  std::map<uint32_t, IdState> ids_;
  uint32_t frames_{0};

  float outlet_temp_c_{NAN};
  int state_{-1};
  int on_request_raw_{-1};
  float controller_temp_c_{NAN};
  int command_raw_{-1};       // 0x054 B0 << 8 | B1
  int switch_input_raw_{-1};  // 0x625 B1
  std::vector<uint8_t> isotp_buf_;  // diagnostic response being reassembled
  size_t isotp_len_{0};
  bool diag_requested_{false};  // we are the tester: answer first frames with flow control
  void publish_diag_(const std::string &text);
  std::function<bool(bool)> listen_only_hook_;
  bool scan_active_{false};
  bool scan_awaiting_{false};
  uint32_t scan_next_{0};  // 32 bit: a scan up to FFFF must not wrap
  uint32_t scan_last_{0};
  uint32_t scan_sent_ms_{0};
  uint32_t scan_timeout_ms_{0};
  uint16_t scan_positive_{0};
  uint16_t scan_negative_{0};
  uint16_t scan_silent_{0};
  uint8_t scan_service_{0x22};
  std::vector<uint16_t> poll_dids_;  // non-empty: sweep this list instead of a range
  size_t poll_index_{0};
  bool polling_{false};
  uint32_t poll_interval_ms_{0};
  uint32_t poll_next_ms_{0};
  uint16_t nrc_count_[256]{};  // negative response codes seen during the scan
  void start_scan_(uint8_t service, uint32_t first, uint32_t last);
  uint32_t single_until_ms_{0};  // single request: back to listen-only after this
  uint32_t scan_rtt_sum_ms_{0};  // request to answer, answered requests only
  uint32_t scan_rtt_max_ms_{0};
  void scan_answered_();
  int fault_code_{-1};
#ifdef USE_SENSOR
  sensor::Sensor *outlet_temperature_{nullptr};
  sensor::Sensor *state_code_{nullptr};
  sensor::Sensor *controller_temperature_{nullptr};
  sensor::Sensor *did_sensors_[DID_SENSOR_COUNT]{};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *state_text_{nullptr};
  text_sensor::TextSensor *fault_text_{nullptr};
  text_sensor::TextSensor *event_text_{nullptr};
  text_sensor::TextSensor *fault_codes_text_{nullptr};
  text_sensor::TextSensor *diag_response_text_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *on_request_{nullptr};
  binary_sensor::BinarySensor *flame_{nullptr};
#endif
};

}  // namespace esphome::hydronic_s3
