#pragma once
// Diagnostic CPU observation order, never a GPU dependency or HUD-free proof.
// Fixed capacity, explicit loss count, freeze before export. No API resource ownership.
#include <cstdint>
#include <mutex>
#include <vector>
namespace nr::hudtrace {
struct Event {
    uint64_t seq=0, frame=0, list=0, generation=0, a=0, b=0, c=0, d=0;
    uint32_t thread=0;
    const char* kind=""; // static strings only
};
class Ring {
    std::mutex mutex_;
    std::vector<Event> data_;
    uint64_t count_=0;
    bool frozen_=false;
public:
    explicit Ring(size_t capacity) : data_(capacity ? capacity : 1) {}
    void add(Event e) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frozen_) return;
        e.seq=++count_;
        data_[(count_-1)%data_.size()]=e;
    }
    std::vector<Event> freeze(uint64_t& lost) {
        std::lock_guard<std::mutex> lock(mutex_);
        frozen_=true;
        const uint64_t size=count_ < data_.size() ? count_ : data_.size();
        lost=count_-size;
        std::vector<Event> result;
        result.reserve(size);
        for (uint64_t i=count_-size;i<count_;++i) result.push_back(data_[i%data_.size()]);
        return result;
    }
};
} // namespace nr::hudtrace
