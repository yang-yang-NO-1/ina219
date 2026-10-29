// Orange Pi 5 + INA219 10mOhm: terminal, JSON file and read-only local HTTP API.
// No wiringPi dependency; Linux i2c-dev, C++11.
#include "ina219_api.h"
#include "battery_soc_storage.h"
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <string>

namespace {
constexpr uint8_t kConfigReg = 0x00;
constexpr uint8_t kShuntReg = 0x01;
constexpr uint8_t kBusReg = 0x02;
constexpr uint8_t kPowerReg = 0x03;
constexpr uint8_t kCurrentReg = 0x04;
constexpr uint8_t kCalibrationReg = 0x05;
volatile sig_atomic_t running = 1;
void onSignal(int) { running = 0; }
uint64_t unixMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
uint64_t steadyMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Options {
    std::string device = "/dev/i2c-5";
    int address = 0x40;
    unsigned intervalSec = 1;
    unsigned httpPort = 0;   // Opt-in; only loopback 127.0.0.1.
    std::string jsonFile = "/run/ina219_battery.json";  // RAM-backed on most Linux systems.
    bool jsonLines = false;
    bool once = false;
    battery::AlarmPolicy alarm;
    battery::SocPolicy soc;
    std::string socStateFile = "/var/lib/ina219/soc_state.txt";
    unsigned socSaveSeconds = 120; // flash wear: no more than once every 2 minutes
    bool manualSocSpecified = false;
    double manualSocPercent = 0;
};
void usage(const char* name) {
    std::printf("Usage: %s [options]\n", name);
    std::puts("  --bus /dev/i2c-5    I2C bus (default /dev/i2c-5)");
    std::puts("  --address 0x40     7-bit I2C address (default 0x40)");
    std::puts("  --interval 1       Sampling interval, seconds [1,3600]");
    std::puts("  --http-port 8765   Read-only HTTP API on 127.0.0.1, disabled by default");
    std::puts("  --json-file PATH   Atomic JSON snapshot (default /run/ina219_battery.json)");
    std::puts("  --no-json-file     Disable JSON file writing");
    std::puts("  --json-lines       One JSON object per line on stdout");
    std::puts("  --once             Sample once and exit");
    std::puts("  --warning 6.8      Low battery threshold in volts");
    std::puts("  --critical 6.5     Critical threshold in volts");
    std::puts("  --hysteresis 0.2   Recovery hysteresis in volts");
    std::puts("  --confirm 3        Consecutive samples required for alarm transitions");
    std::puts("  --capacity-mah 2500  PACK nominal usable capacity (REQUIRED to enable SOC)");
    std::puts("  --initial-soc 100   Set initial known SOC, overrides saved state; use only for calibration");
    std::puts("  --soc-state-file P  Persistent SOC state (default /var/lib/ina219/soc_state.txt)");
    std::puts("  --no-soc-state      Disable SOC persistence");
    std::puts("  --soc-save-seconds 120  Save interval [30,3600]");
    std::puts("  --rest-seconds 1800  Continuous idle required before OCV calibration [600,7200]");
    std::puts("  --rest-current-ma 30  Max absolute idle current for OCV [0,100]");
    std::puts("  --charge-efficiency 0.98  Coulomb count charging factor [0.8,1]");
}
bool parseLong(const char* s, long lo, long hi, long& out) {
    if (!s || !*s || *s == '-') return false;
    errno = 0;
    char* end = nullptr;
    long val = std::strtol(s, &end, 0);
    if (errno || end == s || *end || val < lo || val > hi) return false;
    out = val; return true;
}
bool parseDouble(const char* s, double lo, double hi, double& out) {
    if (!s || !*s) return false;
    errno = 0;
    char* end = nullptr;
    double val = std::strtod(s, &end);
    if (errno || end == s || *end || !std::isfinite(val) || val < lo || val > hi) return false;
    out = val; return true;
}
bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--help" || key == "-h") { usage(argv[0]); std::exit(0); }
        if (key == "--once") { o.once = true; continue; }
        if (key == "--json-lines") { o.jsonLines = true; continue; }
        if (key == "--no-json-file") { o.jsonFile.clear(); continue; }
        if (key == "--no-soc-state") { o.socStateFile.clear(); continue; }
        if (i + 1 == argc) return false;
        const char* value = argv[++i];
        long num = 0;
        if (key == "--bus") {
            if (std::strncmp(value, "/dev/i2c-", 9) || !value[9]) return false;
            o.device = value;
        } else if (key == "--address") {
            if (!parseLong(value, 0x03, 0x77, num)) return false;
            o.address = static_cast<int>(num);
        } else if (key == "--interval") {
            if (!parseLong(value, 1, 3600, num)) return false;
            o.intervalSec = static_cast<unsigned>(num);
        } else if (key == "--http-port") {
            if (!parseLong(value, 1, 65535, num)) return false;
            o.httpPort = static_cast<unsigned>(num);
        } else if (key == "--json-file") {
            if (!*value) return false;
            o.jsonFile = value;
        } else if (key == "--warning") {
            if (!parseDouble(value, 6.40, 8.40, o.alarm.warning_v)) return false;
        } else if (key == "--critical") {
            if (!parseDouble(value, 6.40, 8.30, o.alarm.critical_v)) return false;
        } else if (key == "--hysteresis") {
            if (!parseDouble(value, 0.01, 0.80, o.alarm.hysteresis_v)) return false;
        } else if (key == "--confirm") {
            if (!parseLong(value, 1, 30, num)) return false;
            o.alarm.confirm_samples = static_cast<unsigned>(num);
        } else if (key == "--capacity-mah") {
            if (!parseDouble(value, 100, 100000, o.soc.capacity_mah)) return false;
        } else if (key == "--initial-soc") {
            if (!parseDouble(value, 0, 100, o.manualSocPercent)) return false;
            o.manualSocSpecified = true;
        } else if (key == "--soc-state-file") {
            if (!*value) return false;
            o.socStateFile = value;
        } else if (key == "--soc-save-seconds") {
            if (!parseLong(value, 30, 3600, num)) return false;
            o.socSaveSeconds = static_cast<unsigned>(num);
        } else if (key == "--rest-seconds") {
            if (!parseLong(value, 600, 7200, num)) return false;
            o.soc.rest_seconds = static_cast<unsigned>(num);
        } else if (key == "--rest-current-ma") {
            double currentMa=0;
            if (!parseDouble(value, 0, 100, currentMa)) return false;
            o.soc.rest_current_a = currentMa/1000.;
        } else if (key == "--charge-efficiency") {
            if (!parseDouble(value, 0.80, 1.0, o.soc.charge_efficiency)) return false;
        } else return false;
    }
    // Ensure an early warning before the BMS's reported ~6.4-V cutoff.
    return (!o.manualSocSpecified || o.soc.capacity_mah > 0) &&
           (o.soc.capacity_mah <= 0 || o.intervalSec <= 5) &&
           o.alarm.warning_v > o.alarm.critical_v &&
           o.alarm.critical_v >= 6.40 &&
           o.alarm.warning_v + o.alarm.hysteresis_v <= 9.0;
}

class INA219 {
public:
    ~INA219() { if (fd_ >= 0) ::close(fd_); }
    bool openDevice(const std::string& path, int address) {
        fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) { std::perror("opening I2C device"); return false; }
        unsigned long funcs = 0;
        if (::ioctl(fd_, I2C_FUNCS, &funcs) < 0 || !(funcs & I2C_FUNC_I2C)) {
            std::fprintf(stderr, "I2C adapter lacks I2C_RDWR\n"); return false;
        }
        addr_ = static_cast<uint16_t>(address); return true;
    }
    bool writeReg(uint8_t reg, uint16_t value) {
        uint8_t bytes[3] = {reg, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
        i2c_msg msg{}; msg.addr = addr_; msg.len = 3; msg.buf = bytes;
        i2c_rdwr_ioctl_data xfer{&msg, 1};
        return ::ioctl(fd_, I2C_RDWR, &xfer) == 1;
    }
    bool readReg(uint8_t reg, uint16_t& value) {
        uint8_t buf[2]{};
        i2c_msg msg[2]{};
        msg[0].addr = addr_; msg[0].len = 1; msg[0].buf = &reg;
        msg[1].addr = addr_; msg[1].flags = I2C_M_RD;
        msg[1].len = 2; msg[1].buf = buf;
        i2c_rdwr_ioctl_data xfer{msg, 2};
        if (::ioctl(fd_, I2C_RDWR, &xfer) != 2) return false;
        value = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
        return true;
    }
    bool initialize() {
        uint16_t cfg = 0, cal = 0;
        if (!readReg(kConfigReg, cfg) || cfg == 0xffff) return false;
        if (!writeReg(kConfigReg, ina219::kConfig) ||
            !writeReg(kCalibrationReg, ina219::kCalibration)) return false;
        return readReg(kConfigReg, cfg) && cfg == ina219::kConfig &&
               readReg(kCalibrationReg, cal) && cal == ina219::kCalibration;
    }
    bool sample(battery::BatterySnapshot& s) {
        uint16_t cal = 0, cfg = 0;
        if (!readReg(kCalibrationReg, cal) || !readReg(kConfigReg, cfg)) {
            s.error = "i2c_error"; return false;
        }
        if ((cal != ina219::kCalibration || cfg != ina219::kConfig) && !initialize()) {
            s.error = "reinitialize_failed"; return false;
        }
        // A NEW one-shot conversion. The device holds these registers stable
        // after completion; read them as one logical sample, unlike continuous mode.
        if (!writeReg(kConfigReg, ina219::kConfig)) { s.error = "trigger_failed"; return false; }
        uint16_t bus = 0;
        bool ready = false;
        for (unsigned n = 0; n < 100; ++n) { // up to 100 ms for 1.064-ms typical conversion
            if (!readReg(kBusReg, bus)) { s.error = "i2c_error"; return false; }
            if (ina219::conversionReady(bus)) { ready = true; break; }
            usleep(1000);
        }
        if (!ready) { s.error = "conversion_timeout"; return false; }
        if (ina219::overflow(bus)) { s.overflow = true; s.error = "math_overflow"; return false; }
        uint16_t rawShunt = 0, rawCurrent = 0, rawPower = 0;
        if (!readReg(kShuntReg, rawShunt) || !readReg(kCurrentReg, rawCurrent) ||
            !readReg(kPowerReg, rawPower)) { s.error = "i2c_error"; return false; }
        const int16_t shunt = static_cast<int16_t>(rawShunt);
        const int16_t chipCurrent = static_cast<int16_t>(rawCurrent);
        s.bus_voltage_v = ina219::busVolts(bus);
        s.battery_voltage_v = ina219::batteryVolts(bus, shunt);
        s.shunt_voltage_mv = ina219::shuntMilliVolts(shunt);
        s.current_a = ina219::currentFromShuntAmps(shunt);
        s.chip_current_a = ina219::currentAmps(chipCurrent);
        s.chip_power_w = rawPower * (20.0 * ina219::kCurrentLsbA);
        s.pack_power_w = s.battery_voltage_v * s.current_a;
        s.current_mismatch = std::fabs(s.current_a - s.chip_current_a) >
            std::max(0.15, std::fabs(s.current_a) * 0.15);
        s.state = ina219::state(static_cast<float>(s.current_a));
        s.conversion_ready = true;
        s.soc_available = std::fabs(s.current_a) <= 0.10;
        if (s.soc_available) s.soc_rough_percent = ina219::roughSocPercent(static_cast<float>(s.battery_voltage_v));
        s.error.clear(); s.valid = true;
        return true;
    }
private:
    int fd_ = -1;
    uint16_t addr_ = 0x40;
};

// Atomic snapshot replacement, on /run tmpfs by default to avoid flash wear.
bool writeSnapshotFile(const std::string& path, const std::string& json) {
    if (path.empty()) return true;
    std::string pattern = path + ".tmp.XXXXXX";
    std::string mutablePattern = pattern;
    int fd = ::mkstemp(&mutablePattern[0]);
    if (fd < 0) return false;
    ::fchmod(fd, 0644);
    const std::string payload = json + "\n";
    size_t written = 0;
    while (written < payload.size()) {
        ssize_t n = ::write(fd, payload.data() + written, payload.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ::close(fd); ::unlink(mutablePattern.c_str()); return false; }
        written += static_cast<size_t>(n);
    }
    if (::close(fd) != 0 || ::rename(mutablePattern.c_str(), path.c_str()) != 0) {
        ::unlink(mutablePattern.c_str()); return false;
    }
    return true;
}

class LocalHttpApi {
public:
    ~LocalHttpApi() { if (fd_ >= 0) ::close(fd_); }
    bool openPort(unsigned port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd_ < 0) return false;
        int reuse = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // No LAN exposure.
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(fd_, 8) < 0) { std::perror("HTTP API bind/listen"); return false; }
        return true;
    }
    void serve(const std::string& json, const std::string& alarmJson, const std::string& socJson) {
        if (fd_ < 0) return;
        // At most 8 connections per iteration; clients never access the I2C driver.
        for (int n = 0; n < 8; ++n) {
            int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) break;
            // Even on loopback, TCP accept may precede receipt of the HTTP request.
            // A short receive timeout avoids spurious 404 responses with curl.
            timeval timeout{0, 150000};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            char buf[1024]{};
            ssize_t got = ::recv(client, buf, sizeof(buf) - 1, 0);
            std::string path;
            bool get = got > 0 && std::strncmp(buf, "GET ", 4) == 0;
            if (get) {
                const char* start = buf + 4;
                const char* end = static_cast<const char*>(std::memchr(start, ' ', got - 4));
                if (end) path.assign(start, end);
            }
            const bool batteryGet = get && path == "/api/v1/battery";
            const bool alarmGet = get && path == "/api/v1/alarm";
            const bool socGet = get && path == "/api/v1/soc";
            const bool healthGet = get && path == "/health";
            std::string body;
            if (batteryGet || alarmGet || socGet) {
                body = alarmGet ? alarmJson : (socGet ? socJson : json);
            } else if (healthGet) body = "{\"alive\":true}";
            else body = "{\"error\":\"not_found\"}";
            int code = (batteryGet || alarmGet || socGet || healthGet) ? 200 : 404;
            std::string response = "HTTP/1.1 " + std::string(code == 200 ? "200 OK" : "404 Not Found") +
                "\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body;
            size_t sent = 0;
            while (sent < response.size()) {
                ssize_t bytes = ::send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                if (bytes < 0 && errno == EINTR) continue;
                if (bytes <= 0) break;
                sent += static_cast<size_t>(bytes);
            }
            ::close(client);
        }
    }
private:
    int fd_ = -1;
};

void printHuman(const battery::BatterySnapshot& s, const battery::AlarmPolicy& policy) {
    if (!s.valid) { std::fprintf(stderr, "Sample invalid: %s (existing alarm retained)\n", s.error.c_str()); return; }
    std::printf("Bus: %.3f V | Battery: %.3f V | Shunt: %+.2f mV\n", s.bus_voltage_v, s.battery_voltage_v, s.shunt_voltage_mv);
    std::printf("Current (shunt): %+.3f A | INA219 register: %+.3f A | Pack power: %+.2f W | %s\n",
                s.current_a, s.chip_current_a, s.pack_power_w, s.state.c_str());
    if (s.current_mismatch) std::puts("NOTICE: INA219 register differs from shunt voltage; inspect transients/calibration.");
    if (s.soc.available) {
        std::printf("SOC: %.1f%% (%.0f / %.0f mAh, source=%s%s%s)\n",
                    s.soc.soc_percent, s.soc.remaining_capacity_mah,
                    s.soc.nominal_capacity_mah, s.soc.source.c_str(),
                    s.soc.uncertain?", approximate":"", s.soc.stale?", stale":"");
    } else if (s.soc.nominal_capacity_mah > 0) {
        std::puts("SOC: initializing (provide --initial-soc once, or wait for sustained rest)");
    } else std::puts("SOC: disabled; set --capacity-mah and initial SOC for coulomb counting");
    if (s.soc_available) std::printf("Rough REST-VOLTAGE SOC only: %d%%\n", s.soc_rough_percent);
    std::printf("Low battery alarm: %s (warning <= %.2fV, critical <= %.2fV)\n",
                battery::alarmName(s.alarm), policy.warning_v, policy.critical_v);
    std::puts("---"); std::fflush(stdout);
}
}

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) { usage(argv[0]); return 2; }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    INA219 chip;
    if (!chip.openDevice(o.device, o.address) || !chip.initialize()) {
        std::fprintf(stderr, "Cannot initialize INA219 at %s addr 0x%02X; check 3.3V/GND/I2C.\n",
                     o.device.c_str(), o.address);
        return 1;
    }
    LocalHttpApi http;
    if (o.httpPort && !http.openPort(o.httpPort)) return 1;
    std::fprintf(stderr, "INA219 R010 ready: %s 0x%02X, CONFIG=0x%04X CAL=%u\n",
                 o.device.c_str(), o.address, ina219::kConfig, ina219::kCalibration);
    if (o.httpPort) std::fprintf(stderr, "HTTP API: http://127.0.0.1:%u/api/v1/battery\n", o.httpPort);
    if (!o.jsonFile.empty()) std::fprintf(stderr, "JSON snapshot: %s\n", o.jsonFile.c_str());
    battery::AlarmTracker alarm(o.alarm);
    battery::SocEstimator soc(o.soc);
    if (o.soc.capacity_mah > 0) {
        if (!o.socStateFile.empty() && o.socStateFile == "/var/lib/ina219/soc_state.txt") {
            if (::mkdir("/var/lib/ina219", 0755) < 0 && errno != EEXIST)
                std::fprintf(stderr, "WARNING: cannot create /var/lib/ina219: %s\n", std::strerror(errno));
        }
        if (o.manualSocSpecified) {
            soc.initializeManual(o.manualSocPercent);
            std::fprintf(stderr, "SOC explicitly initialized at %.1f%% (manual override).\n", o.manualSocPercent);
        } else if (!o.socStateFile.empty()) {
            double previousMah=0;
            bool previousUncertain=false;
            if (battery::loadSocState(o.socStateFile, o.soc.capacity_mah, previousMah, previousUncertain)
                && soc.restore(previousMah, previousUncertain)) {
                std::fprintf(stderr, "SOC restored from %s; time while Pi was off was NOT counted.\n", o.socStateFile.c_str());
            } else {
                std::fprintf(stderr, "SOC not initialized: no matching saved state. Use --initial-soc only when known, or rest for OCV initialization.\n");
            }
        }
    }
    battery::BatterySnapshot snapshot;
    uint64_t lastSocSave = steadyMs();
    bool socFileFailureReported = false;
    bool fileFailureReported = false;
    uint64_t nextSample = 0;
    int onceResult = 0;
    while (running) {
        uint64_t now = steadyMs();
        if (nextSample == 0 || now >= nextSample) {
            // Schedule next cycle from current time (no bursts after blocking).
            nextSample = now + o.intervalSec * 1000ull;
            const uint64_t prevLastValid = snapshot.last_valid_ms;
            snapshot = battery::BatterySnapshot();
            snapshot.timestamp_ms = unixMs();
            snapshot.last_valid_ms = prevLastValid;
            bool ok = chip.sample(snapshot);
            const uint64_t conversionCompleteMs = steadyMs();
            if (ok) snapshot.last_valid_ms = snapshot.timestamp_ms;
            soc.update(ok, conversionCompleteMs, snapshot.current_a, snapshot.battery_voltage_v);
            snapshot.soc = soc.info();
            bool changed = alarm.update(ok, snapshot.battery_voltage_v);
            snapshot.alarm = alarm.level();
            snapshot.alarm_changes = alarm.changes();
            if (changed) std::fprintf(stderr, "BATTERY ALARM changed to %s (%.3f V)\n",
                        battery::alarmName(snapshot.alarm), snapshot.battery_voltage_v);
            std::string json = battery::toJson(snapshot, o.alarm);
            if (!o.jsonFile.empty() && !writeSnapshotFile(o.jsonFile, json) && !fileFailureReported) {
                std::fprintf(stderr, "Warning: cannot write JSON file: %s\n", o.jsonFile.c_str());
                fileFailureReported = true;
            }
            if (soc.hasEstimate() && !o.socStateFile.empty() &&
                conversionCompleteMs - lastSocSave >= uint64_t(o.socSaveSeconds)*1000u) {
                if (!battery::saveSocState(o.socStateFile, snapshot.soc, snapshot.timestamp_ms)
                    && !socFileFailureReported) {
                    std::fprintf(stderr, "WARNING: cannot persist SOC at %s\n", o.socStateFile.c_str());
                    socFileFailureReported = true;
                }
                lastSocSave = conversionCompleteMs;
            }
            if (o.jsonLines) { std::puts(json.c_str()); std::fflush(stdout); }
            else printHuman(snapshot, o.alarm);
            if (o.once) { onceResult = ok ? 0 : 1; break; }
        }
        // Serve UI requests against a cached snapshot; never let UI drive I2C.
        std::string json = battery::toJson(snapshot, o.alarm);
        http.serve(json, battery::toAlarmJson(snapshot, o.alarm), battery::toSocJson(snapshot));
        usleep(20000);
    }
    // Also persist on graceful Ctrl+C / SIGTERM or --once; SIGKILL/power loss
    // can still lose at most the configured save interval of measurements.
    if (soc.hasEstimate() && !o.socStateFile.empty() &&
        !battery::saveSocState(o.socStateFile, soc.info(), unixMs())) {
        std::fprintf(stderr, "WARNING: failed to save SOC at shutdown: %s\n", o.socStateFile.c_str());
    }
    return onceResult;
}
