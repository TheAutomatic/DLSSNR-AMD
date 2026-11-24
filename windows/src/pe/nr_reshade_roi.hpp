#pragma once

#include "nr_path_win.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_roi.hpp"
#include "nr_pe_session.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <d3d11.h>

namespace nr::pe::roi {

struct PresentRow {
    uint64_t present{};
    char time[sizeof(LocalTime::text)]{};
    Luma stage[stages + 1]{};
};

class Csv {
  public:
    static Csv* open() {
        const std::string log_file = log_path();
        const std::string path = log_file.substr(0, log_file.find_last_of("\\/") + 1) + "dlssnr-amd-roi.csv";
        std::unique_ptr<Csv> csv(new (std::nothrow) Csv);
        if (!csv) return nullptr;
        csv->file_ = nr::u8::fopen(path.c_str(), "w");
        if (!csv->file_) {
            log("[nr] NR_DIAG_ROI=1: could not open '%s'", path.c_str());
            return nullptr;
        }
        HMODULE module{};
        // Like the screenshot worker, the opt-in writer lives until process exit, never joined under the loader lock.
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&write_loop), &module)) {
            if (HANDLE thread = CreateThread(nullptr, 0, write_loop, csv.get(), 0, nullptr)) {
                CloseHandle(thread);
                const auto pos = configured_percent();
                log("[nr] NR_DIAG_ROI=1 active (native D3D11 RenoDX, ROI %ux%u at %u%%,%u%%): writing '%s'",
                    side, side, pos.x, pos.y, path.c_str());
                return csv.release();
            }
            FreeLibrary(module);
        }
        std::fclose(csv->file_);
        log("[nr] NR_DIAG_ROI=1: could not start the CSV writer");
        return nullptr;
    }

    void push(const PresentRow& row) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_.load(std::memory_order_acquire) == rows_.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        rows_[tail % rows_.size()] = row;
        tail_.store(tail + 1, std::memory_order_release);
    }

  private:
    static DWORD WINAPI write_loop(void* arg) {
        auto& csv = *static_cast<Csv*>(arg);
        std::fputs("time,present,nr_input_mean,nr_input_sd,network_output_mean,network_output_sd,"
                   "renodx_output_mean,renodx_output_sd,back_buffer_mean,back_buffer_sd\n", csv.file_);
        std::fflush(csv.file_);
        for (;;) {
            Sleep(100);
            const size_t begin = csv.head_.load(std::memory_order_relaxed);
            const size_t end = csv.tail_.load(std::memory_order_acquire);
            for (size_t i = begin; i != end; ++i) csv.write(csv.rows_[i % csv.rows_.size()]);
            csv.head_.store(end, std::memory_order_release);
            if (begin != end && std::fflush(csv.file_) != 0 && !csv.write_failed_) {
                csv.write_failed_ = true;
                log("[nr] NR_DIAG_ROI: CSV write failed");
            }
            if (const auto dropped = csv.dropped_.exchange(0, std::memory_order_relaxed))
                log("[nr] NR_DIAG_ROI: CSV writer fell behind; dropped %u rows", dropped);
        }
    }

    void write(const PresentRow& row) {
        std::fprintf(file_, "%s,%llu", row.time, static_cast<unsigned long long>(row.present));
        for (const auto& stage : row.stage) {
            if (!stage.valid) { std::fputs(",,", file_); continue; }
            for (double value : {stage.mean, stage.sd}) {
                char field[64];
                const auto end = std::to_chars(field, field + sizeof field, value, std::chars_format::fixed, 6);
                std::fputc(',', file_);
                if (end.ec == std::errc{}) std::fwrite(field, 1, end.ptr - field, file_);
            }
        }
        std::fputc('\n', file_);
    }
    FILE* file_{};
    bool write_failed_{};
    std::atomic<size_t> head_{}, tail_{};
    std::atomic<unsigned> dropped_{};
    std::array<PresentRow, 2048> rows_{};
};

class PresentRing {
  public:
    explicit PresentRing(Csv& csv) : csv_(csv) {}
    ~PresentRing() { for (auto& slot : slots_) slot.release(); }

    bool uses_device(IUnknown* dev) const {
        if (!dev) return false;
        ID3D11Device* d11{};
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&d11)))) return false;
        IUnknown* target_id{};
        const bool have_id = SUCCEEDED(d11->QueryInterface(IID_PPV_ARGS(&target_id)));
        bool match = false;
        for (const auto& slot : slots_) {
            if (!slot.device) continue;
            if (slot.device == d11) { match = true; break; }
            if (have_id) {
                IUnknown* slot_id{};
                if (SUCCEEDED(slot.device->QueryInterface(IID_PPV_ARGS(&slot_id)))) {
                    match = slot_id == target_id;
                    slot_id->Release();
                    if (match) break;
                }
            }
        }
        if (target_id) target_id->Release();
        d11->Release();
        return match;
    }

    void drain(Session* session, bool closing = false) {
        while (count_ && pop(session, closing)) {}
    }

    void capture(uint64_t present, IUnknown* back_buffer, Session* session) {
        if (count_ == slots_.size()) pop(session, true);
        Slot& slot = slots_[(head_ + count_++) % slots_.size()];
        slot.row = {present};
        std::memcpy(slot.row.time, LocalTime{}.text, sizeof slot.row.time);
        slot.issued = slot.enqueue(back_buffer);
    }

  private:
    static VkFormat vk_format(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
        }
    }

    struct Slot {
        PresentRow row{};
        ID3D11Device* device{};
        ID3D11Texture2D* staging{};
        ID3D11Query* query{};
        ID3D11DeviceContext* context{};
        D3D11_TEXTURE2D_DESC desc{};
        bool pending{}, issued{};

        void release() {
            if (staging) staging->Release();
            if (query) query->Release();
            if (context) context->Release();
            if (device) device->Release();
            staging = nullptr; query = nullptr; context = nullptr; device = nullptr;
            desc = {};
            pending = false; issued = false;
        }

        HRESULT poll() {
            if (!pending) return S_OK;
            const HRESULT hr = context->GetData(query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            pending = hr == S_FALSE;
            return hr;
        }

        bool enqueue(IUnknown* back_buffer) {
            if (!back_buffer) return false;
            ID3D11Texture2D* back{};
            if (FAILED(back_buffer->QueryInterface(IID_PPV_ARGS(&back)))) return false;
            ID3D11Device* src_device{};
            back->GetDevice(&src_device);
            if (!src_device) {
                back->Release();
                return false;
            }
            if (device && device != src_device) release();
            if (poll() != S_OK) {
                src_device->Release();
                back->Release();
                return false;
            }
            D3D11_TEXTURE2D_DESC src{};
            back->GetDesc(&src);
            const auto patch = region(src.Width, src.Height);
            bool ok = src.SampleDesc.Count == 1 && patch.width && patch.height && vk_format(src.Format);
            if (ok && (!staging || device != src_device || desc.Format != src.Format ||
                       desc.Width != patch.width || desc.Height != patch.height)) {
                release();
                src_device->GetImmediateContext(&context);
                desc = {};
                desc.Width = patch.width; desc.Height = patch.height;
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = src.Format;
                desc.Usage = D3D11_USAGE_STAGING;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                const D3D11_QUERY_DESC event{D3D11_QUERY_EVENT, 0};
                ok = context && SUCCEEDED(src_device->CreateTexture2D(&desc, nullptr, &staging)) &&
                     SUCCEEDED(src_device->CreateQuery(&event, &query));
                if (ok) {
                    device = src_device;
                    src_device = nullptr;
                } else {
                    release();
                }
            }
            if (ok) {
                const D3D11_BOX box{patch.x, patch.y, 0, patch.x + patch.width, patch.y + patch.height, 1};
                context->CopySubresourceRegion(staging, 0, 0, 0, 0, back, 0, &box);
                context->End(query);
                pending = true;
            }
            if (src_device) src_device->Release();
            back->Release();
            return ok;
        }

        bool read() {
            if (!issued) return true;
            const HRESULT ready = poll();
            if (ready == S_FALSE) return false;
            if (FAILED(ready)) { issued = false; return true; }
            D3D11_MAPPED_SUBRESOURCE map{};
            const HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
            if (SUCCEEDED(hr)) {
                row.stage[stages] = Session::measure_roi(vk_format(desc.Format), map.pData, map.RowPitch,
                                                         desc.Width, desc.Height);
                context->Unmap(staging, 0);
            }
            issued = false;
            return true;
        }
    };

    bool pop(Session* session, bool expire) {
        auto& slot = slots_[head_];
        const bool back = slot.read();
        if (!back && !expire) return false;
        const bool nr = !session || session->read_present_roi(slot.row.present, slot.row.stage);
        if (!nr && !expire) return false;
        csv_.push(slot.row);
        head_ = (head_ + 1) % slots_.size();
        --count_;
        return true;
    }
    Csv& csv_;
    std::array<Slot, slots> slots_{};
    size_t head_{}, count_{};
};

}  // namespace nr::pe::roi
