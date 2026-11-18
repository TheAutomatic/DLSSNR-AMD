#pragma once
#include <MinHook.h>
#include <array>
#include <atomic>
#include <mutex>
#include <utility>

namespace nr::hudcap {
// One immutable original per implementation address, with up to kMaxStages chained detours per slot.
// No map lookup, allocation or lock on the intercepted call's hot path.
template<class F> struct MethodHook;
template<class R, class... A> struct MethodHook<R(STDMETHODCALLTYPE*)(A...)> {
    using Fn = R(STDMETHODCALLTYPE*)(A...);
    using Handler = R(STDMETHODCALLTYPE*)(Fn, A...);
    static constexpr size_t kMaxSlots = 16;
    static constexpr size_t kMaxStages = 4;

    struct Stage {
        Handler handler = nullptr;
        Fn raw = nullptr;
    };
    struct Slot {
        void* target = nullptr;
        Fn original = nullptr;
        std::atomic<size_t> depth{0};
        std::array<Stage, kMaxStages> stages{};
    };

    inline static std::array<Slot, kMaxSlots> slots{};
    inline static std::mutex mutex;

    template<size_t I, size_t D> static R STDMETHODCALLTYPE stage_thunk(A... args) {
        const auto& s = slots[I];
        const auto& st = s.stages[D];
        if (st.handler) {
            const Fn next = (D == 0) ? s.original : &stage_thunk<I, (D > 0 ? D - 1 : 0)>;
            return st.handler(next, std::forward<A>(args)...);
        }
        return st.raw(std::forward<A>(args)...);
    }

    template<size_t I> static R STDMETHODCALLTYPE call(A... args) {
        switch (slots[I].depth.load(std::memory_order_acquire)) {
        case 4: return stage_thunk<I, 3>(std::forward<A>(args)...);
        case 3: return stage_thunk<I, 2>(std::forward<A>(args)...);
        case 2: return stage_thunk<I, 1>(std::forward<A>(args)...);
        case 1: return stage_thunk<I, 0>(std::forward<A>(args)...);
        default: return slots[I].original(std::forward<A>(args)...);
        }
    }

    template<size_t... I> static constexpr auto entry_detours(std::index_sequence<I...>) {
        return std::array<Fn, sizeof...(I)>{&call<I>...};
    }
    template<size_t I> static constexpr std::array<Fn, kMaxStages> stage_fns() {
        return {&stage_thunk<I, 0>, &stage_thunk<I, 1>, &stage_thunk<I, 2>, &stage_thunk<I, 3>};
    }
    template<size_t... I> static constexpr auto all_stage_fns(std::index_sequence<I...>) {
        return std::array<std::array<Fn, kMaxStages>, sizeof...(I)>{stage_fns<I>()...};
    }

    static MH_STATUS ensure_init() {
        const MH_STATUS st = MH_Initialize();
        return (st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED) ? MH_OK : st;
    }

    static MH_STATUS install(void* object, int index, Handler handler) {
        if (!object) return MH_ERROR_NOT_CREATED;
        void* target = (*reinterpret_cast<void***>(object))[index];
        return install_at(target, handler, nullptr, nullptr);
    }

    static MH_STATUS install_raw(void* target, Fn detour, Fn* out_next) {
        return install_at(target, nullptr, detour, out_next);
    }

    static MH_STATUS install_raw_method(void* object, int index, Fn detour, Fn* out_next) {
        if (!object) return MH_ERROR_NOT_CREATED;
        void* target = (*reinterpret_cast<void***>(object))[index];
        return install_at(target, nullptr, detour, out_next);
    }

    static MH_STATUS remove_raw(void* target, Fn detour, Fn* out_next) {
        if (out_next) *out_next = nullptr;
        if (!target || !detour) return MH_OK;
        std::lock_guard<std::mutex> lock(mutex);
        for (Slot& s : slots) {
            if (s.target != target) continue;
            const size_t d = s.depth.load(std::memory_order_relaxed);
            for (size_t k = 0; k < d; ++k) {
                if (s.stages[k].raw != detour) continue;
                for (size_t j = k; j + 1 < d; ++j) s.stages[j] = s.stages[j + 1];
                s.stages[d - 1] = {};
                s.depth.store(d - 1, std::memory_order_release);
                if (d == 1) {
                    MH_DisableHook(target);
                    MH_RemoveHook(target);
                    s.target = nullptr;
                    s.original = nullptr;
                }
                return MH_OK;
            }
        }
        return MH_OK;
    }

private:
    static MH_STATUS install_at(void* target, Handler handler, Fn detour, Fn* out_next) {
        if (!target || (!handler && !detour)) {
            if (out_next) *out_next = nullptr;
            return MH_ERROR_NOT_CREATED;
        }
        const MH_STATUS init = ensure_init();
        if (init != MH_OK) {
            if (out_next) *out_next = nullptr;
            return init;
        }

        std::lock_guard<std::mutex> lock(mutex);
        constexpr auto entries = entry_detours(std::make_index_sequence<kMaxSlots>{});
        constexpr auto stages_tbl = all_stage_fns(std::make_index_sequence<kMaxSlots>{});

        for (size_t i = 0; i < slots.size(); ++i) {
            Slot& s = slots[i];
            if (s.target != target) continue;
            const size_t d = s.depth.load(std::memory_order_relaxed);
            for (size_t k = 0; k < d; ++k) {
                if (handler && s.stages[k].handler == handler) return MH_OK;
                if (detour && s.stages[k].raw == detour) {
                    if (out_next && !*out_next) *out_next = (k == 0) ? s.original : stages_tbl[i][k - 1];
                    return MH_OK;
                }
            }
            if (d >= kMaxStages) {
                if (out_next) *out_next = nullptr;
                return MH_ERROR_MEMORY_ALLOC;
            }
            const Fn next_fn = (d == 0) ? s.original : stages_tbl[i][d - 1];
            if (out_next) *out_next = next_fn;
            s.stages[d] = Stage{handler, detour};
            s.depth.store(d + 1, std::memory_order_release);
            return MH_OK;
        }

        for (size_t i = 0; i < slots.size(); ++i) {
            Slot& s = slots[i];
            if (s.target) continue;
            s.stages[0] = Stage{handler, detour};
            MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(entries[i]),
                                             reinterpret_cast<void**>(&s.original));
            if (status != MH_OK) {
                s.stages[0] = {};
                if (out_next) *out_next = nullptr;
                return status;
            }
            if (out_next) *out_next = s.original;
            s.depth.store(1, std::memory_order_release);
            status = MH_EnableHook(target);
            if (status != MH_OK) {
                MH_RemoveHook(target);
                s.depth.store(0, std::memory_order_release);
                s.stages[0] = {};
                s.original = nullptr;
                if (out_next) *out_next = nullptr;
                return status;
            }
            s.target = target;
            return MH_OK;
        }
        if (out_next) *out_next = nullptr;
        return MH_ERROR_MEMORY_ALLOC;
    }
};
}
