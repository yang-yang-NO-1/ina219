#pragma once
// Public interface for future C++ UI and the JSON API.  All SI units.
#include "ina219_math.h"
#include "battery_soc.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

namespace battery {

enum class AlarmLevel { Normal, Warning, Critical };
inline const char* alarmName(AlarmLevel l) {
    switch (l) {
        case AlarmLevel::Warning: return "warning";
        case AlarmLevel::Critical: return "critical";
        default: return "normal";
    }
}

struct AlarmPolicy {
    // 2S BMS is reported to cut off near 6.4 V. These are EARLIER software alerts,
    // not an attempt to replace or override hardware cell protection.
    double warning_v = 6.80;
    double critical_v = 6.50;
    double hysteresis_v = 0.20;
    unsigned confirm_samples = 3;
};

class AlarmTracker {
public:
    explicit AlarmTracker(AlarmPolicy policy = AlarmPolicy()) : policy_(policy) {}
    // Invalid readings never downgrade/clear an existing alarm.
    bool update(bool valid, double battery_v) {
        if (!valid || !std::isfinite(battery_v)) { candidate_count_ = 0; return false; }
        AlarmLevel target = level_;
        switch (level_) {
            case AlarmLevel::Normal:
                if (battery_v <= policy_.critical_v) target = AlarmLevel::Critical;
                else if (battery_v <= policy_.warning_v) target = AlarmLevel::Warning;
                break;
            case AlarmLevel::Warning:
                if (battery_v <= policy_.critical_v) target = AlarmLevel::Critical;
                else if (battery_v >= policy_.warning_v + policy_.hysteresis_v)
                    target = AlarmLevel::Normal;
                break;
            case AlarmLevel::Critical:
                if (battery_v >= policy_.critical_v + policy_.hysteresis_v) {
                    target = battery_v >= policy_.warning_v + policy_.hysteresis_v
                        ? AlarmLevel::Normal : AlarmLevel::Warning;
                }
                break;
        }
        if (target == level_) { candidate_count_ = 0; return false; }
        if (target != candidate_) { candidate_ = target; candidate_count_ = 0; }
        if (++candidate_count_ < policy_.confirm_samples) return false;
        level_ = target;
        candidate_count_ = 0;
        ++changes_;
        return true;
    }
    AlarmLevel level() const { return level_; }
    uint64_t changes() const { return changes_; }
    const AlarmPolicy& policy() const { return policy_; }
private:
    AlarmPolicy policy_;
    AlarmLevel level_ = AlarmLevel::Normal;
    AlarmLevel candidate_ = AlarmLevel::Normal;
    unsigned candidate_count_ = 0;
    uint64_t changes_ = 0;
};

// This is a value object; independent UI code can read/copy it without I2C access.
struct BatterySnapshot {
    bool valid = false;             // False means ALL numeric readings are invalid.
    std::string error = "starting";
    uint64_t timestamp_ms = 0;     // Unix epoch in milliseconds, attempt time.
    uint64_t last_valid_ms = 0;    // Unix epoch of last successful sample, 0 if none.
    double bus_voltage_v = 0;
    double battery_voltage_v = 0;
    double shunt_voltage_mv = 0;
    double current_a = 0;          // Preferred: shunt voltage / 0.010 ohm.
    double chip_current_a = 0;     // INA219 calibrated Current register.
    double chip_power_w = 0;       // INA219 unsigned power register; use pack_power_w for direction.
    double pack_power_w = 0;       // Signed battery_voltage * current_a.
    bool current_mismatch = false; // Shunt-derived vs calibrated register: diagnostic ONLY.
    bool conversion_ready = false;
    bool overflow = false;
    std::string state = "unknown"; // charging, discharging, idle, unknown.
    bool soc_available = false;    // Only for near-idle, voltage-based rough indication.
    int soc_rough_percent = 0;
    AlarmLevel alarm = AlarmLevel::Normal;
    uint64_t alarm_changes = 0;
    SocInfo soc;                      // SOC estimate for UI, see /api/v1/soc.
};

// The UI should handle null readings on faults and explicitly inspect valid/error.
inline std::string jsonQuote(const std::string& value) {
    std::string s = "\"";
    for (unsigned char c : value) {
        if (c == '\"') s += "\\\"";
        else if (c == '\\') s += "\\\\";
        else if (c == '\n') s += "\\n";
        else if (c == '\r') s += "\\r";
        else if (c == '\t') s += "\\t";
        else if (c < 0x20) {
            char buf[7]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); s += buf;
        } else s += static_cast<char>(c);
    }
    return s + "\"";
}
inline std::string numberOrNull(bool valid, double v) {
    if (!valid || !std::isfinite(v)) return "null";
    char buf[64]; std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}
inline std::string alarmObjectJson(const BatterySnapshot& s, const AlarmPolicy& p) {
    std::ostringstream o;
    o << "{\"level\":" << jsonQuote(alarmName(s.alarm))
      << ",\"active\":" << (s.alarm != AlarmLevel::Normal ? "true" : "false")
      << ",\"sensor_fault\":" << (!s.valid ? "true" : "false")
      << ",\"changes\":" << s.alarm_changes
      << ",\"warning_threshold_v\":" << numberOrNull(true, p.warning_v)
      << ",\"critical_threshold_v\":" << numberOrNull(true, p.critical_v)
      << ",\"recovery_hysteresis_v\":" << numberOrNull(true, p.hysteresis_v)
      << ",\"confirm_samples\":" << p.confirm_samples
      << ",\"hardware_cutoff_estimate_v\":6.4,\"hardware_cutoff_is_guaranteed\":false}";
    return o.str();
}
inline std::string toAlarmJson(const BatterySnapshot& s, const AlarmPolicy& p) {
    std::ostringstream o;
    o << "{\"schema_version\":1,\"timestamp_ms\":" << s.timestamp_ms
      << ",\"valid\":" << (s.valid ? "true" : "false")
      << ",\"battery_voltage_v\":" << numberOrNull(s.valid, s.battery_voltage_v)
      << ",\"alarm\":" << alarmObjectJson(s, p) << "}";
    return o.str();
}
inline std::string socObjectJson(const SocInfo& soc) {
    std::ostringstream o;
    o << "{\"available\":" << (soc.available ? "true" : "false")
      << ",\"soc_percent\":" << numberOrNull(soc.available, soc.soc_percent)
      << ",\"remaining_capacity_mah\":" << numberOrNull(soc.available, soc.remaining_capacity_mah)
      << ",\"nominal_capacity_mah\":" << numberOrNull(soc.nominal_capacity_mah > 0, soc.nominal_capacity_mah)
      << ",\"charged_mah_session\":" << numberOrNull(soc.nominal_capacity_mah > 0, soc.charged_mah_session)
      << ",\"discharged_mah_session\":" << numberOrNull(soc.nominal_capacity_mah > 0, soc.discharged_mah_session)
      << ",\"source\":" << jsonQuote(soc.source)
      << ",\"uncertain\":" << (soc.uncertain ? "true" : "false")
      << ",\"stale\":" << (soc.stale ? "true" : "false")
      << ",\"rest_qualified\":" << (soc.rest_qualified ? "true" : "false")
      << ",\"skipped_gaps\":" << soc.skipped_gaps
      << "}";
    return o.str();
}
inline std::string toSocJson(const BatterySnapshot& s) {
    std::ostringstream o;
    o << "{\"schema_version\":2,\"timestamp_ms\":" << s.timestamp_ms
      << ",\"sensor_valid\":" << (s.valid ? "true" : "false")
      << ",\"soc\":" << socObjectJson(s.soc) << "}";
    return o.str();
}
inline std::string toJson(const BatterySnapshot& s, const AlarmPolicy& p) {
    std::ostringstream o;
    o << "{\"schema_version\":2,\"timestamp_ms\":" << s.timestamp_ms
      << ",\"last_valid_ms\":" << (s.last_valid_ms ? std::to_string(s.last_valid_ms) : "null")
      << ",\"valid\":" << (s.valid ? "true" : "false")
      << ",\"error\":" << (s.valid ? "null" : jsonQuote(s.error))
      << ",\"bus_voltage_v\":" << numberOrNull(s.valid, s.bus_voltage_v)
      << ",\"battery_voltage_v\":" << numberOrNull(s.valid, s.battery_voltage_v)
      << ",\"shunt_voltage_mv\":" << numberOrNull(s.valid, s.shunt_voltage_mv)
      << ",\"current_a\":" << numberOrNull(s.valid, s.current_a)
      << ",\"chip_current_a\":" << numberOrNull(s.valid, s.chip_current_a)
      << ",\"chip_power_w\":" << numberOrNull(s.valid, s.chip_power_w)
      << ",\"pack_power_w\":" << numberOrNull(s.valid, s.pack_power_w)
      << ",\"current_mismatch\":" << (s.valid ? (s.current_mismatch ? "true" : "false") : "null")
      << ",\"conversion_ready\":" << (s.valid ? (s.conversion_ready ? "true" : "false") : "null")
      << ",\"overflow\":" << (s.overflow ? "true" : "false")
      << ",\"state\":" << jsonQuote(s.valid ? s.state : "unknown")
      << ",\"soc_rough_percent\":" << (s.valid && s.soc_available ? std::to_string(s.soc_rough_percent) : "null")
      << ",\"alarm\":" << alarmObjectJson(s, p)
      << ",\"soc\":" << socObjectJson(s.soc)
      << "}";
    return o.str();
}
}  // namespace battery
