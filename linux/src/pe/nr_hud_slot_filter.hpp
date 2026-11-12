#pragma once
// HUD FIX's descriptor bookkeeping (nr_asi_hudcap.cpp: the shader-resource views of textures that could hold a frame)
// runs on every view creation and descriptor copy of the game, HUD FIX on or off, on all of its recording threads.
// Almost none of those calls touch a tracked view. SlotFilter answers "no tracked view at any of these addresses"
// without a lock, so such a call returns at once; only a call that may touch one takes the map's lock.
//
// Counts per hashed bucket: a 4 KiB page of descriptor addresses, and a 16-byte granule. They are changed only under
// the map's exclusive lock, with every key the map gains or loses; they are read without a lock. A zero is exact (no
// key of the map hashes there, so none is in that page / granule); a non-zero only sends the call to the locked path.
// The game orders its own writes and copies of a descriptor (two threads writing or copying the same descriptor at
// once is not valid D3D12), so a relaxed read made after the game's own synchronisation sees every count it must.
//
// AddressBloom: a set of addresses that is only ever added to (a Bloom filter, two bits a key). A clear bit is exact
// (the key was never added); a set bit is "maybe". Used for "a UAV view of this resource was made".
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace nr::hudcap {

inline uint32_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return uint32_t(x);
}

struct SlotFilter {
    static constexpr unsigned kPageShift = 12, kGranuleShift = 4;
    static constexpr uint32_t kPages = 1u << 16, kGranules = 1u << 20;
    std::atomic<uint32_t> pages[kPages];
    std::atomic<uint32_t> granules[kGranules];
    static uint32_t page_of(uint64_t a) { return mix64(a >> kPageShift) & (kPages - 1); }
    static uint32_t granule_of(uint64_t a) { return mix64((a >> kGranuleShift) ^ 0x9e3779b97f4a7c15ull) & (kGranules - 1); }
    // (under the map's exclusive lock)
    void add(uint64_t a) {
        pages[page_of(a)].fetch_add(1, std::memory_order_relaxed);
        granules[granule_of(a)].fetch_add(1, std::memory_order_relaxed);
    }
    void remove(uint64_t a) {
        pages[page_of(a)].fetch_sub(1, std::memory_order_relaxed);
        granules[granule_of(a)].fetch_sub(1, std::memory_order_relaxed);
    }
    bool maybe_at(uint64_t a) const {
        return pages[page_of(a)].load(std::memory_order_relaxed) &&
               granules[granule_of(a)].load(std::memory_order_relaxed);
    }
    // a key of the map may be in [begin, begin + bytes) (byte addresses, any alignment)
    bool maybe_in(uint64_t begin, uint64_t bytes) const {
        if (!bytes) return false;
        const uint64_t last = begin + bytes - 1 < begin ? ~0ull : begin + bytes - 1;
        for (uint64_t page = begin >> kPageShift;; ++page) {
            if (pages[mix64(page) & (kPages - 1)].load(std::memory_order_relaxed)) {
                const uint64_t lo = page << kPageShift > begin ? page << kPageShift : begin;
                const uint64_t page_last = (page << kPageShift) + ((1ull << kPageShift) - 1);
                const uint64_t hi = page_last < last ? page_last : last;
                for (uint64_t g = lo >> kGranuleShift; g <= hi >> kGranuleShift; ++g)
                    if (granules[mix64(g ^ 0x9e3779b97f4a7c15ull) & (kGranules - 1)].load(std::memory_order_relaxed))
                        return true;
            }
            if (page == last >> kPageShift) return false;
        }
    }
};

struct AddressBloom {
    static constexpr uint32_t kWords = 1u << 16;   // 4 Mi bits
    std::atomic<uint64_t> words[kWords];
    static uint32_t bit(uint64_t key, uint64_t salt) { return mix64(key ^ salt) & (kWords * 64 - 1); }
    void add(uint64_t key) {
        for (const uint64_t salt : {0x2545f4914f6cdd1dull, 0x9e3779b97f4a7c15ull}) {
            const uint32_t b = bit(key, salt);
            std::atomic<uint64_t>& w = words[b >> 6];
            const uint64_t m = 1ull << (b & 63);
            if (!(w.load(std::memory_order_relaxed) & m)) w.fetch_or(m, std::memory_order_relaxed);
        }
    }
    bool maybe(uint64_t key) const {
        for (const uint64_t salt : {0x2545f4914f6cdd1dull, 0x9e3779b97f4a7c15ull}) {
            const uint32_t b = bit(key, salt);
            if (!(words[b >> 6].load(std::memory_order_relaxed) & (1ull << (b & 63)))) return false;
        }
        return true;
    }
};

// HUD FIX's views table: a CBV_SRV_UAV descriptor's CPU address -> the texture its view views (only the tracked kinds;
// nr_asi_hudcap.cpp says which). Sharded by 64 KiB of descriptor addresses (64 shards, a lock each: the game's
// recording threads write their own parts of its heaps and do not wait for each other), the filter in front (a call
// that touches no tracked view takes no lock at all). No call holds two shards' locks at once.
struct ViewTable {
    static constexpr unsigned kChunkShift = 12, kShards = 1024;
    struct alignas(64) Shard { std::shared_mutex m; std::map<uint64_t, uint64_t> v; };
    Shard shards[kShards];
    SlotFilter filter;
    Shard& shard(uint64_t a) { return shards[mix64(a >> kChunkShift) & (kShards - 1)]; }
    // [begin, begin + bytes) cut at the chunks' bounds: f(lo, hi) in rising order
    template <class F> static void chunks(uint64_t begin, uint64_t bytes, F f) {
        if (!bytes) return;
        const uint64_t end = begin + bytes < begin ? ~0ull : begin + bytes;
        for (uint64_t lo = begin; lo < end;) {
            const uint64_t next = ((lo >> kChunkShift) + 1) << kChunkShift;
            const uint64_t hi = next > lo && next < end ? next : end;
            f(lo, hi);
            if (hi == end) break;
            lo = hi;
        }
    }
    void set(uint64_t at, uint64_t res) {
        Shard& s = shard(at);
        std::unique_lock<std::shared_mutex> lk(s.m);
        const auto in = s.v.try_emplace(at, res);
        if (in.second) filter.add(at);
        else in.first->second = res;
    }
    void erase(uint64_t at) {
        if (!filter.maybe_at(at)) return;
        Shard& s = shard(at);
        std::unique_lock<std::shared_mutex> lk(s.m);
        if (s.v.erase(at)) filter.remove(at);
    }
    // every view forgotten (HUD FIX switched off: the bookkeeping stops; what the table held goes stale)
    void clear() {
        for (Shard& s : shards) {
            std::unique_lock<std::shared_mutex> lk(s.m);
            for (const auto& kv : s.v) filter.remove(kv.first);
            s.v.clear();
        }
    }
    // the tracked views in [begin, end): f(at, res), in rising order of `at`
    template <class F> void each(uint64_t begin, uint64_t end, F f) {
        if (end <= begin) return;
        chunks(begin, end - begin, [&](uint64_t lo, uint64_t hi) {
            if (!filter.maybe_in(lo, hi - lo)) return;
            Shard& s = shard(lo);
            std::shared_lock<std::shared_mutex> lk(s.m);
            for (auto it = s.v.lower_bound(lo); it != s.v.end() && it->first < hi; ++it) f(it->first, it->second);
        });
    }
    // CopyDescriptors' bytes [src, src + bytes) onto [dst, ...): the source's tracked views read first (a snapshot: an
    // overlapping copy reads the source as it was), then the destination's replaced; on_copy(at, from, res) a view.
    template <class F> void copy(uint64_t dst, uint64_t src, uint64_t bytes, F on_copy) {
        if (!bytes || dst == src) return;
        const bool from_any = filter.maybe_in(src, bytes);
        if (!from_any && !filter.maybe_in(dst, bytes)) return;
        thread_local std::vector<std::pair<uint64_t, uint64_t>> snap;   // (offset, res), rising
        thread_local std::vector<std::map<uint64_t, uint64_t>::node_type> nodes;
        snap.clear();
        if (from_any) each(src, src + bytes, [&](uint64_t at, uint64_t res) { snap.push_back({at - src, res}); });
        size_t k = 0;
        chunks(dst, bytes, [&](uint64_t lo, uint64_t hi) {
            const size_t first = k;
            while (k < snap.size() && dst + snap[k].first < hi) ++k;
            if (first == k && !filter.maybe_in(lo, hi - lo)) return;
            Shard& s = shard(lo);
            std::unique_lock<std::shared_mutex> lk(s.m);
            for (auto it = s.v.lower_bound(lo); it != s.v.end() && it->first < hi;) {   // (their nodes reused below)
                filter.remove(it->first);
                nodes.push_back(s.v.extract(it++));
            }
            auto hint = s.v.lower_bound(lo);
            for (size_t j = first; j < k; ++j) {
                const uint64_t at = dst + snap[j].first;
                if (!nodes.empty()) {
                    auto node = std::move(nodes.back());
                    nodes.pop_back();
                    node.key() = at;
                    node.mapped() = snap[j].second;
                    hint = s.v.insert(hint, std::move(node));
                } else {
                    hint = s.v.emplace_hint(hint, at, snap[j].second);
                }
                ++hint;
                filter.add(at);
                on_copy(at, src + snap[j].first, snap[j].second);
            }
            nodes.clear();
        });
        if (snap.capacity() > 65536) { snap.clear(); snap.shrink_to_fit(); }
    }
};

}  // namespace nr::hudcap
