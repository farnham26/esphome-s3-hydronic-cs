#include "hydronic_s3.h"

#include <cinttypes>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

// Research aids (research: true): per-ID table, change log, DID/local-ID range scans.

namespace esphome::hydronic_s3 {

static const char *const TAG = "hydronic_s3";

static constexpr uint32_t EXT_FLAG = 1u << 31;
// A byte that has changed this often is a counter or an analog value: its
// changes stop being logged one by one and show in the periodic dump only.
static constexpr uint8_t QUIET_AFTER_CHANGES = 16;

// Research: per-ID table (period, gaps), new IDs and per-byte changes in the log.
void HydronicS3::track_id_(uint32_t can_id, bool ext, bool rtr, const std::vector<uint8_t> &data, uint32_t now) {
  const uint8_t len = data.size() > 8 ? 8 : data.size();
  char buf[25];
  auto [it, fresh] = this->ids_.try_emplace(can_id | (ext ? EXT_FLAG : 0));
  IdState &s = it->second;
  if (fresh) {
    hex(buf, data.data(), len);
    if (ext) {
      // J1939: prio(3) EDP/DP(2) PF(8) PS(8) SA(8). PS is a destination for PF < 240 (PDU1).
      const uint8_t pf = can_id >> 16, ps = can_id >> 8, sa = can_id;
      const uint32_t pgn = (can_id >> 8) & (pf >= 240 ? 0x3FFFF : 0x3FF00);
      ESP_LOGD(TAG, "new ext %08" PRIX32 " [%u] %s  J1939 prio %u PGN %" PRIu32 " (0x%05" PRIX32 ") SA %u%s%s",
               can_id, len, rtr ? "RTR" : buf, (unsigned) (can_id >> 26) & 7, pgn, pgn, sa,
               pf < 240 ? " DA " : "", pf < 240 ? to_string(ps).c_str() : "");
    } else {
      ESP_LOGD(TAG, "new std %03" PRIX32 " [%u] %s", can_id, len, rtr ? "RTR" : buf);
    }
  } else {
    const float dt = now - s.last_ms;
    s.period_ms = s.count == 1 ? dt : 0.9f * s.period_ms + 0.1f * dt;
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

void HydronicS3::scan_dids(uint16_t first, uint16_t last) { this->start_scan_(0x22, first, last); }
void HydronicS3::scan_local_ids(uint8_t first, uint8_t last) { this->start_scan_(0x21, first, last); }
void HydronicS3::scan_routines(uint16_t first, uint16_t last) { this->start_scan_(0x31, first, last); }

void HydronicS3::start_scan_(uint8_t service, uint32_t first, uint32_t last) {
  if (this->sweep_)  // pre-empt a poll sweep (its pending answer is still awaited); polling resumes after the scan
    this->scan_active_ = false;
  if (this->scan_active_ || !this->set_listen_only_(false)) {
    ESP_LOGW(TAG, "scan not started");
    return;
  }
  this->scan_service_ = service;
  std::fill(std::begin(this->nrc_count_), std::end(this->nrc_count_), 0);
  this->scan_active_ = true;
  this->sweep_ = false;
  this->scan_next_ = first;
  this->scan_last_ = last;
  this->scan_positive_ = this->scan_negative_ = this->scan_silent_ = 0;
  this->scan_rtt_sum_ms_ = this->scan_rtt_max_ms_ = 0;
  ESP_LOGI(TAG, "scan service %02X, %04X..%04X", service, (unsigned) first, (unsigned) last);
}

void HydronicS3::dump_ids_() {
  // Nothing new (bus idle or disconnected): one line, not the full table every time.
  if (this->frames_ == this->dumped_frames_)
    return;
  this->dumped_frames_ = this->frames_;
  if (this->ids_.empty()) {
    ESP_LOGI(TAG, "no frames received");
    return;
  }
  const uint32_t now = millis();
  char buf[25], mask[9];
  ESP_LOGD(TAG, "%u IDs, %" PRIu32 " frames", (unsigned) this->ids_.size(), this->frames_);
  for (auto &[key, s] : this->ids_) {
    hex(buf, s.data, s.len);
    for (uint8_t b = 0; b < 8; b++)
      mask[b] = b >= s.len ? ' ' : (s.changed >> b & 1) ? '^' : '.';
    mask[8] = 0;
    ESP_LOGD(TAG, "  %0*" PRIX32 " n=%-6" PRIu32 " T~%6.0fms age=%5" PRIu32 "ms  %-24s %s", s.ext ? 8 : 3,
             key & ~EXT_FLAG, s.count, s.period_ms, now - s.last_ms, buf, mask);
    s.changed = 0;
  }
}

}  // namespace esphome::hydronic_s3
