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
  std::copy(data.begin(), data.begin() + len, s.data);
  s.len = len;
  s.ext = ext;
  s.count++;
  s.last_ms = now;
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
