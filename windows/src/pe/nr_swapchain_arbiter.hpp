#pragma once
// Arbitrates primary DXGI swap-chain ownership when standalone hosts present multiple windows.
// Prefers the largest active visible window (ignoring tiny <64x64 helpers) and breaks near-equal
// area ties by foreground focus while keeping an active winner sticky when unfocused.

#include <windows.h>
#include <dxgi.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace nr::pe {

class SwapChainArbiter {
public:
    static constexpr uint64_t kActiveWindowMs = 600;
    static constexpr uint32_t kMinMeaningfulDim = 64;
    static constexpr size_t kMaxEntries = 16;

    struct ChangeInfo {
        bool changed = false;
        size_t active_count = 0;
        IDXGISwapChain* sc = nullptr;
        HWND hwnd = nullptr;
        uint32_t w = 0;
        uint32_t h = 0;
    };

    template <typename AliveFn, typename VisibleFn, typename FocusedFn>
    bool observe(IDXGISwapChain* sc, HWND hwnd, uint32_t w, uint32_t h, uint64_t now_ms,
                 AliveFn&& alive_fn, VisibleFn&& visible_fn, FocusedFn&& focused_fn,
                 ChangeInfo* info = nullptr) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!sc || !w || !h) {
            if (info) *info = {};
            return true;
        }
        const uintptr_t key = hwnd ? uintptr_t(hwnd) : uintptr_t(sc);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->key != key &&
                ((now_ms > it->last_ms && now_ms - it->last_ms > kActiveWindowMs) ||
                 (it->hwnd && !alive_fn(it->hwnd)))) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }

        const bool self_visible = visible_fn(hwnd);
        Entry* found = nullptr;
        for (Entry& e : entries_) {
            if (e.key == key) {
                e.sc = sc;
                e.hwnd = hwnd;
                e.w = w;
                e.h = h;
                e.visible = self_visible;
                e.last_ms = now_ms;
                found = &e;
            } else {
                e.visible = visible_fn(e.hwnd);
            }
        }
        if (!found) {
            if (entries_.size() >= kMaxEntries) {
                auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                               [](const Entry& a, const Entry& b) { return a.last_ms < b.last_ms; });
                if (oldest != entries_.end()) entries_.erase(oldest);
            }
            entries_.push_back(Entry{key, sc, hwnd, w, h, self_visible, now_ms});
        }

        if (entries_.size() == 1u) {
            selected_key_ = key;
            if (info) *info = ChangeInfo{false, 1u, sc, hwnd, w, h};
            return true;
        }

        bool any_meaningful = false;
        for (const Entry& e : entries_) {
            if (e.w >= kMinMeaningfulDim && e.h >= kMinMeaningfulDim) {
                any_meaningful = true;
                break;
            }
        }
        auto match_meaningful = [&](const Entry& e) {
            return !any_meaningful || (e.w >= kMinMeaningfulDim && e.h >= kMinMeaningfulDim);
        };

        bool any_visible = false;
        for (const Entry& e : entries_) {
            if (match_meaningful(e) && e.visible) {
                any_visible = true;
                break;
            }
        }
        auto qualifies = [&](const Entry& e) {
            return match_meaningful(e) && (!any_visible || e.visible);
        };

        uint64_t max_area = 0;
        for (const Entry& e : entries_) {
            if (!qualifies(e)) continue;
            const uint64_t area = uint64_t(e.w) * uint64_t(e.h);
            if (area > max_area) max_area = area;
        }

        const Entry* focused_best = nullptr;
        const Entry* sticky_best = nullptr;
        const Entry* area_best = nullptr;
        for (const Entry& e : entries_) {
            if (!qualifies(e)) continue;
            const uint64_t area = uint64_t(e.w) * uint64_t(e.h);
            if (area * 100ull < max_area * 95ull) continue;

            if (focused_fn(e.hwnd)) {
                const uint64_t fb_area = focused_best ? uint64_t(focused_best->w) * uint64_t(focused_best->h) : 0;
                if (!focused_best || area > fb_area ||
                    (area == fb_area && (e.key == selected_key_ ||
                                         (focused_best->key != selected_key_ && e.w > focused_best->w)))) {
                    focused_best = &e;
                }
            }
            if (e.key == selected_key_) sticky_best = &e;
            const uint64_t ab_area = area_best ? uint64_t(area_best->w) * uint64_t(area_best->h) : 0;
            if (!area_best || area > ab_area ||
                (area == ab_area && (e.key == selected_key_ ||
                                     (area_best->key != selected_key_ && e.w > area_best->w)))) {
                area_best = &e;
            }
        }

        const Entry* best = focused_best ? focused_best : sticky_best ? sticky_best : area_best;
        if (!best) best = &entries_.front();
        const bool switched = (best->key != selected_key_);
        selected_key_ = best->key;
        if (info) *info = ChangeInfo{switched, entries_.size(), best->sc, best->hwnd, best->w, best->h};
        return key == selected_key_;
    }

    bool is_primary(IDXGISwapChain* sc, ChangeInfo* info = nullptr) {
        if (!sc) {
            if (info) *info = {};
            return true;
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(sc->GetDesc(&desc))) {
            if (info) *info = {};
            return true;
        }
        const HWND fg = GetForegroundWindow();
        auto alive_fn = [](HWND h) { return !h || IsWindow(h) != FALSE; };
        auto visible_fn = [](HWND h) {
            if (!h || !IsWindow(h)) return false;
            HWND root = GetAncestor(h, GA_ROOT);
            if (!root) root = h;
            return IsWindowVisible(h) && !IsIconic(root);
        };
        auto focused_fn = [fg](HWND h) {
            if (!h || !fg) return false;
            HWND root = GetAncestor(h, GA_ROOT);
            if (!root) root = h;
            return h == fg || root == fg;
        };
        return observe(sc, desc.OutputWindow, desc.BufferDesc.Width, desc.BufferDesc.Height, GetTickCount64(),
                       alive_fn, visible_fn, focused_fn, info);
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mu_);
        entries_.clear();
        selected_key_ = 0;
    }

private:
    struct Entry {
        uintptr_t key = 0;
        IDXGISwapChain* sc = nullptr;
        HWND hwnd = nullptr;
        uint32_t w = 0;
        uint32_t h = 0;
        bool visible = false;
        uint64_t last_ms = 0;
    };

    std::mutex mu_;
    std::vector<Entry> entries_;
    uintptr_t selected_key_ = 0;
};

}  // namespace nr::pe
