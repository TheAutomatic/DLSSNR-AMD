#pragma once
#include <atomic>
#include <any>
#include <string>
#include <cstdint>

namespace nr::frame_pair {
inline constexpr uint32_t failed_calls=UINT32_MAX;
// Only a Present being inspected gives its input a tag. A split stage publishes
// the tag its queued output actually contains, including cold passthrough.
struct Observation {
    inline static thread_local Observation* current = nullptr;
    inline static std::atomic<uint64_t> sequence{0};
    Observation* parent = current;
    uint64_t input, output;
    explicit Observation(bool enabled) : input(enabled ? sequence.fetch_add(1, std::memory_order_relaxed) + 1 : 0), output(input) {
        current = enabled ? this : nullptr;
    }
    ~Observation() { current = parent; }
    Observation(const Observation&) = delete;
    Observation& operator=(const Observation&) = delete;
};

// One Present may run multiple independent layers. Bind the age to that
// recording so a skipped layer or another swap chain cannot supply a stale age.
struct PresentAge {
    inline static thread_local PresentAge* active = nullptr;
    inline static std::atomic<uint64_t> serial{0};
    uint64_t id=serial.fetch_add(1,std::memory_order_relaxed)+1;
    PresentAge* parent = active;
    unsigned frames = 0, next_frames = 0;
    void (*publish)(unsigned,unsigned)=nullptr;
    bool terminal_chain = false;
    bool multiple = false;
    bool isolate_chain = false, isolated_layer = false;
    unsigned group_first=0, group_size=0, group_index=0;
    bool in_group=false, group_written=false, paired=false;
    std::any group_jobs;
    std::string group_reason;
    uint64_t version=0;
    float group_share=-1.f;
    bool interleaved_conflict=false;
    int net_override=-1;

    bool gathering() const { return in_group && group_size>1; }

    // A frame reuse group is one recording (its computed anchors, its network cut): it runs on one route. Where the D3D12
    // passthrough cannot serve a member, that member takes the Vulkan bridge (the route rule of every processing) and
    // so does the rest of its group from then on; the group goes back to the passthrough two Presents after the last
    // member that needed the bridge (each route change starts the stream afresh, as any route change does).
    inline static std::atomic<uint64_t> group_bridge_at{0};
    bool group_bridge() const {
        const uint64_t at = group_bridge_at.load(std::memory_order_relaxed);
        return gathering() && at && id >= at && id - at <= 2;
    }
    void need_bridge() const { if (gathering()) group_bridge_at.store(id, std::memory_order_relaxed); }

    static bool isolated() { return active && active->isolated_layer; }
    bool before_terminal = true;
    static bool terminal() { return active && active->terminal_chain && !active->before_terminal; }

    PresentAge() { active = this; }
    ~PresentAge() { active=parent; if(publish)publish(frames,next_frames); }
    static void record(unsigned n) { if (active && n) active->frames += n; }
};

struct Sources {
    uint64_t last = 0, older = 0;
    void show(bool held, bool two_sided) {
        Observation* observation = Observation::current;
        const uint64_t input = observation ? observation->input : 0;
        if (observation && held) observation->output = two_sided ? older : last;
        older = last;
        last = input;
    }
};

// Compare keeps a rolling window. A single screenshot needs only the input
// selected at its request, until that picture is shown or discarded at a cut.
struct History {
    uint64_t tags[3]{};
    unsigned next = 0, wait = 0, width = 0, height = 0, format = 0;
    bool comparing = false;
    int begin(uint32_t w, uint32_t h, uint32_t f, bool compare) {
        if (width != w || height != h || format != f || compare != comparing) {
            *this = {};
            width = w; height = h; format = f; comparing = compare;
        }
        if (compare) {
            const unsigned slot = next;
            next = (next + 1) % 3;
            return int(slot);
        }
        if (tags[0] && wait < 2) { ++wait; return -1; }
        wait = 0;
        return 0;
    }
    int find(uint64_t id) const {
        if (id) for (int i = 0; i < 3; ++i) if (tags[i] == id) return i;
        return -1;
    }
};
}
