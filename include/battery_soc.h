#pragma once
// Estimate SOC by coulomb counting while powered, with optional settled-voltage
// calibration. No individual-cell telemetry or temperature compensation.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace battery {
struct SocPolicy {
    double capacity_mah = 0.0; // configured PACK Ah. 2S1P Ah == one cell Ah.
    double charge_efficiency = 0.98; // approximation; effective stored charge
    double rest_current_a = 0.03;
    unsigned rest_seconds = 1800;
    double ocv_correction_weight = 0.20; // one correction per qualifying rest episode
    double max_sample_gap_s = 10.0; // NEVER integrate across missing time
};

inline double clampSoc(double x) { return std::max(0.0, std::min(100.0, x)); }
// Generic 2S Li-ion no-load voltage curve (illustrative, not a pack calibration).
inline double socFromRestVoltage(double v) {
    struct Point { double volts; double pct; };
    static const Point table[] = {
        {6.40, 0}, {6.80, 4}, {7.00, 9}, {7.20, 18}, {7.40, 30},
        {7.60, 50}, {7.80, 67}, {8.00, 80}, {8.20, 92}, {8.40, 100}
    };
    if (v <= table[0].volts) return 0;
    for (unsigned i = 1; i < sizeof(table)/sizeof(table[0]); ++i) {
        if (v <= table[i].volts) {
            double ratio = (v - table[i-1].volts) / (table[i].volts-table[i-1].volts);
            return table[i-1].pct + ratio*(table[i].pct-table[i-1].pct);
        }
    }
    return 100;
}

struct SocInfo {
    bool available = false;
    double soc_percent = 0;
    double remaining_capacity_mah = 0;
    double nominal_capacity_mah = 0;
    double charged_mah_session = 0;
    double discharged_mah_session = 0;
    std::string source = "uninitialized";
    bool uncertain = false; // true after unmeasured intervals/offline restart
    bool stale = true; // true when most recent sensor sample is invalid
    bool rest_qualified = false;
    uint64_t skipped_gaps = 0;
};

class SocEstimator {
public:
    explicit SocEstimator(const SocPolicy& policy) : p_(policy) {}
    bool enabled() const { return p_.capacity_mah > 0; }
    bool hasEstimate() const { return enabled() && initialized_; }
    const SocPolicy& policy() const { return p_; }

    void initializeManual(double percent) {
        if (!enabled()) return;
        remaining_mah_ = p_.capacity_mah * clampSoc(percent)/100.0;
        initialized_ = true;
        source_ = "manual";
        uncertain_ = false;
        stale_ = true;
        anchor_ = false;
        rest_start_ms_ = 0;
        rest_adjusted_ = false;
    }

    // On restart we cannot observe charge/discharge while the Pi was off.
    bool restore(double remainingMah, bool previousUncertain) {
        if (!enabled() || !std::isfinite(remainingMah) || remainingMah < 0 ||
            remainingMah > p_.capacity_mah) return false;
        remaining_mah_ = remainingMah;
        initialized_ = true;
        source_ = "restored";
        uncertain_ = true; // offline charging/discharging is unmeasured
        (void) previousUncertain;
        stale_ = true;
        anchor_ = false;
        rest_start_ms_ = 0;
        rest_adjusted_ = false;
        return true;
    }

    // monotonic_ms must be time measured AFTER successful INA219 conversion.
    void update(bool valid, uint64_t monotonic_ms, double current_a, double battery_v) {
        if (!enabled()) return;
        if (!valid || !std::isfinite(current_a) || !std::isfinite(battery_v) ||
            battery_v < 0 || std::fabs(current_a) > 12.0) {
            stale_ = true;
            if (anchor_) { ++skipped_gaps_; uncertain_ = true; }
            anchor_ = false;
            rest_start_ms_ = 0;
            rest_adjusted_ = false;
            return;
        }
        stale_ = false;
        if (anchor_) {
            // Trapezoid integration; skip long samples to avoid inventing current.
            if (monotonic_ms > last_ms_) {
                const double seconds = (monotonic_ms - last_ms_)/1000.0;
                if (seconds <= p_.max_sample_gap_s) {
                    const double mean_a = (last_a_ + current_a)*0.5;
                    const double mah = mean_a*1000.0*seconds/3600.0;
                    if (mean_a >= 0) discharged_session_mah_ += mah;
                    else charged_session_mah_ += (-mah)*p_.charge_efficiency;
                    if (initialized_) {
                        const double delta = mah >= 0 ? -mah : (-mah)*p_.charge_efficiency;
                        remaining_mah_ = std::max(0.0, std::min(p_.capacity_mah, remaining_mah_+delta));
                        if (std::fabs(mah) > 0.0) {
                            source_ = (source_ == "restored" || source_ == "coulomb_restored")
                                ? "coulomb_restored" : "coulomb";
                        }
                    }
                } else {
                    ++skipped_gaps_;
                    uncertain_ = true;
                }
            } else {
                ++skipped_gaps_;
                uncertain_ = true;
            }
        }
        last_ms_ = monotonic_ms;
        last_a_ = current_a;
        anchor_ = true;
        if (std::fabs(current_a) <= p_.rest_current_a) {
            if (!rest_start_ms_) { rest_start_ms_ = monotonic_ms ? monotonic_ms : 1; rest_adjusted_ = false; }
            if (!rest_adjusted_ && monotonic_ms >= rest_start_ms_ &&
                (monotonic_ms-rest_start_ms_) >= uint64_t(p_.rest_seconds)*1000u) {
                const double ocvSoc = socFromRestVoltage(battery_v);
                if (!initialized_) {
                    remaining_mah_ = p_.capacity_mah*ocvSoc/100.0;
                    initialized_ = true;
                    source_ = "rest_voltage_initial";
                } else {
                    double oldSoc = remaining_mah_/p_.capacity_mah*100.0;
                    double nextSoc = oldSoc + p_.ocv_correction_weight*(ocvSoc-oldSoc);
                    remaining_mah_ = p_.capacity_mah*clampSoc(nextSoc)/100.0;
                    source_ = "rest_voltage_corrected";
                }
                // A generic OCV table does not fully eliminate uncertainty.
                uncertain_ = true;
                rest_adjusted_ = true;
            }
        } else {
            rest_start_ms_ = 0;
            rest_adjusted_ = false;
        }
    }

    SocInfo info() const {
        SocInfo v;
        v.nominal_capacity_mah = p_.capacity_mah;
        v.charged_mah_session = charged_session_mah_;
        v.discharged_mah_session = discharged_session_mah_;
        v.source = enabled() ? source_ : "capacity_not_configured";
        v.available = hasEstimate();
        if (v.available) {
            v.remaining_capacity_mah = remaining_mah_;
            v.soc_percent = clampSoc(100.0*remaining_mah_/p_.capacity_mah);
        }
        v.uncertain = uncertain_;
        v.stale = stale_;
        v.rest_qualified = rest_adjusted_;
        v.skipped_gaps = skipped_gaps_;
        return v;
    }
private:
    SocPolicy p_;
    double remaining_mah_ = 0;
    double charged_session_mah_ = 0;
    double discharged_session_mah_ = 0;
    bool initialized_ = false, uncertain_ = false, stale_ = true, anchor_ = false;
    bool rest_adjusted_ = false;
    uint64_t last_ms_ = 0, rest_start_ms_ = 0, skipped_gaps_ = 0;
    double last_a_ = 0;
    std::string source_ = "uninitialized";
};
}  // namespace battery
