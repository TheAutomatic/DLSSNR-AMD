#pragma once
// The NR screenshot's D3D12 pair (RenoDX's F5): two READBACK buffers, the copies into them and the pair handed to the
// screenshot worker (nr_screenshot.hpp). Two users, which differ only in how they learn that the GPU ran the copies:
//   - the Present layers (nr_asi_main.cpp PresentShot): our own list, signalled on our own fence;
//   - the D3D12 passthrough (nr_d3d12_passthrough_backend.cpp): the game's list, finished when its recording ticket is.
// A buffer the GPU may still write is never released here: that is the caller's decision.
#include "nr_pe_interop.hpp"   // vulkan_format_of
#include "nr_screenshot.hpp"
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <utility>

namespace nr::pe::d3d12shot {

struct Readback {
    ID3D12Resource* rb[2] = {};               // 0: before NR, 1: after NR
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    uint32_t w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    // `desc`: the source texture's description with Width / Height set to the region read (from texel 0, 0). False:
    // the request has already ended (fail / abandon) with its reason.
    bool prepare(ID3D12Device* device, D3D12_RESOURCE_DESC desc) {
        if (!nr::pe::screenshot::readable(nr::pe::vulkan_format_of(desc.Format))) {
            nr::pe::screenshot::fail("unsupported_present", "the frame's format cannot be read back");
            return false;
        }
        desc.MipLevels = 1;
        desc.DepthOrArraySize = 1;
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (auto*& r : rb)
            if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&r))))
                r = nullptr;
        if (!rb[0] || !rb[1]) {
            release();
            nr::pe::screenshot::abandon("allocation_failed", "the readback buffers could not be allocated");
            return false;
        }
        // The worker's "unwritten" check: a pair read before the copies ran still carries this stamp.
        for (ID3D12Resource* r : rb) {
            void* m = nullptr;
            const D3D12_RANGE none{0, 0};
            if (SUCCEEDED(r->Map(0, &none, &m)) && m) {
                nr::pe::screenshot::stamp(m, total);
                const D3D12_RANGE written{0, SIZE_T(total)};
                r->Unmap(0, &written);
            }
        }
        w = uint32_t(desc.Width); h = desc.Height; format = desc.Format;
        return true;
    }

    // `src` is in `state` before and after (the copy moves it to COPY_SOURCE and back); only the prepared region.
    void copy(ID3D12GraphicsCommandList* l, ID3D12Resource* src, D3D12_RESOURCE_STATES state, int i) const {
        if (!rb[i]) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) l->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{}, from{};
        dst.pResource = rb[i]; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        from.pResource = src; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{0, 0, 0, w, h, 1};
        l->CopyTextureRegion(&dst, 0, 0, 0, &from, &box);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) l->ResourceBarrier(1, &b);
    }

    // The GPU has run both copies: the pair goes to the worker, which releases the buffers once it has read them.
    // `hdr`: 0 SDR, 1 scRGB / linear (1.0 = `unit_nits`), 2 PQ. False: mapping failed (the buffers stay here).
    bool deliver(uint8_t hdr, std::string meta, float unit_nits = 80.0f) {
        if (!rb[0] || !rb[1]) return false;
        void* m[2] = {};
        const D3D12_RANGE all{0, SIZE_T(fp.Footprint.RowPitch) * h};
        if (FAILED(rb[0]->Map(0, &all, &m[0]))) return false;
        if (FAILED(rb[1]->Map(0, &all, &m[1]))) { rb[0]->Unmap(0, nullptr); return false; }
        nr::pe::screenshot::Pair pair;
        pair.width = w; pair.height = h; pair.row_pitch = fp.Footprint.RowPitch;
        pair.format = nr::pe::vulkan_format_of(format);
        pair.hdr_mode = hdr; pair.linear_unit_nits = unit_nits;
        pair.pre = m[0]; pair.post = m[1];
        pair.bytes = uint64_t(fp.Footprint.RowPitch) * h;
        pair.meta = std::move(meta);
        struct Owner { ID3D12Resource* rb[2]; };
        pair.owner = new Owner{{rb[0], rb[1]}};
        pair.release = [](void* o) {
            auto* own = static_cast<Owner*>(o);
            for (ID3D12Resource* r : own->rb) { r->Unmap(0, nullptr); r->Release(); }
            delete own;
        };
        rb[0] = rb[1] = nullptr;
        nr::pe::screenshot::deliver(std::move(pair));
        return true;
    }

    // Only when no recorded copy can still write them (never executed, or finished).
    void release() {
        for (auto*& r : rb) if (r) { r->Release(); r = nullptr; }
    }
    // Kept for good: the GPU may still write them (no proof the copies finished).
    void forget() { rb[0] = rb[1] = nullptr; }
};

}  // namespace nr::pe::d3d12shot
