#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include "nr_d3d12_passthrough_elf.hpp"
#include "nr_pe_d3d12split.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace nr::pe {
void log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    std::puts("");
    va_end(args);
}
int trace_execute() { return 0; }
}

namespace {
// COM ABI doubles: only the slots exercised by the real implementation are populated.
struct Object { void** vt; };
void* device_vt[44]{}, *native_vt[44]{}, *heap_vt[9]{}, *list_vt[60]{}, *queue_vt[19]{};
Object device{device_vt}, native{native_vt}, heap{heap_vt}, list{list_vt}, queue{queue_vt};
int wrapper_roots = 0, native_roots = 0, heap_releases = 0, native_releases = 0;
HRESULT heap_result = S_OK, get_device_result = S_OK, root_result = S_OK;
ULONG STDMETHODCALLTYPE release(Object* obj) {
    if (obj == &heap) ++heap_releases;
    if (obj == &native) ++native_releases;
    return 1;
}
HRESULT STDMETHODCALLTYPE query(Object*, REFIID, void** out) { *out = nullptr; return E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE get_device(Object* obj, REFIID, void** out) {
    *out = nullptr;
    if (obj == &heap && FAILED(get_device_result)) return get_device_result;
    *out = obj == &heap ? &native : &device;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE create_heap(Object*, const D3D12_HEAP_DESC* desc, REFIID, void** out) {
    *out = nullptr;
    if (FAILED(heap_result)) return heap_result;
    if (!desc->SizeInBytes || desc->Properties.Type != D3D12_HEAP_TYPE_DEFAULT) return E_INVALIDARG;
    *out = &heap;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE create_root(Object* obj, UINT, const void*, SIZE_T, REFIID, void** out) {
    if (obj == &device) ++wrapper_roots;
    else ++native_roots;
    *out = nullptr;
    if (FAILED(root_result)) return root_result;
    *out = &heap;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE create_queue(Object*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void** out) {
    *out = &queue;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE reset(Object*, ID3D12CommandAllocator*, ID3D12PipelineState*) { return S_OK; }
HRESULT STDMETHODCALLTYPE close(Object*) { return S_OK; }
void STDMETHODCALLTYPE noop() {}
int failures = 0;
void check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
template<class T> void* fn(T f) { return reinterpret_cast<void*>(f); }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (auto& slot : list_vt) slot = fn(noop);
    list_vt[0] = fn(query);
    list_vt[2] = fn(release);
    list_vt[7] = fn(get_device);
    list_vt[9] = fn(close);
    list_vt[10] = fn(reset);
    device_vt[12] = fn(noop);
    device_vt[2] = fn(release);
    device_vt[8] = fn(create_queue);
    device_vt[16] = fn(create_root);
    device_vt[28] = fn(create_heap);
    native_vt[2] = fn(release);
    native_vt[16] = fn(create_root);
    heap_vt[2] = fn(release);
    heap_vt[7] = fn(get_device);
    queue_vt[2] = fn(release);
    queue_vt[10] = fn(noop);
    auto* cmd = reinterpret_cast<ID3D12GraphicsCommandList*>(&list);
    std::string why;
    // All methods and vtables live in this executable, not d3d12*.dll or vkd3d.
    check(!nr::pe::split::prepare(cmd, &why) && why.find("next Reset") != std::string::npos,
          "wrapper list hooked, waiting for Reset");
    cmd->Reset(nullptr, nullptr);
    check(nr::pe::split::prepare(cmd, &why), "wrapper list prepare succeeds after Reset");
    check(queue_vt[10] != fn(noop), "wrapper queue patched");
    check(device_vt[12] != fn(noop), "wrapper device patched");

    namespace elf = nr::pe::d3d12_passthrough::elf;
    auto* dev = reinterpret_cast<ID3D12Device*>(&device);
    ID3D12RootSignature* rs = nullptr;
    check(elf::create_carrier_root_signature(dev, 1, &rs, &why) && rs,
          "carrier root signature created");
    check(wrapper_roots == 0 && native_roots == 1, "RenoDX wrapper root rewrite bypassed");
    check(heap_releases == 1 && native_releases == 1, "native probe references released");
    heap_result = E_OUTOFMEMORY;
    check(!elf::create_carrier_root_signature(dev, 1, &rs, &why) && !rs && why.find("CreateHeap") == 0,
          "heap failure returned without wrapper fallback");
    heap_result = S_OK;
    get_device_result = E_NOINTERFACE;
    check(!elf::create_carrier_root_signature(dev, 1, &rs, &why) && !rs && why.find("GetDevice") != std::string::npos,
          "GetDevice failure returned");
    check(heap_releases == 2, "heap released on GetDevice failure");
    get_device_result = S_OK;
    root_result = E_FAIL;
    check(!elf::create_carrier_root_signature(dev, 1, &rs, &why) && !rs && why.find("CreateRootSignature") == 0,
          "native root creation failure returned");
    check(heap_releases == 3 && native_releases == 2 && wrapper_roots == 0,
          "failure references released without wrapper fallback");
    std::printf("wrapper regression: %d failures\n", failures);
    return failures ? 1 : 0;
}
