#pragma once
#include <MinHook.h>
#include <array>
#include <mutex>
#include <utility>

namespace nr::hudcap {
// One immutable original per implementation address, not per device object.
// No map lookup, allocation or lock on the intercepted call's hot path.
template<class F> struct MethodHook;
template<class R, class... A> struct MethodHook<R(STDMETHODCALLTYPE*)(A...)> {
    using Fn=R(STDMETHODCALLTYPE*)(A...);
    using Handler=R(STDMETHODCALLTYPE*)(Fn,A...);
    struct Slot { void* target=nullptr; Fn original=nullptr; Handler handler=nullptr; };
    inline static std::array<Slot,16> slots{};
    inline static std::mutex mutex;
    template<size_t I> static R STDMETHODCALLTYPE call(A... args) {
        return slots[I].handler(slots[I].original,std::forward<A>(args)...);
    }
    template<size_t... I> static auto detours(std::index_sequence<I...>) {
        return std::array<Fn,sizeof...(I)>{&call<I>...};
    }
    static MH_STATUS install(void* object,int index,Handler handler) {
        void* target=(*reinterpret_cast<void***>(object))[index];
        std::lock_guard<std::mutex> lock(mutex);
        const auto calls=detours(std::make_index_sequence<16>{});
        for(size_t i=0;i<slots.size();++i) {
            auto& s=slots[i];
            if(s.target==target) return s.handler==handler ? MH_OK : MH_ERROR_ALREADY_CREATED;
            if(s.target) continue;
            s.handler=handler;
            auto status=MH_CreateHook(target,reinterpret_cast<void*>(calls[i]),reinterpret_cast<void**>(&s.original));
            if(status!=MH_OK) {s={};return status;}
            // Publish handler/original before enabling the entry point.
            status=MH_EnableHook(target);
            if(status!=MH_OK) {MH_RemoveHook(target);s={};return status;}
            s.target=target;
            return MH_OK;
        }
        return MH_ERROR_MEMORY_ALLOC; // bounded registry exhausted
    }
};
}
