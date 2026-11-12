#pragma once
// HUD FIX's capture on the .asi route (nr_hud_capture.hpp's rules): the game's commands followed through hooks of their
// functions, the back buffer copied where its HUD starts. Armed while [HudFix] Mode is Auto or Captured.
//   D3D12: the device's pipeline and render-target-view creation (from the game's swap chain on), the lists' draws,
//          copies and bindings and the queue's ExecuteCommandLists (from the first armed frame); idle while not armed.
//   D3D11: the immediate context's draws, copies and bindings (from the first armed frame); a deferred context's draws
//          are not seen.
//   Vulkan (a native Vulkan game's device): its image views, framebuffers, render passes and pipelines (from its
//          vkCreateDevice), the command buffers' render passes, draws, copies and bindings (from the first armed frame),
//          their submits in order. The copy can only be made outside a render pass: before each pass on a swap-chain
//          image that keeps its contents - a HUD drawn in the scene's own pass is not captured.
#include <d3d11.h>
#include <d3d12.h>
#include "nr_gpu_completion.hpp"
#include <dxgi1_4.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nr::asi::hudcap {

// The format group a CopyResource accepts between two textures (the typeless family: a game's TYPELESS frame beside a
// typed swap chain is one group); every check whether a game's frame can be copied to or from ours asks this.
int copy_group(DXGI_FORMAT f);
// The typed format our copy of a game's UI texture (XeSS-FG's UI tag) is made in for its alpha (UI.a): its own format's
// UAV-writable twin (TYPELESS / sRGB -> UNORM); DXGI_FORMAT_UNKNOWN for a format without an alpha we read. The tag is
// taken (hudcap) and copied (the Present layers) by this one rule.
DXGI_FORMAT ui_alpha_format(DXGI_FORMAT f);

// The game's swap chain was made on `device` (its back buffers' views and the pipelines made from now on are followed
// once the capture is armed; `armed_now`: [HudFix] Mode is Auto or Captured already - the device's hooks go in at once).
void d3d12_device(ID3D12Device* device, bool armed_now);
// Observe root creation early; this does not adopt a device or arm capture.
void d3d12_module(HMODULE module);

// At the game's Present, before the layers: armed or not this frame (the hooks go in at the first armed one; `queue` the
// one the game presents on), and the verdict on the frame's writes for `back` (its swap chain's current buffer, `format`
// NR's writable twin of its format).
// `hudless`: the copy (UNORDERED_ACCESS, `format`, w x h) when captured. `why`: why not.
enum class Result { kOff, kCaptured, kClean, kMissed };
// Frame generation's HUD-less frame this frame (D3D12): what the game handed its frame generation before its Present -
// FSR 3.1's ffxConfigure (HUDLessColor), XeSS-FG's tag (XEFG_SWAPCHAIN_RES_HUDLESS_COLOR), DLSS-G's (Streamline's
// kBufferTypeHUDLessColor tag, NGX's DLSSG.HUDLess at its evaluate: fg_note_dlssg). Handed over while the game's Present
// runs (OptiScaler's own frame generation, inside its Present hook) it would be a frame late: not taken (the capture
// serves those games).
struct Fg {
    bool seen = false;             // this frame's, usable: `copy` holds it, or (copied == false) `game` in `state` does
    ID3D12Resource* copy = nullptr;   // ours (UNORDERED_ACCESS, the back buffer's writable format, w x h)
    // Authored XeSS UI alpha, paired with this HUD-less resource by context/presentId/epoch.
    // Full-frame SDR RGBA8 only, valid until Present; ui_keep pins the original through the GPU copy.
    ID3D12Resource* ui = nullptr;
    D3D12_RESOURCE_STATES ui_state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t ui_frame = 0;
    std::shared_ptr<void> ui_keep;
    bool copied = false;           // already copied into `copy` (on the game's list, when it was handed over)
    ID3D12Resource* game = nullptr;   // the game's resource, valid until the present
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool substitute = false;       // NR's answer may be written back into `game` (frame generation reads it at the present)
    bool hudless = true;           // a frame before the game's HUD (HUD FIX may run on it); false: the frame frame generation
                                   // is dispatched from (ffxDispatch), which may hold the HUD - only NR's change goes into it
    const char* source = "";
    std::string why;               // not seen: why
    bool offered = false;          // not armed: a hand-over came in the last 2 s (the method could run)
};
struct Frame {
    Result result = Result::kOff;
    ID3D12Resource* hudless = nullptr;
    std::string why;
    bool broken = false;   // the capture cannot run in this game at all (its hooks): greyed in the menu, `why`
    Fg fg;
    // kCaptured by the chain capture (nr_hud_capture.hpp chain_picks): `hudless` is the copy of the game's image
    // `chain_image`, the last finished picture of the back buffer's size before its final pass. Until it is trusted
    // (`chain_trusted`: its copies fitted the presented frame - chain_checked) the layers run on the frame itself and
    // the copy only says where the HUD is.
    bool chain = false, chain_trusted = false;
    uint64_t chain_image = 0;
    std::shared_ptr<void> chain_keep; // pins selected snapshot until its reader recording is registered
};
// The chain capture's check (the Present layers, a few frames later): the copy of `image` composed against the presented
// frame differed from it in `hud` of `total` pixels. A copy differing in more than a quarter is not the frame without
// its HUD: refused for a while; one fitting for 15 frames in a row is trusted (NR then runs on it).
void chain_checked(uint64_t image, uint32_t hud, uint32_t total);
void d3d12_use_chain(const Frame& frame); // current nr::gpu recording reads this snapshot
// fg_list_copies: a hand-over valid only while it is handed over is copied on the command list it came with (HUD FIX takes
// it); otherwise only hand-overs valid until the present are kept (read at the Present, nothing put into another's list).
Frame d3d12_present(ID3D12Device* device, IDXGISwapChain* sc, ID3D12CommandQueue* queue, ID3D12Resource* back,
                    DXGI_FORMAT format, uint32_t w, uint32_t h, bool armed, bool fg_armed = false, bool fg_list_copies = false);
// A Present whose layers ran at DLSS-G's evaluate (d3d12_present there with `sc` null): the capture's back buffers from
// its swap chain.
void d3d12_backs(IDXGISwapChain* sc);
// Frame generation's hand-overs are taken from the game's own calls only (`game` says whether a caller address is the
// game's): OptiScaler's own frame generation calls the same libraries with its own command lists and threads.
void set_game_caller(bool (*game)(void* caller));
// HUD FIX wants the capture (Auto / Captured): the views' bookkeeping runs (hooks installed then; off: stopped). The
// query answers at a device's creation, before the first Present ([HudFix] Mode read from the ini).
void set_capture_wanted(bool wanted);
void set_capture_wanted_query(bool (*query)());
// The game dispatches its FSR frame generation itself (its last configure gave no frame-generation callback): the
// generated frames are made from a frame of the game's, not from the back buffer at the present.
bool ffx_app_dispatch();
// After every ExecuteCommandLists of the game's (NR Control's queue hook calls the .asi's observer, which calls this;
// the capture hooks the queue itself only where that hook is not there): the lists' writes in the GPU's order.
nr::gpu::Ticket d3d12_recording(ID3D12GraphicsCommandList* list);
bool vk_lifetime(VkDevice device);
void d3d12_executed(unsigned count, ID3D12CommandList* const* lists, ID3D12CommandQueue* queue);
// NGX's DLSS-G evaluate (the .asi's NVSDK_NGX_D3D12_EvaluateFeature hook): its DLSSG.HUDLess (COPY_DEST there, as
// OptiScaler takes it), copied on its command list. `layers_here`: the Present layers run in this evaluate (d3d12_present
// is called from it next, `sc` null): the frame is this present's, NR's change may be put into it.
void fg_note_dlssg(ID3D12GraphicsCommandList* list, ID3D12Resource* hudless, bool layers_here = false);
// The game's Present call runs (the .asi's Present detours, around the original): frame generation's hand-overs then
// are OptiScaler's own (a frame late), not taken.
struct PresentCall {
    PresentCall();
    ~PresentCall();
};

// D3D11, as d3d12_present: `back` the swap chain's buffer, `format` NR's writable twin. `hudless`: the copy (a texture
// of `format`, w x h, shader resource + unordered access) when captured.
struct Frame11 {
    Result result = Result::kOff;
    ID3D11Texture2D* hudless = nullptr;
    std::string why;
    bool broken = false;
};
Frame11 d3d11_present(ID3D11Device* device, ID3D11Texture2D* back, DXGI_FORMAT format, uint32_t w, uint32_t h, bool armed);

// Vulkan: the game's device was made (`translator`: by DXVK or vkd3d-proton - not followed: their games have their
// D3D capture). Its creations are followed from now on.
void vk_device(VkDevice device, bool translator);
// What the game's device's vkCreateImage / vkCreateImageView said (followed from vk_device, not on a translator's
// device): an image's usage (false: not seen), whether a view reorders its channels (false: identity or not seen). The
// .asi's Vulkan route chooses its copies by them (a transfer copy where the image allows one).
bool vk_image_usage(VkImage image, VkImageUsageFlags* usage);
bool vk_view_reordered(VkImageView view);
// The game's queue submits (the .asi's queue hooks): its command buffers in submit order.
void vk_submitted(uint32_t n, const VkCommandBuffer* buffers);
void vk_submitted2(uint32_t n, const VkSubmitInfo2* submits);
// At a native Vulkan game's Present, before the layers: `images` its swap chain's, `image` the one presented, `hudless`
// the image the copies go into (ours: GENERAL, the swap chain's format, w x h; null: none this frame).
struct FrameVk {
    Result result = Result::kOff;
    std::string why;
    bool broken = false;
};
FrameVk vk_present(VkDevice device, const std::vector<VkImage>& images, VkImage image, VkImage hudless, uint32_t w, uint32_t h,
                   bool armed);

// The .asi records its own commands on this thread (its Present layers): the hooks pass them through unseen.
struct Own {
    Own();
    ~Own();
};

}  // namespace nr::asi::hudcap
