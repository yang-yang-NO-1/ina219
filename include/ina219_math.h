#pragma once
#include <cstdint>
namespace ina219 {
constexpr float kShuntOhms = 0.010f;
constexpr float kCurrentLsbA = 0.00025f;
constexpr uint16_t kCalibration = 16384;
// 32V bus, +/-80mV shunt PGA, 12-bit bus/shunt ADC, triggered shunt+bus.
// Triggered sampling lets the Current register and Shunt register correspond
// to the same completed conversion, avoiding occasional mismatches.
constexpr uint16_t kConfig = 0x299B;
constexpr float kCurrentDeadbandA = 0.030f;
inline float shuntMilliVolts(int16_t raw) { return raw * 0.01f; }
inline float busVolts(uint16_t raw) { return (raw >> 3) * 0.004f; }
inline float currentAmps(int16_t raw) { return raw * kCurrentLsbA; }
inline float currentFromShuntAmps(int16_t raw) {
    return shuntMilliVolts(raw) / (1000.0f * kShuntOhms);
}
inline float batteryVolts(uint16_t busRaw, int16_t shuntRaw) {
    return busVolts(busRaw) + shuntMilliVolts(shuntRaw) / 1000.0f;
}
inline bool conversionReady(uint16_t rawBus) { return (rawBus & 0x0002u) != 0; }
inline bool overflow(uint16_t rawBus) { return (rawBus & 0x0001u) != 0; }
inline const char* state(float amps) {
    if (amps > kCurrentDeadbandA) return "discharging";
    if (amps < -kCurrentDeadbandA) return "charging";
    return "idle";
}
inline int roughSocPercent(float v) {
    if (v >= 8.40f) return 100;
    if (v >= 8.20f) return 90;
    if (v >= 8.00f) return 80;
    if (v >= 7.80f) return 70;
    if (v >= 7.60f) return 60;
    if (v >= 7.40f) return 50;
    if (v >= 7.20f) return 35;
    if (v >= 7.00f) return 25;
    if (v >= 6.80f) return 15;
    if (v >= 6.60f) return 8;
    if (v >= 6.40f) return 3;
    return 0;
}
}  // namespace ina219
