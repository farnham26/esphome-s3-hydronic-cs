#include "hydronic_s3.h"

#include <algorithm>

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

// Decoded so far (heater -> controller unless noted), all little-endian:
//   0x2C4 B0      heater state: 0x03 idle, 0x05 active (heating, control pause and
//                 after-run after switch-off; others not yet seen)
//   0x2C4 B1      bit 3: burner run-down (~65-90 s, into control pause or at switch-off)
//   0x2C4 B4..B5  outlet coolant temperature, 0.1 degC, offset -50 degC
//                 (holds ~75 degC in regulation, drops at once when the mixer opens)
//   0x2C4 B2      flags. Airtronic S3 RE: fault code in state 0x0B (0x20 flame loss, 0x40
//                 overtemperature). Hydronic: 0x40 set 1.7 s after a switch-on request whose
//                 start is held/refused (heater stays idle), cleared with the request
//   0x2C4 B4..B5  FE FF = not available (first frame after the heater wakes, state 0x00)
//   0x625 B1      0x10 while a switch-on request is active (S+ or EasyStart), 0x00 otherwise
//   0x054 B0      controller -> heater: 0x01 heater requested on, 0x00 off
//   0x065 B0..B1  controller -> heater: controller (EasyStart) room temperature, same
//                 encoding as the outlet temperature (hypothesis: 18-19 degC, 1 K steps)
static constexpr uint32_t ID_COMMAND = 0x054;
static constexpr uint32_t ID_CONTROLLER_STATUS = 0x065;
static constexpr uint32_t ID_STATUS = 0x2C4;
static constexpr uint32_t ID_HEARTBEAT = 0x625;
// Diagnostics: UDS over ISO-TP, tester 0x7A0 -> heater 0x73C.
static constexpr uint32_t ID_DIAG_REQUEST = 0x7A0;
static constexpr uint32_t ID_DIAG_RESPONSE = 0x73C;
static constexpr uint8_t ISOTP_PAD = 0xAA;
static constexpr uint32_t DIAG_TIMEOUT_MS = 200;
static constexpr uint32_t DIAG_PENDING_TIMEOUT_MS = 2000;  // after NRC 0x78 "response pending"
static constexpr uint8_t NRC_RESPONSE_PENDING = 0x78;
static constexpr uint16_t NOT_AVAILABLE = 0xFFFE;
static constexpr uint8_t STATE_FAULT = 0x0B;
static constexpr uint32_t STALE_MS = 15000;  // no update for 3 poll sweeps: value unknown

static float decode_temperature(const std::vector<uint8_t> &data, size_t offset) {
  const uint16_t raw = data[offset] | data[offset + 1] << 8;
  return raw >= NOT_AVAILABLE ? NAN : (raw - 500) * 0.1f;
}

const char *HydronicS3::state_name(int state) {
  switch (state) {
    case 0x00:
      return "init";
    case 0x03:
      return "idle";
    case 0x05:
      return "active";
    case STATE_FAULT:
      return "fault";
    default:
      return nullptr;
  }
}

const char *HydronicS3::phase_name(int phase) {
  switch (phase) {
    case 0x00:
      return "Off";
    case 0x0C:
      return "Start";
    case 0x14:
      return "Burn";
    case 0x1E:
    case 0x1F:
      return "Run-down";
    case 0x28:
      return "Pause";
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
  this->set_interval("stale", 1000, [this]() { this->expire_stale_(); });
#ifdef USE_BINARY_SENSOR
  if (this->connected_ != nullptr)
    this->connected_->publish_state(false);
#endif
}

// Values not updated within STALE_MS (bus disconnected, heater unpowered, polling
// stopped) become unknown: NAN / -1 internally, NAN or invalid on the entities.
void HydronicS3::expire_stale_() {
  const uint32_t now = millis();
  auto stale = [now](uint32_t &ms) {
    if (ms == 0 || now - ms < STALE_MS)
      return false;
    ms = 0;
    return true;
  };
  if (stale(this->status_ms_)) {
    ESP_LOGW(TAG, "heater disconnected: no status frames for %u s", (unsigned) (STALE_MS / 1000));
    this->event_("connected", 1, 0);
    // Re-read the identification on reconnect: it may be another heater.
    this->ident_dids_ = {0xF193, 0xF195, 0xF1F0, 0xF1F5};
    this->isotp_len_ = 0;
    this->outlet_temp_c_ = NAN;
    this->state_ = -1;
    this->fault_code_ = -1;
#ifdef USE_SENSOR
    if (this->outlet_temperature_ != nullptr)
      this->outlet_temperature_->publish_state(NAN);
    if (this->state_code_ != nullptr)
      this->state_code_->publish_state(NAN);
#endif
#ifdef USE_BINARY_SENSOR
    if (this->connected_ != nullptr)
      this->connected_->publish_state(false);
#endif
#ifdef USE_TEXT_SENSOR
    if (this->state_text_ != nullptr)
      this->state_text_->publish_state("");
    if (this->fault_text_ != nullptr)
      this->fault_text_->publish_state("");
#endif
  }
  if (stale(this->controller_ms_)) {
    this->controller_temp_c_ = NAN;
#ifdef USE_SENSOR
    if (this->controller_temperature_ != nullptr)
      this->controller_temperature_->publish_state(NAN);
#endif
  }
  if (stale(this->command_ms_)) {
    this->command_raw_ = -1;
    this->on_request_raw_ = -1;
#ifdef USE_BINARY_SENSOR
    if (this->on_request_ != nullptr)
      this->on_request_->invalidate_state();
#endif
  }
  if (stale(this->heartbeat_ms_))
    this->switch_input_raw_ = -1;
#ifdef USE_BINARY_SENSOR
  if (stale(this->flame_ms_) && this->flame_ != nullptr)
    this->flame_->invalidate_state();
#endif
#ifdef USE_SENSOR
  for (size_t i = 0; i < DID_SENSOR_COUNT; i++)
    if (stale(this->did_ms_[i]) && this->did_sensors_[i] != nullptr)
      this->did_sensors_[i]->publish_state(NAN);
#endif
}

void HydronicS3::dump_config() { ESP_LOGCONFIG(TAG, "Hydronic S3 (listen-only discovery)"); }

void HydronicS3::on_frame_(uint32_t can_id, bool ext, bool rtr, const std::vector<uint8_t> &data) {
  const uint32_t now = millis();
  const uint8_t len = data.size() > 8 ? 8 : data.size();
  char buf[25];
  this->frames_++;
  if (this->tx_down_) {
    this->tx_down_ = false;
    this->poll_next_ms_ = now;
    ESP_LOGI(TAG, "bus back, polling resumed");
  }

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
  if (!ext && !rtr)
    this->decode_(can_id, data);
  std::copy(data.begin(), data.begin() + len, s.data);
  s.len = len;
  s.ext = ext;
  s.count++;
  s.last_ms = now;
}

// Transitions with the device time, as one entity state each: entity updates
// reach HA reliably where log lines are dropped on a weak link.
void HydronicS3::event_(const char *what, int from, int to) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%u.%03u %s %02X->%02X", (unsigned) (millis() / 1000), (unsigned) (millis() % 1000), what,
           from & 0xFFFF, to & 0xFFFF);
  ESP_LOGI(TAG, "event %s", buf);
#ifdef USE_TEXT_SENSOR
  if (this->event_text_ != nullptr)
    this->event_text_->publish_state(buf);
#endif
}

void HydronicS3::decode_(uint32_t can_id, const std::vector<uint8_t> &data) {
  if (can_id == ID_STATUS && data.size() >= 6) {
    if (this->status_ms_ == 0) {
      ESP_LOGI(TAG, "heater connected");
      this->event_("connected", 0, 1);
#ifdef USE_BINARY_SENSOR
      if (this->connected_ != nullptr)
        this->connected_->publish_state(true);
#endif
    }
    this->status_ms_ = millis();
    const float temp = decode_temperature(data, 4);
    if (!std::isnan(temp) && temp != this->outlet_temp_c_) {
      this->outlet_temp_c_ = temp;
#ifdef USE_SENSOR
      if (this->outlet_temperature_ != nullptr)
        this->outlet_temperature_->publish_state(temp);
#endif
    }
    if (data[0] != this->state_) {
      this->event_("state", this->state_, data[0]);
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
    if (data[2] != this->fault_code_) {
      this->event_("flags", this->fault_code_, data[2]);
      this->fault_code_ = data[2];
      std::string text = "OK";
      if (this->fault_code_ != 0) {
        // Bit meanings differ from the Airtronic (see table above): report raw.
        char code[24];
        snprintf(code, sizeof(code), "flags 0x%02X", this->fault_code_);
        text = code;
        ESP_LOGW(TAG, "%s", code);
      }
#ifdef USE_TEXT_SENSOR
      if (this->fault_text_ != nullptr)
        this->fault_text_->publish_state(text);
#endif
    }
  } else if (can_id == ID_DIAG_RESPONSE && !data.empty()) {
    this->diag_response_(data);
  } else if (can_id == ID_CONTROLLER_STATUS && data.size() >= 2) {
    this->controller_ms_ = millis();
    const float temp = decode_temperature(data, 0);
    if (!std::isnan(temp) && temp != this->controller_temp_c_) {
      this->controller_temp_c_ = temp;
#ifdef USE_SENSOR
      if (this->controller_temperature_ != nullptr)
        this->controller_temperature_->publish_state(temp);
#endif
    }
  } else if (can_id == ID_HEARTBEAT && data.size() >= 2) {
    this->heartbeat_ms_ = millis();
    if (data[1] != this->switch_input_raw_) {
      this->event_("request", this->switch_input_raw_, data[1]);
      this->switch_input_raw_ = data[1];
    }
  } else if (can_id == ID_COMMAND && data.size() >= 2) {
    this->command_ms_ = millis();
    if ((data[0] << 8 | data[1]) == this->command_raw_)
      return;
    this->event_("command", this->command_raw_, data[0] << 8 | data[1]);
    this->command_raw_ = data[0] << 8 | data[1];
    this->on_request_raw_ = data[0];
#ifdef USE_BINARY_SENSOR
    if (this->on_request_ != nullptr)
      this->on_request_->publish_state(data[0] == 0x01);
#endif
  }
}

void HydronicS3::scan_dids(uint16_t first, uint16_t last) { this->start_scan_(0x22, first, last); }
void HydronicS3::scan_local_ids(uint8_t first, uint8_t last) { this->start_scan_(0x21, first, last); }

void HydronicS3::start_scan_(uint8_t service, uint32_t first, uint32_t last) {
  if (this->scan_active_ || !this->listen_only_hook_ || !this->listen_only_hook_(false)) {
    ESP_LOGW(TAG, "scan not started");
    return;
  }
  this->scan_service_ = service;
  std::fill(std::begin(this->nrc_count_), std::end(this->nrc_count_), 0);
  this->scan_active_ = true;
  this->scan_awaiting_ = false;
  this->scan_next_ = first;
  this->scan_last_ = last;
  this->scan_positive_ = this->scan_negative_ = this->scan_silent_ = 0;
  this->scan_rtt_sum_ms_ = this->scan_rtt_max_ms_ = 0;
  ESP_LOGI(TAG, "scan service %02X, %04X..%04X", service, (unsigned) first, (unsigned) last);
}

void HydronicS3::send_uds_request(const std::string &hex) {
  std::vector<uint8_t> payload;
  for (size_t i = 0; i + 1 < hex.size();) {
    if (hex[i] == ' ') {
      i++;
      continue;
    }
    payload.push_back(static_cast<uint8_t>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
    i += 2;
  }
  // Single frame only: up to 7 payload bytes.
  if (payload.empty() || payload.size() > 7 || this->scan_active_ || !this->listen_only_hook_ ||
      !this->listen_only_hook_(false)) {
    ESP_LOGW(TAG, "UDS request '%s' not sent", hex.c_str());
    return;
  }
  std::vector<uint8_t> frame(8, ISOTP_PAD);
  frame[0] = payload.size();
  std::copy(payload.begin(), payload.end(), frame.begin() + 1);
  this->diag_requested_ = true;
  this->single_until_ms_ = millis() + 3000;
  const bool sent = this->canbus_->send_data(ID_DIAG_REQUEST, false, frame) == canbus::ERROR_OK;
  ESP_LOGI(TAG, "UDS request %s %s", hex.c_str(), sent ? "sent" : "send failed");
}

void HydronicS3::start_polling(std::vector<uint16_t> dids, uint32_t interval_ms) {
  if (this->polling_ || this->scan_active_ || dids.empty() || !this->listen_only_hook_ ||
      !this->listen_only_hook_(false))
    return;
  this->poll_dids_ = std::move(dids);
  this->poll_interval_ms_ = interval_ms;
  this->poll_next_ms_ = millis();
  this->polling_ = true;
  ESP_LOGI(TAG, "polling %u DIDs every %u ms", (unsigned) this->poll_dids_.size(), (unsigned) interval_ms);
}

void HydronicS3::stop_polling() {
  if (!this->polling_)
    return;
  this->polling_ = false;
  if (!this->scan_active_) {
    this->diag_requested_ = false;
    this->listen_only_hook_(true);
  }
}

void HydronicS3::loop() {
  if (this->polling_ && !this->scan_active_ && !this->tx_down_ && static_cast<int32_t>(millis() - this->poll_next_ms_) >= 0) {
    // Start a sweep over the list (reuses the scan state machine).
    this->poll_next_ms_ = millis() + this->poll_interval_ms_;
    this->scan_service_ = 0x22;
    this->poll_index_ = 0;
    this->scan_next_ = 0;
    this->sweep_dids_ = this->poll_dids_;
    this->sweep_dids_.insert(this->sweep_dids_.end(), this->ident_dids_.begin(), this->ident_dids_.end());
    this->scan_last_ = this->sweep_dids_.size() - 1;
    this->scan_awaiting_ = false;
    this->scan_active_ = true;
  }
  if (this->single_until_ms_ != 0 && static_cast<int32_t>(millis() - this->single_until_ms_) >= 0) {
    this->single_until_ms_ = 0;
    this->diag_requested_ = false;
    this->listen_only_hook_(true);
  }
  if (!this->scan_active_)
    return;
  const uint32_t now = millis();
  if (this->scan_awaiting_) {
    if (now - this->scan_sent_ms_ < this->scan_timeout_ms_)
      return;
    this->scan_awaiting_ = false;
    this->scan_silent_++;
  }
  if (this->scan_next_ > this->scan_last_ && this->polling_) {  // sweep done, stay in normal mode
    this->scan_active_ = false;
    return;
  }
  if (this->scan_next_ > this->scan_last_) {
    this->scan_active_ = false;
    this->diag_requested_ = false;
    this->listen_only_hook_(true);
    char buf[160];
    const unsigned answered = this->scan_positive_ + this->scan_negative_;
    int n = snprintf(buf, sizeof(buf), "scan %02X done: %u positive, %u negative, %u silent, answer avg %u ms max %u ms, NRC",
                     this->scan_service_, this->scan_positive_, this->scan_negative_, this->scan_silent_,
                     answered ? (unsigned) (this->scan_rtt_sum_ms_ / answered) : 0u, (unsigned) this->scan_rtt_max_ms_);
    for (int nrc = 0; nrc < 256 && n < (int) sizeof(buf) - 12; nrc++)
      if (this->nrc_count_[nrc] != 0)
        n += snprintf(buf + n, sizeof(buf) - n, " %02X:%u", nrc, this->nrc_count_[nrc]);
    this->publish_diag_(buf);
    return;
  }
  const uint16_t id = this->polling_ ? this->sweep_dids_[this->scan_next_++] : static_cast<uint16_t>(this->scan_next_++);
  std::vector<uint8_t> req;
  if (this->scan_service_ == 0x21)
    req = {0x02, 0x21, static_cast<uint8_t>(id), ISOTP_PAD, ISOTP_PAD, ISOTP_PAD, ISOTP_PAD, ISOTP_PAD};
  else
    req = {0x03, 0x22, static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id), ISOTP_PAD, ISOTP_PAD, ISOTP_PAD,
           ISOTP_PAD};
  this->diag_requested_ = true;
  this->scan_awaiting_ = true;
  this->scan_sent_ms_ = now;
  this->scan_timeout_ms_ = DIAG_TIMEOUT_MS;
  if (this->canbus_->send_data(ID_DIAG_REQUEST, false, req) != canbus::ERROR_OK)
    this->send_failed_();
}

// No ack on the bus keeps every TX buffer pending (disconnected bus, heater and
// EasyStart unpowered), so each further send fails. Stop the scan or sweep; a
// received frame proves the bus is back (on_frame_()): a pending request is acked
// then and answered, or the heater broadcasts its status.
void HydronicS3::send_failed_() {
  this->scan_awaiting_ = false;
  this->scan_active_ = false;
  this->diag_requested_ = false;
  this->isotp_len_ = 0;
  if (!this->polling_) {
    this->listen_only_hook_(true);
    this->publish_diag_("scan aborted: send failed");
  } else if (!this->tx_down_) {
    ESP_LOGW(TAG, "send failed, polling paused until frames are received");
  }
  this->tx_down_ = true;
}

void HydronicS3::scan_answered_() {
  if (!this->scan_awaiting_)
    return;
  const uint32_t rtt = millis() - this->scan_sent_ms_;
  this->scan_rtt_sum_ms_ += rtt;
  if (rtt > this->scan_rtt_max_ms_)
    this->scan_rtt_max_ms_ = rtt;
  this->scan_awaiting_ = false;
}

void HydronicS3::publish_diag_(const std::string &text) {
  ESP_LOGI(TAG, "diag %s", text.c_str());
#ifdef USE_TEXT_SENSOR
  if (this->diag_response_text_ != nullptr)
    this->diag_response_text_->publish_state(text);
#endif
}

void HydronicS3::request_fault_codes(uint8_t status_mask) {
  this->diag_requested_ = true;
  const std::vector<uint8_t> req{0x03, 0x19, 0x02, status_mask, ISOTP_PAD, ISOTP_PAD, ISOTP_PAD, ISOTP_PAD};
  const canbus::Error err = this->canbus_->send_data(ID_DIAG_REQUEST, false, req);
  ESP_LOGI(TAG, "fault code request (mask %02X): %s", status_mask, err == canbus::ERROR_OK ? "sent" : "send failed");
}

// ISO-TP reassembly of the heater's diagnostic responses (single, first and
// consecutive frames; flow control comes from the tester on 0x7A0).
void HydronicS3::diag_response_(const std::vector<uint8_t> &data) {
  switch (data[0] >> 4) {
    case 0x0: {  // single frame
      const size_t len = data[0] & 0x0F;
      if (len >= 1 && len < data.size())
        this->uds_(std::vector<uint8_t>(data.begin() + 1, data.begin() + 1 + len));
      this->isotp_len_ = 0;
      this->diag_requested_ = false;
      break;
    }
    case 0x1:  // first frame
      if (data.size() < 2)
        return;
      this->isotp_len_ = (data[0] & 0x0F) << 8 | data[1];
      this->isotp_buf_.assign(data.begin() + 2, data.end());
      if (this->diag_requested_) {
        // Flow control: continue, no block limit, STmin 20 ms (as the EasyStart).
        const std::vector<uint8_t> fc{0x30, 0x00, 0x14, ISOTP_PAD, ISOTP_PAD, ISOTP_PAD, ISOTP_PAD, ISOTP_PAD};
        this->canbus_->send_data(ID_DIAG_REQUEST, false, fc);
      }
      break;
    case 0x2:  // consecutive frame
      if (this->isotp_len_ == 0)
        return;
      this->isotp_buf_.insert(this->isotp_buf_.end(), data.begin() + 1, data.end());
      if (this->isotp_buf_.size() >= this->isotp_len_) {
        this->isotp_buf_.resize(this->isotp_len_);
        this->isotp_len_ = 0;
        this->diag_requested_ = false;
        this->uds_(this->isotp_buf_);
      }
      break;
    default:
      break;
  }
}

// UDS responses of interest. 59 02 <mask> {DTC hi mid lo, status}*: ReadDTCInformation
// by status mask, as requested by the EasyStart's error-memory readout.
void HydronicS3::uds_(const std::vector<uint8_t> &payload) {
  if ((payload.size() >= 3 && payload[0] == 0x62) || (payload.size() >= 2 && payload[0] == 0x61)) {
    // Positive response: 62 <DID hi lo> data (22) or 61 <local id> data (21).
    const size_t data_at = payload[0] == 0x62 ? 3 : 2;
    std::string text;
    char buf[8];
    if (payload[0] == 0x62)
      snprintf(buf, sizeof(buf), "%02X%02X:", payload[1], payload[2]);
    else
      snprintf(buf, sizeof(buf), "L%02X:", payload[1]);
    text = buf;
    std::string ascii;
    for (size_t i = data_at; i < payload.size(); i++) {
      snprintf(buf, sizeof(buf), " %02X", payload[i]);
      text += buf;
      ascii += payload[i] >= 0x20 && payload[i] < 0x7F ? static_cast<char>(payload[i]) : '.';
    }
    text += " '" + ascii + "'";
    if (payload[0] == 0x62)
      this->decode_did_(payload[1] << 8 | payload[2], payload.data() + 3, payload.size() - 3);
    this->scan_positive_++;
    this->scan_answered_();
    this->publish_diag_(text);
    return;
  }
  if (payload.size() >= 3 && payload[0] == 0x7F && this->single_until_ms_ != 0) {
    char buf[40];
    snprintf(buf, sizeof(buf), "negative: service %02X NRC %02X", payload[1], payload[2]);
    if (payload[2] != NRC_RESPONSE_PENDING)
      this->publish_diag_(buf);
    return;
  }
  if (payload.size() >= 3 && payload[0] == 0x7F) {  // negative response
    if (payload[2] == NRC_RESPONSE_PENDING) {
      this->scan_sent_ms_ = millis();
      this->scan_timeout_ms_ = DIAG_PENDING_TIMEOUT_MS;
      return;
    }
    if (payload[1] == this->scan_service_) {
      this->scan_negative_++;
      this->nrc_count_[payload[2]]++;
    }
    this->scan_answered_();
    return;
  }
  if (payload.size() < 3 || payload[0] != 0x59 || payload[1] != 0x02) {
    if (this->single_until_ms_ != 0 && !payload.empty() && (payload[0] & 0x40)) {  // other positive response
      std::string text = "raw:";
      char buf[4];
      for (uint8_t b : payload) {
        snprintf(buf, sizeof(buf), " %02X", b);
        text += buf;
      }
      this->publish_diag_(text);
    }
    return;
  }
  std::string text;
  for (size_t i = 3; i + 4 <= payload.size(); i += 4) {
    char dtc[20];
    snprintf(dtc, sizeof(dtc), "%sP%02X%02X%02X (%02X)", text.empty() ? "" : ", ", payload[i], payload[i + 1],
             payload[i + 2], payload[i + 3]);
    text += dtc;
  }
  if (text.empty())
    text = "none";
  char head[48];
  snprintf(head, sizeof(head), "mask %02X: ", payload[2]);
  text = head + text;
  ESP_LOGI(TAG, "fault codes %s", text.c_str());
#ifdef USE_TEXT_SENSOR
  if (this->fault_codes_text_ != nullptr)
    this->fault_codes_text_->publish_state(text);
#endif
}

void HydronicS3::decode_did_(uint16_t did, const uint8_t *d, size_t n) {
  const auto ident = std::find(this->ident_dids_.begin(), this->ident_dids_.end(), did);
  if (ident != this->ident_dids_.end()) {
    this->ident_dids_.erase(ident);
#ifdef USE_TEXT_SENSOR
    std::string text;
    IdentText which = IDENT_COUNT;
    if (did == 0xF1F0) {
      char buf[20];
      if (n >= 6)
        snprintf(buf, sizeof(buf), "%02X.%02X%02X.%02X.%02X%02X", d[0], d[1], d[2], d[3], d[4], d[5]);
      text = n >= 6 ? buf : "";
      which = IDENT_PART_NUMBER;
    } else {
      for (size_t i = 0; i < n; i++)
        text += d[i] >= 0x20 && d[i] < 0x7F ? static_cast<char>(d[i]) : '.';
      which = did == 0xF193 ? IDENT_HARDWARE_VERSION : did == 0xF195 ? IDENT_SOFTWARE_VERSION : IDENT_SERIAL;
    }
    if (this->ident_texts_[which] != nullptr)
      this->ident_texts_[which]->publish_state(text);
#endif
    return;
  }
  auto u16 = [d](size_t i) { return static_cast<uint16_t>(d[i] << 8 | d[i + 1]); };
#ifdef USE_SENSOR
  auto pub = [this](DidSensor which, float value) {
    this->did_ms_[which] = millis();
    if (this->did_sensors_[which] != nullptr)
      this->did_sensors_[which]->publish_state(value);
  };
  switch (did) {
    case 0xFD01:
      if (n >= 4) {
        pub(DID_FLAME_SENSOR_TEMP, u16(0) / 100.0f - 50.0f);
        pub(DID_INLET_TEMPERATURE, u16(2) / 100.0f - 50.0f);
      }
      break;
    case 0xFD04:
      if (n >= 2)
        pub(DID_SUPPLY_VOLTAGE, u16(0) / 10.0f);
      break;
    case 0xFD08:
      if (n >= 2)
        pub(DID_FD08_RAW, u16(0));
      break;
    case 0xFD35:
      if (n >= 2)
        pub(DID_FD35_RAW, u16(0));
      break;
    case 0xFD09:
      if (n >= 2)
        pub(DID_BLOWER_SPEED, u16(0));
      break;
    case 0xFD0A:
      if (n >= 2)
        pub(DID_GLOW_PLUG_POWER, u16(0) / 10.0f);
      break;
    case 0xFD0C:
      if (n >= 1)
        pub(DID_GLOW_PLUG_RAW, d[0]);
      break;
    case 0xFD0D:
      if (n >= 1)
        pub(DID_METERING_PUMP_RATE, d[0] / 10.0f);
      break;
    case 0xFD10:
      if (n >= 2)
        pub(DID_OPERATING_PHASE, d[1]);
      break;
    case 0xFD11:
      if (n >= 2) {
        const float l_h = d[1] * 3600.0f / 1e6f;
        pub(DID_FUEL_CONSUMPTION, l_h);
        pub(DID_HEATING_POWER, l_h * HEAT_FLOW_W_PER_L_H);
      }
      if (n >= 10) {
        const uint32_t litres = static_cast<uint32_t>(u16(2)) << 16 | u16(4);
        const uint32_t nl = static_cast<uint32_t>(u16(6)) << 16 | u16(8);
        pub(DID_FUEL_TOTAL, litres + nl / 1e9f);
      }
      break;
    case 0xFD17:
      for (size_t i = 0; i < 7 && i * 4 + 4 <= n; i++) {
        const uint32_t minutes = static_cast<uint32_t>(u16(i * 4)) << 16 | u16(i * 4 + 2);
        pub(static_cast<DidSensor>(DID_HOURS_TOTAL + i), minutes / 60.0f);
      }
      break;
    default:
      break;
  }
#endif
#ifdef USE_BINARY_SENSOR
  if (did == 0xFD0F && n >= 1 && this->flame_ != nullptr) {
    this->flame_ms_ = millis();
    this->flame_->publish_state(d[0] & 0x01);
  }
#endif
}

void HydronicS3::dump_ids_() {
  // Nothing new (bus idle or disconnected): one line, not the full table every time.
  if (this->frames_ == this->dumped_frames_)
    return;
  this->dumped_frames_ = this->frames_;
  if (this->ids_.empty()) {
    ESP_LOGW(TAG, "no frames received");
    return;
  }
  const uint32_t now = millis();
  char buf[25], mask[9];
  ESP_LOGI(TAG, "%u IDs, %" PRIu32 " frames", (unsigned) this->ids_.size(), this->frames_);
  for (auto &[key, s] : this->ids_) {
    hex(buf, s.data, s.len);
    for (uint8_t b = 0; b < 8; b++)
      mask[b] = b >= s.len ? ' ' : (s.changed >> b & 1) ? '^' : '.';
    mask[8] = 0;
    ESP_LOGI(TAG, "  %0*" PRIX32 " n=%-6" PRIu32 " T~%6.0fms age=%5" PRIu32 "ms  %-24s %s", s.ext ? 8 : 3,
             key & ~EXT_FLAG, s.count, s.period_ms, now - s.last_ms, buf, mask);
    s.changed = 0;
  }
}

}  // namespace esphome::hydronic_s3
