#pragma once
#include "nr_log.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace nr::diagbuild {
enum class Mode { none, late, late_gpu, late_pipelines };

inline Mode parse(const char* value) {
    if (!value) return Mode::none;
    if (!std::strcmp(value, "late")) return Mode::late;
    if (!std::strcmp(value, "late-gpu")) return Mode::late_gpu;
    if (!std::strcmp(value, "late-pipelines")) return Mode::late_pipelines;
    return Mode::none;
}
inline Mode active() { return parse(std::getenv("NR_DIAG_BUILD")); }
inline const char* name(Mode mode) {
    switch (mode) {
    case Mode::late: return "late";
    case Mode::late_gpu: return "late-gpu";
    case Mode::late_pipelines: return "late-pipelines";
    default: return "baseline";
    }
}
inline double delay(const char* value) {
    if (!value || !*value || *value < '0' || *value > '9') return 8.0;
    char* end = nullptr;
    const double seconds = std::strtod(value, &end);
    return end != value && !*end && std::isfinite(seconds) && seconds >= 0 && seconds <= 86400
        ? seconds : 8.0;
}
using Clock = std::chrono::steady_clock;
inline Clock::time_point origin() {
    static const auto start = Clock::now();
    return start;
}
inline double elapsed() { return std::chrono::duration<double>(Clock::now() - origin()).count(); }
inline auto deadline(double seconds) {
    return origin() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}
inline thread_local bool background = false;
struct Background {
    bool previous = background;
    Background() { background = true; }
    ~Background() { background = previous; }
};
inline bool selected(Mode mode) { return background && active() == mode; }
inline void wait(Mode mode, const char* phase) {
    if (!selected(mode)) return;
    const double seconds = delay(std::getenv("NR_DIAG_BUILD_DELAY"));
    nr::logf("[nr] NR_DIAG_BUILD=%s wait begin %s: t=%.3fs, deadline=%.3fs (background build thread)",
             name(mode), phase, elapsed(), seconds);
    std::this_thread::sleep_until(deadline(seconds));
    nr::logf("[nr] NR_DIAG_BUILD=%s wait end %s: t=%.3fs (background build thread)",
             name(mode), phase, elapsed());
}
}
