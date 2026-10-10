#include "hydronic_s3.h"

#include <cinttypes>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::hydronic_s3 {

static const char *const TAG = "hydronic_s3";

static constexpr uint32_t EXT_FLAG = 1u << 31;
static constexpr uint32_t DUMP_INTERVAL_MS = 10000;
// A byte that has changed this often is a counter or an analog value: its
// changes stop being logged one by one and show in the periodic dump only.
static constexpr uint8_t QUIET_AFTER_CHANGES = 16;
// Frames counted before the period is trusted for gap detection.
static constexpr uint32_t PERIOD_SETTLE_FRAMES = 10;
static constexpr float GAP_FACTOR = 1.8f;

// Decoded so far (heater -> controller unless noted), all little-endian:
//   0x2C4 B0      heater state: 0x03 idle, 0x05 active (heating, control pause and
//                 after-run after switch-off; others not yet seen)
//   0x2C4 B1      bit 3: burner run-down (~65-90 s, into control pause or at switch-off)
//   0x2C4 B4..B5  outlet coolant temperature, 0.1 degC, offset -50 degC
//                 (holds ~75 degC in regulation, drops at once when the mixer opens)
//   0x054 B0      controller -> heater: 0x01 heater requested on, 0x00 off
static constexpr uint32_t ID_COMMAND = 0x054;
static constexpr uint32_t ID_STATUS = 0x2C4;

const char *HydronicS3::state_name(int state) {
  switch (state) {
    case 0x03:
      return "idle";
    case 0x05:
      return "active";
    default:
      return nullptr;
  }
}

static void hex(char *out, const uint8_t *d, uint8_t len) {
  static const char HX[] = "0123456789ABCDEF";
  for (uint8_t i = 0; i < len; i++) {
    *out++ = HX[d[i] >> 4];
    *out++ = HX[d[i] & 0xF];
    *out++ = ' ';
  }
  *out = 0;
}

void HydronicS3::setup() {
  this->canbus_->add_callback([this](uint32_t can_id, bool ext, bool rtr, const std::vector<uint8_t> &data) {
    this->on_frame_(can_id, ext, rtr, data);
  });
  this->set_interval("dump", DUMP_INTERVAL_MS, [this]() { this->dump_ids_(); });
}

void HydronicS3::dump_config() { ESP_LOGCONFIG(TAG, "Hydronic S3 (listen-only discovery)"); }

void HydronicS3::on_frame_(uint32_t can_id, bool ext, bool rtr, const std::vector<uint8_t> &data) {
  const uint32_t now = millis();
  const uint8_t len = data.size() > 8 ? 8 : data.size();
  char buf[25];
  this->frames_++;

  auto [it, fresh] = this->ids_.try_emplace(can_id | (ext ? EXT_FLAG : 0));
  IdState &s = it->second;
  if (fresh) {
    hex(buf, data.data(), len);
    if (ext) {
      // J1939: prio(3) EDP/DP(2) PF(8) PS(8) SA(8). PS is a destination for PF < 240 (PDU1).
      const uint8_t pf = can_id >> 16, ps = can_id >> 8, sa = can_id;
      const uint32_t pgn = (can_id >> 8) & (pf >= 240 ? 0x3FFFF : 0x3FF00);
      ESP_LOGI(TAG, "new ext %08" PRIX32 " [%u] %s  J1939 prio %u PGN %" PRIu32 " (0x%05" PRIX32 ") SA %u%s%s",
               can_id, len, rtr ? "RTR" : buf, (unsigned) (can_id >> 26) & 7, pgn, pgn, sa,
               pf < 240 ? " DA " : "", pf < 240 ? to_string(ps).c_str() : "");
    } else {
      ESP_LOGI(TAG, "new std %03" PRIX32 " [%u] %s", can_id, len, rtr ? "RTR" : buf);
    }
  } else {
    const float dt = now - s.last_ms;
    if (s.count >= PERIOD_SETTLE_FRAMES && dt > GAP_FACTOR * s.period_ms) {
      s.gaps++;
      this->gaps_++;
    } else {
      s.period_ms = s.count == 1 ? dt : 0.9f * s.period_ms + 0.1f * dt;
    }
    for (uint8_t b = 0; b < len; b++) {
      if (b < s.len && data[b] == s.data[b])
        continue;
      s.changed |= 1 << b;
      if (s.changes[b] < QUIET_AFTER_CHANGES) {
        hex(buf, data.data(), len);
        ESP_LOGD(TAG, "%0*" PRIX32 " B%u %02X->%02X  %s", ext ? 8 : 3, can_id, b, s.data[b], data[b], buf);
      }
      if (s.changes[b] < UINT8_MAX)
        s.changes[b]++;
    }
  }
  if (!ext && !rtr)
    this->decode_(can_id, data);
  std::copy(data.begin(), data.begin() + len, s.data);
  s.len = len;
  s.ext = ext;
  s.count++;
  s.last_ms = now;
}

void HydronicS3::decode_(uint32_t can_id, const std::vector<uint8_t> &data) {
  if (can_id == ID_STATUS && data.size() >= 6) {
    const float temp = ((data[4] | data[5] << 8) - 500) * 0.1f;
    if (temp != this->outlet_temp_c_) {
      this->outlet_temp_c_ = temp;
#ifdef USE_SENSOR
      if (this->outlet_temperature_ != nullptr)
        this->outlet_temperature_->publish_state(temp);
#endif
    }
    if (data[0] != this->state_) {
      this->state_ = data[0];
      const char *name = state_name(this->state_);
      ESP_LOGI(TAG, "state 0x%02X (%s)", this->state_, name != nullptr ? name : "unknown");
#ifdef USE_SENSOR
      if (this->state_code_ != nullptr)
        this->state_code_->publish_state(this->state_);
#endif
#ifdef USE_TEXT_SENSOR
      if (this->state_text_ != nullptr) {
        char buf[8];
        snprintf(buf, sizeof(buf), "0x%02X", this->state_);
        this->state_text_->publish_state(name != nullptr ? name : buf);
      }
#endif
    }
  } else if (can_id == ID_COMMAND && !data.empty() && data[0] != this->on_request_raw_) {
    this->on_request_raw_ = data[0];
#ifdef USE_BINARY_SENSOR
    if (this->on_request_ != nullptr)
      this->on_request_->publish_state(data[0] == 0x01);
#endif
  }
}

void HydronicS3::dump_ids_() {
  if (this->ids_.empty()) {
    ESP_LOGW(TAG, "no frames received");
    return;
  }
  const uint32_t now = millis();
  char buf[25], mask[9];
  ESP_LOGI(TAG, "%u IDs, %" PRIu32 " frames, %" PRIu32 " gaps", (unsigned) this->ids_.size(), this->frames_,
           this->gaps_);
  for (auto &[key, s] : this->ids_) {
    hex(buf, s.data, s.len);
    for (uint8_t b = 0; b < 8; b++)
      mask[b] = b >= s.len ? ' ' : (s.changed >> b & 1) ? '^' : '.';
    mask[8] = 0;
    ESP_LOGI(TAG, "  %0*" PRIX32 " n=%-6" PRIu32 " T=%6.0fms gaps=%-4" PRIu32 " age=%5" PRIu32 "ms  %-24s %s",
             s.ext ? 8 : 3, key & ~EXT_FLAG, s.count, s.period_ms, s.gaps, now - s.last_ms, buf, mask);
    s.changed = 0;
  }
}

}  // namespace esphome::hydronic_s3
