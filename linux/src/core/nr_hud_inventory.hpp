#pragma once
// Metadata-only FG input inventory. No resource ownership or GPU work.
#include <array>
#include <cstdint>
#include <mutex>

namespace nr::hudcap {
class InputInventory {
public:
    struct Count { uint64_t calls = 0, populated = 0; bool overflow = false; };
    // First three observations per provider/type, then at most one per ten seconds.
    // Counts include suppressed calls. Fixed storage also bounds unknown tag types.
    bool sample(uint32_t provider, uint64_t type, bool populated, uint64_t ms, Count& count) {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot* slot = nullptr;
        for (auto& s : slots_)
            if (s.count.calls && s.provider == provider && s.type == type) { slot = &s; break; }
        if (!slot) for (auto& s : slots_) if (!s.count.calls) {
            slot = &s; s.provider = provider; s.type = type; break;
        }
        if (!slot) { slot = &overflow_; slot->count.overflow = true; }
        ++slot->count.calls;
        slot->count.populated += populated ? 1 : 0;
        count = slot->count;
        if (count.calls > 3 && (ms < slot->last || ms - slot->last < 10000)) return false;
        slot->last = ms;
        return true;
    }
private:
    struct Slot { uint32_t provider = 0; uint64_t type = 0, last = 0; Count count; };
    std::array<Slot, 64> slots_{};
    Slot overflow_{};
    std::mutex mutex_;
};
} // namespace nr::hudcap
