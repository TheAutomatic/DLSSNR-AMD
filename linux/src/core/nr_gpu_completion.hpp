#pragma once
// A recording may be submitted repeatedly. Its references survive both GPU use and the recording itself.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nr::gpu {
// Recordings handed to a queue so far (with NR work in them), for the fallback below.
inline std::atomic<uint64_t> submissions{0};
// A recording with no completion proof (an unknown submit, a receipt that never answers, a list never reset) is taken
// as done by the rule the retirement used before the proofs (0.0.7 / b3i): 64 later submissions AND 2 s since this one
// was last submitted (or made). The proof frees earlier; without one nothing waits forever.
inline constexpr uint64_t kFallbackSubmissions = 64;
inline constexpr std::chrono::milliseconds kFallbackTime{2000};
struct Recording {
    std::mutex mutex;
    bool sealed = false;
    bool unknown_submit = false;
    bool ever_submitted = false;   // (ran_ready: at least one submission known)
    std::chrono::steady_clock::time_point touched = std::chrono::steady_clock::now();
    uint64_t touched_at = submissions.load();
    bool fallback_due() const {
        return submissions.load() - touched_at >= kFallbackSubmissions &&
               std::chrono::steady_clock::now() - touched >= kFallbackTime;
    }
    std::vector<std::function<bool()>> waits;
    std::vector<std::shared_ptr<Recording>> children;
    std::vector<std::weak_ptr<Recording>> parents;
    bool ready() {
        std::vector<std::shared_ptr<Recording>> kept;
        bool due = false;
        {
            std::lock_guard<std::mutex> guard(mutex);
            waits.erase(std::remove_if(waits.begin(), waits.end(), [](auto& f) { return f(); }), waits.end());
            due = fallback_due();
            if (!sealed || unknown_submit || !waits.empty()) return due;
            for (const auto& p : parents) if (auto owner = p.lock()) kept.push_back(std::move(owner));
        }
        // A secondary can be reset while an executable primary still retains its earlier recording.
        return due || std::all_of(kept.begin(), kept.end(), [](const auto& p) { return p->ready(); });
    }
    // HUD capture cannot treat elapsed time/submission count as a resource lifetime proof.
    bool capture_ready() {
        std::vector<std::shared_ptr<Recording>> owners;
        {
            std::lock_guard<std::mutex> guard(mutex);
            waits.erase(std::remove_if(waits.begin(), waits.end(), [](auto& f) { return f(); }), waits.end());
            if (!sealed || unknown_submit || !waits.empty()) return false;
            for (const auto& p:parents) if (auto owner=p.lock()) owners.push_back(std::move(owner));
        }
        return std::all_of(owners.begin(),owners.end(),[](const auto& p){return p->capture_ready();});
    }
    // Every execution of it known so far has finished on the GPU (at least one; none unknown) - also while it is not
    // sealed. HUD FIX's chain snapshots: a game that keeps its closed command lists without Reset never
    // seals them, so capture_ready() never came and each copy made a new back-buffer-size image (23 MB at 3200x1800,
    // ~190 MB/s) until RAM ran out and the frame rate collapsed. Only a later re-execution of the same closed list is not
    // covered.
    bool ran_ready() {
        std::lock_guard<std::mutex> guard(mutex);
        waits.erase(std::remove_if(waits.begin(), waits.end(), [](auto& f) { return f(); }), waits.end());
        return ever_submitted && !unknown_submit && waits.empty();
    }
    void seal() { std::lock_guard<std::mutex> guard(mutex); sealed = true; }
    void submitted(std::function<bool()> wait) {
        std::vector<std::shared_ptr<Recording>> kept;
        {
            std::lock_guard<std::mutex> guard(mutex);
            waits.erase(std::remove_if(waits.begin(), waits.end(), [](auto& f) { return f(); }), waits.end());
            kept = children;
            ever_submitted = true;
            if (wait) waits.push_back(wait); else unknown_submit = true;
            touched = std::chrono::steady_clock::now();
            touched_at = ++submissions;
        }
        for (const auto& child : kept) child->submitted(wait);
    }
};
using Ticket = std::shared_ptr<Recording>;
inline std::mutex registry_mutex;
inline std::unordered_map<const void*, Ticket> recordings;
inline Ticket& current() { static thread_local Ticket t; return t; }
inline Ticket recording(const void* key) {
    std::lock_guard<std::mutex> guard(registry_mutex);
    auto& t = recordings[key];
    if (!t) t = std::make_shared<Recording>();
    return t;
}
inline Ticket find(const void* key) {
    std::lock_guard<std::mutex> guard(registry_mutex);
    const auto it = recordings.find(key);
    return it == recordings.end() ? Ticket{} : it->second;
}
inline void reset(const void* key) {
    std::lock_guard<std::mutex> guard(registry_mutex);
    auto it = recordings.find(key);
    if (it == recordings.end()) return;
    it->second->seal();
    recordings.erase(it);
}
inline void forget(const void* key, const Ticket& ticket) {
    ticket->seal();
    std::lock_guard<std::mutex> guard(registry_mutex);
    const auto i = recordings.find(key);
    if (i != recordings.end() && i->second == ticket) recordings.erase(i);
}
inline void inherit(const void* parent, const void* child) {
    if (const auto c = find(child)) {
        auto p = recording(parent);
        if (p == c) return;
        std::scoped_lock guard(p->mutex, c->mutex);
        if (std::find(p->children.begin(), p->children.end(), c) == p->children.end()) {
            p->children.push_back(c); c->parents.push_back(p);
        }
    }
}
struct Scope {
    Ticket previous;
    explicit Scope(Ticket t) : previous(std::move(current())) { current() = std::move(t); }
    ~Scope() { current() = std::move(previous); }
};
inline bool ready(const Ticket& t) { return !t || t->ready(); }
template<class T> T& slot(std::vector<T>& ring, size_t index) {
    if (!ring[index].gpu || ring[index].gpu->ready()) return ring[index];
    for (auto& s : ring) if (s.gpu && s.gpu->ready()) return s;
    // Recordings still pending (their fallback is at most 2 s away): the ring grows. At 8192 the slot in turn is taken
    // as the ring took it before the proofs, rather than refusing NR.
    if (ring.size() >= 8192) return ring[index];
    ring.emplace_back();
    return ring.back();
}
struct Uses {
    std::vector<Ticket> tickets;
    void touch() {
        tickets.erase(std::remove_if(tickets.begin(), tickets.end(), [](const Ticket& t) { return t->ready(); }), tickets.end());
        if (current() && std::find(tickets.begin(), tickets.end(), current()) == tickets.end()) tickets.push_back(current());
    }
    bool ready() const {
        return std::all_of(tickets.begin(), tickets.end(), [](const Ticket& t) { return t->ready(); });
    }
};
} // namespace nr::gpu
