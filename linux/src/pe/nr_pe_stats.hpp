#pragma once
// NR's run statistics over the whole session (since the game started or the last reset), for NR Control's Diagnostics
// (nr_control_pages.hpp draw_session_stats), its report and the diagnostic package: per NR-chain layer, how often an
// evaluation asked NR for a frame, how often the network ran and how often the frame passed through, by reason - each
// reason with its count and when it was first and last seen, and whether it is expected (NR off, a network being built,
// a frame size settling, ...), video memory (Memory Guard), or a fault. The longest stretch a layer was asked for and did
// not run, and the reason it began with. And the presents: the game's own, all the frames shown (frame generation's
// too), and the game frames whose Present layers ran at DLSS-G's evaluate. Counting only: nothing here decides whether
// NR runs. CPU-side and in this process (the .asi and the ReShade add-on each keep their own).
#include "nr_pe_log.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace nr::pe::stats {

enum Kind : uint8_t { kExpected, kMemory, kFault };
inline const char* kind_name(Kind k) { return k == kExpected ? "expected" : k == kMemory ? "video memory" : "fault"; }

// The reasons a frame passed through that are part of normal running: waiting for something that comes by itself.
inline Kind kind_of(const std::string& why) {
    static const char* const expected[] = {"building", "size is changing", "changing frame size", "moving",
                                           "freeing the previous network", "freeing the least recently used",
                                           "weights are moving to RAM", "plan for this frame size", "NR is off",
                                           "turned off", "no layer", "waits for", "frame generation"};
    static const char* const memory[] = {"video memory", "MB is free", "MB of video memory", "not enough video memory",
                                         "Memory Guard"};
    for (const char* m : memory)
        if (why.find(m) != std::string::npos) return kMemory;
    for (const char* e : expected)
        if (why.find(e) != std::string::npos) return kExpected;
    return kFault;
}
// A reason's key: its numbers left out ("needs about 1194 MB at 3840x2160" and "... 1250 MB ..." are one reason).
inline std::string key_of(const std::string& why) {
    std::string k;
    k.reserve(why.size());
    for (size_t i = 0; i < why.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(why[i]))) {
            while (i + 1 < why.size() && (std::isdigit(static_cast<unsigned char>(why[i + 1])) || why[i + 1] == '.' ||
                                          why[i + 1] == 'x'))
                ++i;
            k += '#';
        } else {
            k += why[i];
        }
    }
    return k;
}

struct Reason {
    std::string key, example;   // example: the last full text
    Kind kind = kFault;
    uint64_t count = 0, first_ms = 0, last_ms = 0;
};
struct Layer {
    uint64_t asked = 0, ran = 0, passed = 0;
    std::vector<Reason> reasons;   // at most kReasons; more are counted under the last one, "other reasons"
    uint64_t streak_ms = 0;        // asked and not run since then (0: running)
    std::string streak_why;
    uint64_t longest_ms = 0, longest_end_ms = 0;
    std::string longest_why;
    uint64_t first_ms = 0;
};
constexpr int kLayers = 9;   // 0: outside an NR chain, 1..8 the chain's layers
constexpr size_t kReasons = 64;
struct Presents { uint64_t game = 0, shown = 0, dlssg = 0; };
struct Store {
    std::mutex m;
    Layer layers[kLayers];
    Presents presents;
    uint64_t since_ms = GetTickCount64();
    uint64_t logged_ms = GetTickCount64();
};
inline Store& store() { static Store s; return s; }

// One evaluation of layer `layer` (0 outside an NR chain): whether the network ran, and if not, why.
inline void evaluate(int layer, bool ran, const std::string& why) {
    Store& s = store();
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> g(s.m);
    Layer& l = s.layers[std::clamp(layer, 0, kLayers - 1)];
    if (!l.first_ms) l.first_ms = now;
    ++l.asked;
    if (ran) {
        ++l.ran;
        if (l.streak_ms) {
            if (now - l.streak_ms > l.longest_ms) {
                l.longest_ms = now - l.streak_ms;
                l.longest_end_ms = now;
                l.longest_why = l.streak_why;
            }
            l.streak_ms = 0;
        }
        return;
    }
    ++l.passed;
    const std::string text = why.empty() ? std::string("no reason recorded") : why;
    if (!l.streak_ms) { l.streak_ms = now; l.streak_why = text; }
    const std::string key = key_of(text);
    auto it = std::find_if(l.reasons.begin(), l.reasons.end(), [&](const Reason& r) { return r.key == key; });
    if (it == l.reasons.end()) {
        if (l.reasons.size() >= kReasons) {
            it = l.reasons.end() - 1;   // the last slot takes every further reason
            if (it->key != "(other reasons)") { it->key = "(other reasons)"; it->kind = kFault; }
        } else {
            l.reasons.push_back({key, text, kind_of(text), 0, now, now});
            it = l.reasons.end() - 1;
        }
    }
    ++it->count;
    it->last_ms = now;
    it->example = text;
}
inline void game_present() { Store& s = store(); std::lock_guard<std::mutex> g(s.m); ++s.presents.game; }
inline void shown_present() { Store& s = store(); std::lock_guard<std::mutex> g(s.m); ++s.presents.shown; }
inline void dlssg_frame() { Store& s = store(); std::lock_guard<std::mutex> g(s.m); ++s.presents.dlssg; }
inline void reset() {
    Store& s = store();
    std::lock_guard<std::mutex> g(s.m);
    for (Layer& l : s.layers) l = Layer{};
    s.presents = {};
    s.since_ms = GetTickCount64();
}

// A copy for drawing (the menu draws it every frame; the lock only while copying).
struct Snapshot {
    Layer layers[kLayers];
    Presents presents;
    uint64_t since_ms = 0, now_ms = 0;
};
inline Snapshot snapshot() {
    Store& s = store();
    Snapshot out;
    std::lock_guard<std::mutex> g(s.m);
    for (int i = 0; i < kLayers; ++i) out.layers[i] = s.layers[i];
    out.presents = s.presents;
    out.since_ms = s.since_ms;
    out.now_ms = GetTickCount64();
    return out;
}

inline std::string fmt(const char* f, double a = 0, double b = 0, double c = 0, double d = 0) {
    char t[256];
    std::snprintf(t, sizeof t, f, a, b, c, d);
    return t;
}
// The whole table as text (the log, the report): `top` reasons per layer, most frequent first.
inline std::string text(size_t top = 12) {
    const Snapshot s = snapshot();
    std::string t = fmt("NR run statistics, the last %.0f s:\n", double(s.now_ms - s.since_ms) / 1000.0);
    if (s.presents.game || s.presents.shown)
        t += fmt("  presents: %.0f of the game's own, %.0f frames shown (frame generation's included), %.0f game frames "
                 "whose layers ran at DLSS-G's evaluate\n",
                 double(s.presents.game), double(s.presents.shown), double(s.presents.dlssg));
    bool any = false;
    for (int i = 0; i < kLayers; ++i) {
        const Layer& l = s.layers[i];
        if (!l.asked) continue;
        any = true;
        const uint64_t streak = l.streak_ms ? s.now_ms - l.streak_ms : 0;
        t += (i ? fmt("  Layer %.0f: ", i) : std::string("  NR (no chain): ")) +
             fmt("asked %.0f, ran %.0f (%.1f %%), passed through %.0f", double(l.asked), double(l.ran),
                 100.0 * double(l.ran) / double(l.asked), double(l.passed)) + "\n";
        if (l.longest_ms || streak) {
            const bool now_longer = streak > l.longest_ms;
            t += fmt("    longest stretch without NR: %.1f s", double(now_longer ? streak : l.longest_ms) / 1000.0) +
                 (now_longer ? std::string(", still going") : fmt(", ended %.0f s ago", double(s.now_ms - l.longest_end_ms) / 1000.0)) +
                 ", began with: " + (now_longer ? l.streak_why : l.longest_why) + "\n";
        }
        std::vector<const Reason*> order;
        for (const Reason& r : l.reasons) order.push_back(&r);
        std::sort(order.begin(), order.end(), [](const Reason* a, const Reason* b) { return a->count > b->count; });
        for (size_t k = 0; k < order.size() && k < top; ++k) {
            const Reason& r = *order[k];
            t += fmt("    %8.0f x  [", double(r.count)) + kind_name(r.kind) +
                 fmt("] first %.0f s, last %.0f s ago: ", double(r.first_ms - s.since_ms) / 1000.0,
                     double(s.now_ms - r.last_ms) / 1000.0) + r.example + "\n";
        }
        if (order.size() > top) t += fmt("    (%.0f more reasons)\n", double(order.size() - top));
    }
    if (!any) t += "  no layer was asked to run\n";
    return t;
}
// Every 5 minutes: whether the log is due a short version (the caller logs it).
inline bool log_due() {
    Store& s = store();
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> g(s.m);
    if (now - s.logged_ms < 5 * 60 * 1000) return false;
    s.logged_ms = now;
    return true;
}

// The table into the log, line by line (`top` reasons a layer): every 5 minutes short (log_due), at the end in full.
inline void log_text(size_t top) {
    const std::string t = text(top);
    size_t at = 0;
    while (at < t.size()) {
        const size_t end = t.find('\n', at);
        nr::pe::log("[nr] %s", t.substr(at, end == std::string::npos ? std::string::npos : end - at).c_str());
        if (end == std::string::npos) break;
        at = end + 1;
    }
}

}  // namespace nr::pe::stats
