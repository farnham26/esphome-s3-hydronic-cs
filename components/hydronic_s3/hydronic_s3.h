#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "esphome/components/canbus/canbus.h"
#include "esphome/core/component.h"

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

/// Eberspächer Hydronic S3 on CAN. Listen-only for now: discovers the IDs on
/// the bus and logs what changes, so the protocol can be decoded from logs.
class HydronicS3 : public Component {
 public:
  void set_canbus(canbus::Canbus *bus) { this->canbus_ = bus; }

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

  canbus::Canbus *canbus_{nullptr};
  std::map<uint32_t, IdState> ids_;
  uint32_t frames_{0};
  uint32_t gaps_{0};
};

}  // namespace esphome::hydronic_s3
