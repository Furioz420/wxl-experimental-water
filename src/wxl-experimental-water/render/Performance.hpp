#pragma once
#include "common/Log.hpp"
#include <chrono>
#include <algorithm>

namespace wxl::water::perf {
// Submission timings do not measure GPU execution. Log once per ten seconds so
// the next runtime test can distinguish simulation, upload, draw and copy costs.
using Clock = std::chrono::steady_clock;
enum class Stage { Simulation, Upload, Draw, Capture, Material, Bind, Geometry };
struct Counters {
    double ms[7] = {};
    double frameMs = 0, maxFrameMs = 0;
    unsigned frames = 0, draws = 0;
    unsigned over50 = 0, over100 = 0;
    unsigned long long primitives = 0;
};
inline Counters counters;
inline double Milliseconds(Clock::duration elapsed) {
    return std::chrono::duration<double, std::milli>(elapsed).count();
}
class Scope {
    Stage stage_;
    bool active_;
    Clock::time_point start_ = Clock::now();
public:
    explicit Scope(Stage stage, bool active = true): stage_(stage), active_(active) {}
    ~Scope() { if (active_) counters.ms[static_cast<unsigned>(stage_)] += Milliseconds(Clock::now() - start_); }
};
inline void Draw(unsigned primitives) { ++counters.draws; counters.primitives += primitives; }
inline void Frame() {
    const auto now = Clock::now();
    static auto previous = now, window = now;
    const double elapsed = Milliseconds(now - previous);
    previous = now;
    if (elapsed <= 0) return;
    counters.frameMs += elapsed;
    counters.maxFrameMs = std::max(counters.maxFrameMs, elapsed);
    if (elapsed > 50.0) ++counters.over50;
    if (elapsed > 100.0) ++counters.over100;
    ++counters.frames;
    if (Milliseconds(now - window) < 10000) return;
    if (counters.draws) {
        const double n = counters.frames;
        WLOG_INFO("water-perf-v3: frames=%u draws=%u primitives=%llu frame_avg_ms=%.3f frame_max_ms=%.3f over50=%u over100=%u cpu_per_frame_ms[sim/upload/draw/copy/material-inclusive/bind/geometry]=%.3f/%.3f/%.3f/%.3f/%.3f/%.3f/%.3f",
            counters.frames, counters.draws, counters.primitives, counters.frameMs/n, counters.maxFrameMs,
            counters.over50, counters.over100,
            counters.ms[0]/n, counters.ms[1]/n, counters.ms[2]/n, counters.ms[3]/n,
            counters.ms[4]/n, counters.ms[5]/n, counters.ms[6]/n);
    }
    counters = {};
    window = now;
}
}
