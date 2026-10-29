#include "ina219_api.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

bool near(double a, double b) { return std::fabs(a-b) < 0.0001; }
int main() {
    static_assert(ina219::kConfig == 0x299B, "expected one-shot measurement");
    static_assert(ina219::kCalibration == 16384, "wrong calibration");
    assert(near(ina219::currentFromShuntAmps(1446), 1.446));
    assert(near(ina219::currentAmps(5152), 1.288));
    assert(near(ina219::currentFromShuntAmps(-2000), -2.0));
    assert(ina219::conversionReady(0x0002));
    assert(!ina219::overflow(0x0002));
    assert(ina219::overflow(0x0001));
    assert(near(ina219::batteryVolts(0x4000, 3000), 8.222));

    battery::AlarmTracker tracker;
    assert(tracker.level() == battery::AlarmLevel::Normal);
    tracker.update(true, 6.70); tracker.update(true, 6.70);
    assert(tracker.level() == battery::AlarmLevel::Normal);
    assert(tracker.update(true, 6.70));
    assert(tracker.level() == battery::AlarmLevel::Warning);
    // Invalid readings cannot clear alerts.
    for (int i=0; i<5; ++i) tracker.update(false, 8.1);
    assert(tracker.level() == battery::AlarmLevel::Warning);
    for (int i=0; i<3; ++i) tracker.update(true, 6.45);
    assert(tracker.level() == battery::AlarmLevel::Critical);
    for (int i=0; i<3; ++i) tracker.update(true, 6.55); // below 6.5+0.2
    assert(tracker.level() == battery::AlarmLevel::Critical);
    for (int i=0; i<3; ++i) tracker.update(true, 6.75);
    assert(tracker.level() == battery::AlarmLevel::Warning);
    for (int i=0; i<3; ++i) tracker.update(true, 7.05);
    assert(tracker.level() == battery::AlarmLevel::Normal);
    assert(tracker.changes() == 4);

    battery::BatterySnapshot s;
    s.valid = true;
    s.timestamp_ms = 12345;
    s.last_valid_ms = 12345;
    s.battery_voltage_v = 7.765;
    s.bus_voltage_v = 7.752;
    s.shunt_voltage_mv = 12.82;
    s.current_a = 1.282;
    s.chip_current_a = 1.288;
    s.pack_power_w = 9.95673;
    s.state = "discharging";
    s.alarm = battery::AlarmLevel::Warning;
    std::string j = battery::toJson(s, tracker.policy());
    assert(j.find("\"battery_voltage_v\":7.765000") != std::string::npos);
    assert(j.find("\"alarm\":{\"level\":\"warning\"") != std::string::npos);
    assert(j.find("\"valid\":true") != std::string::npos);
    s.valid = false;
    s.error = "i2c_error";
    j = battery::toJson(s, tracker.policy());
    assert(j.find("\"battery_voltage_v\":null") != std::string::npos);
    assert(j.find("\"sensor_fault\":true") != std::string::npos);
    assert(j.find("\"level\":\"warning\"") != std::string::npos);
    std::puts("PASS: conversion / alarm hysteresis / JSON validity & fields");
}
