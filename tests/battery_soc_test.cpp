#include "battery_soc_storage.h"
#include "ina219_api.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

static bool nearly(double a, double b, double err=0.01) { return std::fabs(a-b) < err; }
int main() {
    // Two cells IN SERIES still have the same Ah capacity as each cell.
    battery::SocPolicy p;
    p.capacity_mah=2000;
    p.rest_seconds=600;
    battery::SocEstimator s(p);
    assert(!s.info().available);
    s.initializeManual(100);
    assert(nearly(s.info().soc_percent,100));
    // 1A discharge for 1 hour, sampled each 10 seconds = 1000mAh consumed.
    for (int i=0; i<=360; ++i) s.update(true, 1000ull+uint64_t(i)*10000ull, 1.0, 7.6);
    assert(nearly(s.info().remaining_capacity_mah,1000));
    assert(nearly(s.info().soc_percent,50));
    assert(nearly(s.info().discharged_mah_session,1000));
    // Charging at -1A for 30min at 98% efficiency: 490mAh stored.
    for (int i=1; i<=180; ++i) s.update(true, 3601000ull+uint64_t(i)*10000ull, -1.0, 7.8);
    // First time step has mean of +1 to -1 amps = 0, then 179 intervals x 10s.
    assert(nearly(s.info().remaining_capacity_mah,1000 + 179.0*10.0/3600.0*1000.0*.98));
    assert(s.info().soc_percent>74 && s.info().soc_percent<75);
    // Invalid input: never integrate across a gap or invent current.
    const double before=s.info().remaining_capacity_mah;
    s.update(false, 5502000, 0, 0);
    s.update(true, 6000000, 3, 7.0);
    assert(nearly(s.info().remaining_capacity_mah,before));
    assert(s.info().uncertain);
    assert(s.info().skipped_gaps>=1);
    // A second valid sample integrates only its own interval.
    s.update(true, 6001000, 3, 7.0);
    assert(nearly(s.info().remaining_capacity_mah,before-3.0*1000.0/3600.0));
    // Full and empty clamps are safe against unrealistic extrapolation.
    s.initializeManual(0);
    for (int i=0;i<4;i++) s.update(true, 7000000+i*1000, 8.0, 7.0);
    assert(nearly(s.info().soc_percent,0));

    // OCV must not initialize under load; only a sufficiently long rest episode.
    battery::SocEstimator resting(p);
    resting.update(true, 1000, 0.6, 7.6);
    resting.update(true, 10000, 0.0, 7.6);
    assert(!resting.info().available);
    resting.update(true, 610000, 0.0, 7.6);
    assert(resting.info().available);
    assert(nearly(resting.info().soc_percent,50));
    assert(resting.info().uncertain); // OCV only an approximation
    // A manual calibration overrides persisted estimate and uncertainty.
    resting.initializeManual(77.5);
    assert(nearly(resting.info().soc_percent,77.5));
    assert(!resting.info().uncertain);
    // Persistence is capacity-matched and restart always flags missing downtime.
    std::string state=battery::formatSocState(resting.info(), 123456);
    double remaining=0; bool uncertain=false;
    assert(battery::parseSocState(state,2000,remaining,uncertain));
    assert(nearly(remaining,1550));
    assert(!battery::parseSocState(state,4000,remaining,uncertain));
    assert(!battery::parseSocState("garbage",2000,remaining,uncertain));
    battery::SocEstimator restarted(p);
    assert(restarted.restore(1550,false));
    assert(nearly(restarted.info().soc_percent,77.5));
    assert(restarted.info().uncertain);
    assert(restarted.info().stale);
    // JSON API exposes null instead of false SOC when no calibration exists.
    battery::BatterySnapshot snap;
    snap.valid=true; snap.timestamp_ms=123456;
    snap.soc=resting.info();
    const std::string full=battery::toJson(snap,battery::AlarmPolicy());
    assert(full.find("\"schema_version\":2")!=std::string::npos);
    assert(full.find("\"soc_percent\":77.500000")!=std::string::npos);
    assert(battery::toSocJson(snap).find("\"soc\":{")!=std::string::npos);
    battery::SocEstimator uninitialized(p);
    snap.soc=uninitialized.info();
    assert(battery::toJson(snap,battery::AlarmPolicy()).find("\"soc_percent\":null")!=std::string::npos);
    // Persist to an actual file, then reload. No filesystem access by the Pi required.
    char tempdir[]="/tmp/ina219-soc-test-XXXXXX";
    assert(::mkdtemp(tempdir)!=nullptr);
    std::string path=std::string(tempdir)+"/soc_state.txt";
    assert(battery::saveSocState(path,resting.info(),999));
    assert(battery::loadSocState(path,2000,remaining,uncertain));
    assert(nearly(remaining,1550));
    std::remove(path.c_str());
    ::rmdir(tempdir);
    std::cout << "SOC integration/rest calibration/storage/JSON tests passed\n";
}
