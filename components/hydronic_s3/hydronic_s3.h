#pragma once

#include <cmath>
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
  float period_ms{0};       // smoothed inter-frame interval
  uint32_t gaps{0};         // intervals well above period_ms: frames lost
};

/// Eberspächer Hydronic S3 on CAN (EasyStart protocol, 11-bit, 500 kbit/s).
/// Listen-only: decodes what is known, and logs new IDs and changing bytes so
/// the rest can be decoded from logs.
class HydronicS3 : public Component {
 public:
  void set_canbus(canbus::Canbus *bus) { this->canbus_ = bus; }
#ifdef USE_SENSOR
  void set_outlet_temperature_sensor(sensor::Sensor *s) { this->outlet_temperature_ = s; }
  void set_state_code_sensor(sensor::Sensor *s) { this->state_code_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_state_text_sensor(text_sensor::TextSensor *s) { this->state_text_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_on_request_binary_sensor(binary_sensor::BinarySensor *s) { this->on_request_ = s; }
#endif

  /// Last decoded values; NAN / -1 until received.
  float outlet_temperature() const { return this->outlet_temp_c_; }
  int state() const { return this->state_; }
  int on_request() const { return this->on_request_raw_; }
  static const char *state_name(int state);

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /// Key: CAN ID, bit 31 set for extended IDs.
  const std::map<uint32_t, IdState> &ids() const { return this->ids_; }
  uint32_t frames() const { return this->frames_; }
  uint32_t gaps() const { return this->gaps_; }

 protected:
  void on_frame_(uint32_t can_id, bool ext, bool rtr, const std::vector<uint8_t> &data);
  void dump_ids_();
  void decode_(uint32_t can_id, const std::vector<uint8_t> &data);

  canbus::Canbus *canbus_{nullptr};
  std::map<uint32_t, IdState> ids_;
  uint32_t frames_{0};
  uint32_t gaps_{0};

  float outlet_temp_c_{NAN};
  int state_{-1};
  int on_request_raw_{-1};
#ifdef USE_SENSOR
  sensor::Sensor *outlet_temperature_{nullptr};
  sensor::Sensor *state_code_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *state_text_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *on_request_{nullptr};
#endif
};

}  // namespace esphome::hydronic_s3
