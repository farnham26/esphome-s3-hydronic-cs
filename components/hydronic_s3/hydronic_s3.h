#pragma once

#include <cmath>
#include <functional>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "esphome/components/canbus/canbus.h"
#include "esphome/core/automation.h"
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
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#include "esphome/core/preferences.h"
#ifdef USE_HYDRONIC_S3_OTA_KEEPALIVE
#include "esphome/components/ota/ota_backend.h"
#endif
#include <atomic>

namespace esphome::hydronic_s3 {

class RunSwitch;
class ModeSelect;

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

/// Values read by UDS ReadDataByIdentifier (see docs/protocol.md "Live and stored data").
/// (?) = plausible, not confirmed.
enum DidSensor : uint8_t {
  DID_INLET_TEMPERATURE,      // FD01 w2, v/100 - 50 °C
  DID_FLAME_SENSOR_TEMP,      // FD01 w1
  DID_SUPPLY_VOLTAGE,         // FD04 0.1 V (26.0 vs. 26.14 V measured)
  DID_BLOWER_SPEED,           // FD09 rpm (~2000 at min, 6154 at full load)
  DID_GLOW_PLUG_POWER,        // FD0A 0.1 W (?)
  DID_GLOW_PLUG_RAW,          // FD0C, non-zero while glowing (start, run-down afterglow)
  DID_METERING_PUMP_RATE,     // FD0D 0.1 Hz (?)
  DID_OPERATING_PHASE,        // FD10 B1: 00 off, 0A cold start, 0C warm start, 0F pre-start check, C8 hold, 14 burn, 1E run-down (off), 1F run-down (pause), 28 pause
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
  DID_FD08_RAW,               // u16: blower command? 20000 at 0 rpm, ~20000 + 1.55 x rpm (not the water pump)
  DID_FD35_RAW,               // u16: temperature in FD01 format, see DID_CONTROL_UNIT_TEMP
  DID_START_STEP,             // FD10 B2: start sub-step 01..12 (B3-4 timer 10 ms): 01-05 glow, 06/08 fuel, 0B flame
  DID_COMBUSTION_STATE,       // FD0F B1: 1 pre-glow, 2 fuel on, 3 flame
  DID_CONTROL_UNIT_TEMP,      // FD35 v/100 - 50 °C (?): +6 K within 5 s of glow plug on, 63-67 °C at full load
  DID_WATER_FLOW,             // estimate: heating power / (coolant heat capacity x (outlet - inlet)), burning only
  DID_WATER_PUMP_CURRENT,     // FEFF B21 x 10 mA (unit likely: 0.78 A vs 0.70 +- 0.1 A measured, 10-11)
  DID_FUEL_PUMP_CURRENT,      // FEFF B19 x 10 mA (as B21; ~0.36 A while the metering pump runs)
  DID_WATER_PUMP_POWER,       // water pump current x supply voltage (FD04): power at the heater's pump output
  DID_SENSOR_COUNT,
};

/// Identification read once (retried each poll sweep until answered).
enum IdentText : uint8_t {
  IDENT_HARDWARE_VERSION,  // F193 ASCII
  IDENT_SOFTWARE_VERSION,  // F195 ASCII
  IDENT_PART_NUMBER,       // F1F0 BCD (25 27 45 01 50 01 = 25.2745.01.5001)
  IDENT_SERIAL,            // F1F5 ASCII
  IDENT_COUNT,
};

/// D6L datasheet: 5600 W at 0.65 l/h, 1300 W at 0.15 l/h (linear within 1 %).
static constexpr float HEAT_FLOW_W_PER_L_H = 5600.0f / 0.65f;
/// Coolant Alde Premixed G13: ready mixed ~50 % glycol (ethylene glycol, 20 % of it replaced by glycerin),
/// frost -37 °C. No published cp; 50 % EG tables at 60-70 °C: 3.47 kJ/(kg K) x 1.05 kg/l. J/(l K), ±5 %.
static constexpr float COOLANT_J_PER_L_K = 3650.0f;
/// Water flow is estimated while burning from outlet - inlet >= FLOW_ON_DT_K until it
/// drops below FLOW_OFF_DT_K (hysteresis; at 2 K the sensor offset alone is ±15-20 %).
static constexpr float FLOW_ON_DT_K = 2.5f;
static constexpr float FLOW_OFF_DT_K = 2.0f;

/// Eberspächer Hydronic S3 on CAN (EasyStart protocol, 11-bit, 500 kbit/s).
/// Listen-only: decodes what is known, and logs new IDs and changing bytes so
/// the rest can be decoded from logs.
class HydronicS3 : public Component
#ifdef USE_HYDRONIC_S3_OTA_KEEPALIVE
    ,
                   public ota::OTAGlobalStateListener
#endif
{
 public:
  void set_canbus(canbus::Canbus *bus) { this->canbus_ = bus; }
  /// Research aids: per-ID table, change log, wake trace (off: decode only).
  void set_research(bool research) { this->research_ = research; }
  /// Observer: diagnosis only (decode, UDS tester), never a control unit — no command set,
  /// wake answer, config burst or clock; another control unit (e.g. tempcontrol) owns the
  /// heater. Not switched on while we command (the heater would lose its control unit).
  void set_observer(bool observer);
  void set_ota_keepalive(bool on) { this->ota_keepalive_ = on; }
#ifdef USE_HYDRONIC_S3_OTA_KEEPALIVE
  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) override;
#endif
  bool observer() const { return this->observer_; }
#ifdef USE_TIME
  /// Clock for the heater (10A, as the EasyStart's time setting): sent with the config
  /// burst and on each sync while we command. Without it, 10A carries "time not set".
  void set_time(time::RealTimeClock *clock) { this->clock_ = clock; }
#endif
#ifdef USE_SENSOR
  void set_outlet_temperature_sensor(sensor::Sensor *s) { this->outlet_temperature_ = s; }
  void set_state_code_sensor(sensor::Sensor *s) { this->state_code_ = s; }
  void set_did_sensor(DidSensor which, sensor::Sensor *s) { this->did_sensors_[which] = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_state_text_sensor(text_sensor::TextSensor *s) { this->state_text_ = s; }
  void set_fault_text_sensor(text_sensor::TextSensor *s) { this->fault_text_ = s; }
  void set_event_text_sensor(text_sensor::TextSensor *s) { this->event_text_ = s; }
  void set_fault_codes_text_sensor(text_sensor::TextSensor *s) { this->fault_codes_text_ = s; }
  void set_diag_response_text_sensor(text_sensor::TextSensor *s) { this->diag_response_text_ = s; }
  void set_ident_text_sensor(IdentText which, text_sensor::TextSensor *s) { this->ident_texts_[which] = s; }
  void set_phase_text_sensor(text_sensor::TextSensor *s) { this->phase_text_ = s; }
  void set_combustion_text_sensor(text_sensor::TextSensor *s) { this->combustion_text_ = s; }
  void set_request_source_text_sensor(text_sensor::TextSensor *s) { this->request_source_text_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_on_request_binary_sensor(binary_sensor::BinarySensor *s) { this->on_request_ = s; }
  void set_flame_binary_sensor(binary_sensor::BinarySensor *s) { this->flame_ = s; }
  void set_connected_binary_sensor(binary_sensor::BinarySensor *s) { this->connected_ = s; }
  void set_vehicle_fan_binary_sensor(binary_sensor::BinarySensor *s) { this->vehicle_fan_ = s; }
  void set_request_binary_sensor(binary_sensor::BinarySensor *s) { this->request_ = s; }
  void set_busy_binary_sensor(binary_sensor::BinarySensor *s) { this->busy_ = s; }
#endif

  /// Heater status frames received within the stale timeout.
  bool connected() const { return this->status_ms_ != 0; }
  /// Awake and not idle (running, run-down): it expects its controller to keep talking.
  bool heater_busy() const {
    return this->connected() && (this->rundown_ || (this->state_ != 0x00 && this->state_ != 0x03));
  }
  /// Last decoded values; NAN / -1 until received and after the stale timeout.
  float outlet_temperature() const { return this->outlet_temp_c_; }
  int state() const { return this->state_; }
  int on_request() const { return this->on_request_raw_; }
  /// Who the heater's switch-on request (FD10 B0) comes from; inferred, see update_request_().
  enum RequestSource : uint8_t { REQUEST_NONE, REQUEST_CAN, REQUEST_S_PLUS, REQUEST_EASYSTART };
  RequestSource request_source() const { return this->request_source_; }
  static const char *request_source_name(RequestSource source);
  /// The heater runs on a request that is not ours (S+ or another control unit).
  bool external_request() const {
    return this->request_source_ == REQUEST_S_PLUS || this->request_source_ == REQUEST_EASYSTART;
  }
  /// 0x2C4 B2 while in fault or its ~26 s aftermath, 0 otherwise; -1 until received.
  int fault_code() const { return this->fault_code_; }
  /// Stored fault codes of the last 19 02 read ("P000250, ..."), "none", or empty until read.
  /// Read automatically when the 0x2C4 flags rise (start hold, fault).
  const std::string &fault_codes() const { return this->fault_codes_; }
  static const char *state_name(int state);
  /// FD10 B1 operating phase; nullptr if unknown.
  static const char *phase_name(int phase);
  /// FD10 B1 operating phase, descriptive; nullptr if unknown.
  static const char *phase_long_name(int phase);
  /// FD0F B1 combustion state; nullptr if unknown.
  static const char *combustion_name(int state);
  /// Sends UDS ReadDTCInformation (19 02 <mask>) as the EasyStart's error-memory
  /// readout does. The bus controller must be out of listen-only mode.
  void request_fault_codes(uint8_t status_mask);
  /// Session + SecurityAccess unlock, then ClearDiagnosticInformation (14 FF FF FF).
  /// The key comes from set_security_provider(); aborts (logged) on any negative
  /// response, timeout, or no/refusing provider.
  void clear_dtc();
  /// Reads DIDs first..last one by one with UDS ReadDataByIdentifier (22); positive
  /// answers go to the diag_response text sensor. The bus controller is taken out of
  /// listen-only mode for the scan through the listen-only hook.
  void scan_dids(uint16_t first, uint16_t last);
  /// Same for KWP2000-style ReadDataByLocalIdentifier (21 <id>), ids first..last.
  void scan_local_ids(uint8_t first, uint8_t last);
  /// Same for RoutineControl requestRoutineResults (31 03 <id>), ids first..last: reports only,
  /// never starts a routine. NRC 31 = no such routine; other NRCs are logged per id.
  void scan_routines(uint16_t first, uint16_t last);
  /// Sends one UDS request given as hex ("19 04 00 02 52 FF"); the answer goes to the
  /// diag_response text sensor. Normal mode for 3 s via the listen-only hook.
  void send_uds_request(const std::string &hex);
  /// Reads the given DIDs in a sweep every interval_ms until stop_polling(); answers
  /// go to the diag_response text sensor. Normal mode while polling.
  void start_polling(std::vector<uint16_t> dids, uint32_t interval_ms);
  void stop_polling();
  /// Start/stop: sends the EasyStart's command set (054-057 every 200 ms, 60D every
  /// 100 ms, as recorded during an EasyStart run) with "on" (true) or "off"
  /// (false, continued while the heater runs down). Backs off for good as soon as an
  /// EasyStart frame appears: never two controllers on the bus. Normal mode meanwhile.
  /// "On" is refused while the heater runs on S+ (request_source() == REQUEST_S_PLUS).
  void set_command(bool on);
  /// Watchdog: "on" stops (with "off") unless set_command(true) renews it within this
  /// time. 0: no watchdog, run until set_command(false).
  void set_command_lease(uint32_t ms) {
    this->command_lease_ms_ = ms;
    this->command_deadline_ms_ = millis() + ms;  // a change renews a running lease
  }
  /// Research: stops sending at once, without "off" (controller loss).
  void abort_command();
  /// Start requested and not backed off.
  bool command_on() const { return this->command_on_; }
  /// Burner starting or heating (FD10 phase, polled); false while unknown.
  bool burner_on() const;
  uint8_t command_mode() const { return this->command_mode_; }
  /// Why a run we commanded ended (run_ended event, on_run_ended reason).
  enum RunEnd : uint8_t {
    RUN_END_COOLANT_LIMIT = 1,  // burn ended at the coolant limit, run still on
    RUN_END_PAUSE_FAULT,        // same, with fault flags set
    RUN_END_START_REFUSED,      // start hold after all retries
    RUN_END_HEATER_STOPPED,     // phase C8 / 1E / DE / 00 without our stop
    RUN_END_RESIDUAL,           // residual heat ended (2C4 C1 after 81)
    RUN_END_OTHER_CONTROL_UNIT,
    RUN_END_WATCHDOG,
    RUN_END_RESIDUAL_REFUSED,   // residual heat refused: C1 / phase DE without 81 (coolant too cold)
  };
  static const char *run_end_name(int why);
  /// Sweep interval of sensor_dids(); polling starts at boot unless a polling switch decides.
  void set_poll_interval(uint32_t ms) { this->sensor_poll_ms_ = ms; }
  uint32_t poll_interval() const { return this->sensor_poll_ms_; }
#ifdef USE_SWITCH
  void set_run_switch(RunSwitch *s) { this->run_switch_ = s; }
  void set_polling_switch(switch_::Switch *s) { this->polling_switch_ = s; }
#endif
#ifdef USE_SELECT
  void set_mode_select(ModeSelect *s) { this->mode_select_ = s; }
#endif
  /// 054 B1 while "on", as the EasyStart's three modes: MODE_HEAT (burner), MODE_FAN
  /// (vehicle fan only), MODE_RESIDUAL (water pump + vehicle fan, no burner).
  void set_command_mode(uint8_t mode) {
    if (mode != this->command_mode_)
      this->residual_ran_ = false;
    this->command_mode_ = mode;
  }
  /// The heater runs this mode (2C4 state 05 heat incl. start/pause, 21 fan, 81 residual), not our command.
  bool mode_active(uint8_t mode) const {
    return this->state_ == (mode == MODE_HEAT ? 0x05 : mode == MODE_FAN ? 0x21 : mode == MODE_RESIDUAL ? 0x81 : -1);
  }
  static constexpr uint8_t MODE_HEAT = 0x01;
  static constexpr uint8_t MODE_FAN = 0x02;
  static constexpr uint8_t MODE_RESIDUAL = 0x03;
  /// The DIDs behind the DidSensor values and the flame flag.
  static std::vector<uint16_t> sensor_dids() {
    return {0xFD01, 0xFD04, 0xFD08, 0xFD09, 0xFD0A, 0xFD0C, 0xFD0D, 0xFD0F, 0xFD10, 0xFD11, 0xFD17, 0xFD35, 0xFEFF};
  }
  /// Switches the bus controller between listen-only (true) and normal mode.
  void set_listen_only_hook(std::function<bool(bool)> hook) { this->listen_only_hook_ = std::move(hook); }
  /// Supplies the SecurityAccess key for a given seed, for clear_dtc() only. Returns
  /// false to refuse (unknown seed, no key available). The heater-specific unlock
  /// material is not in this repo: register this from a local config (e.g. an on_boot
  /// lambda, see README "Controlling the heater"); without one,
  /// clear_dtc() fails with a clear message and nothing else is affected.
  using SecurityProvider = std::function<bool(const uint8_t seed[4], uint8_t key_out[4])>;
  void set_security_provider(SecurityProvider provider) { this->security_provider_ = std::move(provider); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /// Key: CAN ID, bit 31 set for extended IDs.
  const std::map<uint32_t, IdState> &ids() const { return this->ids_; }
  uint32_t frames() const { return this->frames_; }
  /// Every logged transition (what, from, to), plus named ones: burner_starting, flame_stable,
  /// control_pause, idle (FD10 phase, polled), residual_end (2C4 state C1), run_ended (from: RunEnd),
  /// new_fault_code (to: code). Called from loop().
  template<typename F> void add_on_event_callback(F &&callback) { this->event_callback_.add(std::forward<F>(callback)); }

 protected:
  void on_frame_(uint32_t can_id, bool ext, bool rtr, const std::vector<uint8_t> &data);
  void track_id_(uint32_t can_id, bool ext, bool rtr, const std::vector<uint8_t> &data, uint32_t now);
  static void hex(char *out, const uint8_t *d, uint8_t len);  // "AA BB ..", 3 chars per byte
  bool research_{false};
  bool observer_{false};
  bool ota_keepalive_{false};
  // OTA keep-alive: frames from a task while the main loop is blocked by the upload.
  void *keepalive_task_{nullptr};
  std::atomic<bool> keepalive_stop_{false};
  uint8_t keepalive_command_[2]{};  // 054 B0/B1 at the OTA start, held
  static void keepalive_task_fn_(void *arg);
  void dump_ids_();
  void decode_(uint32_t can_id, const std::vector<uint8_t> &data);
  void event_(const char *what, int from, int to, bool log = true);
  void phase_events_(int from, int to);
  struct PendingEvent {
    const char *what;  // string literal
    int from, to;
  };
  // Dispatched from loop(): decode_() raises events before its state is updated, and a
  // callback may command the heater.
  std::vector<PendingEvent> pending_events_;
  CallbackManager<void(std::string, int, int)> event_callback_;
  void diag_response_(const std::vector<uint8_t> &data);
  void uds_(const std::vector<uint8_t> &payload);
  void clear_send_(const std::vector<uint8_t> &payload);
  void clear_abort_(const char *why);
  void clear_step_response_(const std::vector<uint8_t> &payload);
  uint8_t clear_step_{0};  // CLEAR_IDLE..CLEAR_WAIT_CLEAR, see hydronic_s3.cpp
  uint32_t clear_deadline_ms_{0};
  SecurityProvider security_provider_;
  void decode_did_(uint16_t did, const uint8_t *d, size_t n);
  void expire_stale_();
  void send_failed_();
  void update_mode_();
  void publish_state_text_();  // 2C4 state name, "Run-down" for 03 with the run-down bit
  bool set_listen_only_(bool listen_only);
  int8_t listen_only_{-1};  // last mode set through the hook, -1 unknown

  canbus::Canbus *canbus_{nullptr};
  std::map<uint32_t, IdState> ids_;
  uint32_t frames_{0};
  uint32_t dumped_frames_{UINT32_MAX};  // frames_ at the last ID dump
  uint32_t last_frame_ms_{0};
  uint32_t wake_ms_{0};      // first frame after bus silence
  uint8_t wake_trace_{0};    // frames of the wake-up still to log

  float outlet_temp_c_{NAN};
  int state_{-1};
  int on_request_raw_{-1};
  RequestSource request_source_{REQUEST_NONE};
  int request_raw_{-1};               // FD10 B0; -1: unknown
  uint32_t command_change_ms_{0};     // last change of command_on_
  void update_request_(int request);
  int command_raw_{-1};       // 0x054 B0 << 8 | B1
  int switch_input_raw_{-1};  // 0x625 B1
  std::vector<uint8_t> isotp_buf_;  // diagnostic response being reassembled
  size_t isotp_len_{0};
  bool diag_requested_{false};  // we are the tester: answer first frames with flow control
  /// Another tester's request on 0x7A0 awaits its answer until then (0: none): the heater
  /// drops a request that overlaps a pending one, so ours wait.
  uint32_t foreign_diag_until_ms_{0};
  uint32_t diag_quiet_until_ms_{0};  // observer: the control unit's next request goes first
  uint16_t diag_expect_did_{0};      // DID of our pending 22 request (0: other service)
  bool diag_free_() const;
  void answered_();
  uint16_t scan_expect_{0};  // id of our pending scan request: another tester's answers are not ours
  void publish_diag_(const std::string &text);
  std::function<bool(bool)> listen_only_hook_;
  bool scan_active_{false};
  bool sweep_{false};  // the active scan is a poll sweep over sweep_dids_, not a range scan
  std::function<void()> after_sweep_;  // a request deferred until the sweep's pending answer is in
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
  std::vector<uint16_t> ident_dids_{0xF193, 0xF195, 0xF1F0, 0xF1F5};  // not yet answered
  std::vector<uint16_t> sweep_dids_;  // current sweep: poll_dids_ + ident_dids_
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
  std::string fault_codes_;
  std::vector<uint32_t> dtc_known_;  // stored codes (3 bytes) since boot, for new_fault_code
  bool dtc_baseline_{false};         // dtc_known_ read once
  uint8_t dtc_mask_{0};              // status mask of the pending 19 02
  int phase_{-1};  // FD10 B1, last seen
  float did_inlet_c_{NAN}, did_outlet_c_{NAN};  // FD01, same sweep as FD11 (flow estimate)
  float did_voltage_v_{NAN};                     // FD04 (pump power)
  bool flow_valid_{false};  // flow estimate hysteresis state
  /// Last update per value source, 0 = none since the last expiry (see expire_stale_()).
  uint32_t status_ms_{0}, command_ms_{0}, heartbeat_ms_{0}, flame_ms_{0}, fan_ms_{0};
  uint32_t did_ms_[DID_SENSOR_COUNT]{};
  uint32_t counters_ms_{0};  // last sweep with the hour counters (0: due)
  uint32_t sensor_poll_ms_{5000};
#ifdef USE_SWITCH
  RunSwitch *run_switch_{nullptr};
  switch_::Switch *polling_switch_{nullptr};
#endif
#ifdef USE_SELECT
  ModeSelect *mode_select_{nullptr};
#endif
  bool command_on_{false};
  bool stop_deferred_{false};  // stop requested during a start: sent once the start phase ends
  uint32_t handover_until_ms_{0};  // run adopted after an OTA reboot: stops wait for the phase
  void deferred_stop_();
  bool run_end_final_{false};  // run_ended for this run already raised (final kinds)
  bool residual_ran_{false};    // state 81 seen since the run (mode) started: a C1 then is an end, else a refusal
  void run_ended_(RunEnd why, int detail, bool stop);
  uint8_t command_mode_{MODE_HEAT};
  uint32_t command_until_ms_{0};  // off set sent until then; 0: not commanding
  uint32_t command_next_ms_{0};
  uint32_t command_lease_ms_{0};
  uint32_t command_deadline_ms_{0};
  uint8_t start_retries_{0};      // start holds retried for the current "on"
  uint32_t retry_until_ms_{0};    // "off" until then (start hold retry); 0: none
  uint32_t keepalive_next_ms_{0};
  uint8_t keepalive_data_[8]{0x0D, 0x10, 0x75, 0x7A, 0x02, 0x20, 0x70, 0x00};  // last 60D seen
  uint8_t controller_data_[8]{0x6C, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};  // last 065 seen
  uint32_t controller_next_ms_{0};
  bool burst_sent_{false};      // EasyStart config burst replayed since boot
  uint32_t burst_due_ms_{0};    // 0: not scheduled
  uint8_t burst_round_{0};
  void burst_loop_();
  void schedule_burst_();
  bool clock_frame_(uint8_t out[6]);  // 10A from the clock; false: no valid time
  uint8_t clock_counter_{0};          // 10A B2 bits 6-7, 1..3 as the EasyStart
#ifdef USE_TIME
  time::RealTimeClock *clock_{nullptr};
#endif
  /// Controller frames: the MCP2515 has 3 TX buffers, a full one is retried on the
  /// next loop instead of dropping the frame.
  struct TxFrame {
    uint16_t id;
    uint8_t len;
    uint8_t data[8];
  };
  std::deque<TxFrame> tx_queue_;
  void queue_(uint16_t id, const uint8_t *data, uint8_t len);
  void drain_tx_();
  void send_diag_(const std::vector<uint8_t> &frame);  // queued behind controller frames
  bool commanding_() const { return this->command_on_ || this->command_until_ms_ != 0; }
  uint8_t idle_sweeps_{0};  // sweeps left while the heater is idle
  bool rundown_{false};     // 2C4 B1 bit 3: burner run-down, also under state 03
  /// Polling keeps the heater awake: poll continuously only while it is active, and a
  /// few sweeps after each state change, so an idle heater can go to sleep.
  bool poll_wanted_() const {
    return this->polling_ && this->connected() && (this->idle_sweeps_ != 0 || this->rundown_ ||
                                                    (this->state_ != 0x00 && this->state_ != 0x03));
  }
  void command_loop_();
  uint32_t easystart_ms_{0};  // last EasyStart frame, 0: none
  /// Without the EasyStart nobody else acks the heater's frames (it would retry its
  /// first frame forever): listen-only only while an EasyStart was seen recently.
  bool easystart_present_() const;
  bool tx_down_{false};  // a send failed: no polling until a frame proves the bus is back
#ifdef USE_SENSOR
  sensor::Sensor *outlet_temperature_{nullptr};
  sensor::Sensor *state_code_{nullptr};
  sensor::Sensor *did_sensors_[DID_SENSOR_COUNT]{};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *state_text_{nullptr};
  text_sensor::TextSensor *fault_text_{nullptr};
  text_sensor::TextSensor *event_text_{nullptr};
  text_sensor::TextSensor *fault_codes_text_{nullptr};
  text_sensor::TextSensor *diag_response_text_{nullptr};
  text_sensor::TextSensor *ident_texts_[IDENT_COUNT]{};
  text_sensor::TextSensor *phase_text_{nullptr};
  text_sensor::TextSensor *combustion_text_{nullptr};
  text_sensor::TextSensor *request_source_text_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *on_request_{nullptr};
  binary_sensor::BinarySensor *flame_{nullptr};
  binary_sensor::BinarySensor *connected_{nullptr};
  binary_sensor::BinarySensor *vehicle_fan_{nullptr};
  binary_sensor::BinarySensor *busy_{nullptr};
  binary_sensor::BinarySensor *request_{nullptr};
#endif
};

class EventTrigger : public Trigger<std::string, int, int> {
 public:
  explicit EventTrigger(HydronicS3 *parent) {
    parent->add_on_event_callback(
        [this](const std::string &what, int from, int to) { this->trigger(what, from, to); });
  }
};

/// on_run_ended: a run we commanded ended other than by our stop (reason: run_end_name(), log only).
class RunEndedTrigger : public Trigger<std::string> {
 public:
  explicit RunEndedTrigger(HydronicS3 *parent) {
    parent->add_on_event_callback([this](const std::string &w, int why, int detail) {
      if (w != "run_ended")
        return;
      char text[64];
      snprintf(text, sizeof(text), "%s (0x%02X)", HydronicS3::run_end_name(why), detail);
      this->trigger(text);
    });
  }
};

/// on_new_fault_code: a stored fault code that was not in the memory before (code: "P000250").
class NewFaultCodeTrigger : public Trigger<std::string> {
 public:
  explicit NewFaultCodeTrigger(HydronicS3 *parent) {
    parent->add_on_event_callback([this](const std::string &w, int, int code) {
      if (w != "new_fault_code")
        return;
      char text[8];
      snprintf(text, sizeof(text), "P%06X", code);
      this->trigger(text);
    });
  }
};

/// hydronic_s3.command: mode and lease (if given) first, then set_command(run).
template<typename... Ts> class CommandAction : public Action<Ts...> {
 public:
  explicit CommandAction(HydronicS3 *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(bool, run)
  TEMPLATABLE_VALUE(uint8_t, mode)
  TEMPLATABLE_VALUE(uint32_t, lease)

  void play(const Ts &...x) override {
    if (this->mode_.has_value())
      this->parent_->set_command_mode(this->mode_.value(x...));
    if (this->lease_.has_value())
      this->parent_->set_command_lease(this->lease_.value(x...));
    this->parent_->set_command(this->run_.value(x...));
  }

 protected:
  HydronicS3 *parent_;
};

/// hydronic_s3.busy: heater_busy().
template<typename... Ts> class BusyCondition : public Condition<Ts...> {
 public:
  explicit BusyCondition(HydronicS3 *parent) : parent_(parent) {}
  bool check(const Ts &...x) override { return this->parent_->heater_busy(); }

 protected:
  HydronicS3 *parent_;
};

/// hydronic_s3.mode_active: mode_active(mode).
template<typename... Ts> class ModeActiveCondition : public Condition<Ts...> {
 public:
  explicit ModeActiveCondition(HydronicS3 *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint8_t, mode)
  bool check(const Ts &...x) override { return this->parent_->mode_active(this->mode_.value(x...)); }

 protected:
  HydronicS3 *parent_;
};

/// hydronic_s3.burner_on: burner_on().
template<typename... Ts> class BurnerOnCondition : public Condition<Ts...> {
 public:
  explicit BurnerOnCondition(HydronicS3 *parent) : parent_(parent) {}
  bool check(const Ts &...x) override { return this->parent_->burner_on(); }

 protected:
  HydronicS3 *parent_;
};

#ifdef USE_SWITCH
/// "Heater Run": set_command(); reads back command_on() (refusal, back-off, start refused, watchdog).
class RunSwitch : public switch_::Switch, public Component, public Parented<HydronicS3> {
 public:
  void setup() override;
  // After the parent: its setup adopts a run held over an OTA reboot, which a restored "off" must see.
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

 protected:
  void write_state(bool state) override;
};

/// Polling of sensor_dids() at poll_interval.
class PollingSwitch : public switch_::Switch, public Component, public Parented<HydronicS3> {
 public:
  void setup() override;

 protected:
  void write_state(bool state) override;
};
#endif

#ifdef USE_SELECT
/// What "Heater Run" starts: Heat / Residual heat / Fan, restored. Shows "S+" (read-only) while S+ runs.
class ModeSelect : public select::Select, public Component, public Parented<HydronicS3> {
 public:
  void setup() override;
  void sync();  // from HydronicS3::loop()

 protected:
  void control(size_t index) override;
  ESPPreferenceObject pref_;
};
#endif

#ifdef USE_NUMBER
/// Run watchdog in minutes (set_command_lease), restored; 0 = run until turned off.
class RunWatchdogNumber : public number::Number, public Component, public Parented<HydronicS3> {
 public:
  void setup() override;

 protected:
  void control(float value) override;
  ESPPreferenceObject pref_;
};
#endif

#ifdef USE_BUTTON
class ReadFaultCodesButton : public button::Button, public Parented<HydronicS3> {
 protected:
  void press_action() override { this->parent_->request_fault_codes(0x08); }
};
class ClearFaultCodesButton : public button::Button, public Parented<HydronicS3> {
 protected:
  void press_action() override { this->parent_->clear_dtc(); }
};
#endif

/// hydronic_s3.commanding: command_on().
template<typename... Ts> class CommandingCondition : public Condition<Ts...> {
 public:
  explicit CommandingCondition(HydronicS3 *parent) : parent_(parent) {}
  bool check(const Ts &...x) override { return this->parent_->command_on(); }

 protected:
  HydronicS3 *parent_;
};

}  // namespace esphome::hydronic_s3
