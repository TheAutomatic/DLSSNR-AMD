#pragma once
// HUD FIX's capture ([HudFix] Captured; Auto's first choice): where the game starts drawing its HUD over the finished
// scene, the rules both routes follow - the ReShade add-on through ReShade's events (nr_reshade_addon.cpp), the .asi
// through its own hooks of the game's D3D12 command lists (nr_asi_hudcap.cpp).
// The game's writes on its back buffers are followed. A scene write: a copy, resolve or upload over the whole buffer, a
// clear, a draw over the whole viewport (and scissor) without blending of at most two triangles (the last full-screen
// pass: tone mapping, post), or a depth-tested draw (a scene drawn straight into the back buffer). Any other draw, clear
// or copy on it is an overlay: the HUD. At the first overlay after a scene write the back buffer is copied, before that
// overlay runs. At the frame's Present the writes are read in the order the GPU runs them: the copy holds the frame
// without its HUD when the write before it was a scene write, no scene write came after it and it is of this frame's
// back buffer.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nr::hudcap {

enum : uint8_t { kLost = 0, kScene = 1, kOverlay = 2, kCopy = 3, kUnknown = 4, kChain = 5 };
// kChain (the chain capture, below): image `back` - one of the back buffer's size, not a back buffer - copied into `dst`
// where its finished picture was: the game ended its writes on it (a transition out of render target or unordered
// access) with a scene write last, or started drawing an overlay over it.
// kUnknown: a write on the back buffer this capture cannot judge (a draw whose pipeline it does not know, indirect work
// that may be a dispatch, a secondary command buffer / bundle / deferred context, a next subpass): the frame is not
// trusted.
struct Rec {
    uint8_t kind;
    uint64_t back;   // the back buffer written (kLost: the records before were dropped)
    uint64_t dst = 0;   // kCopy: the image copied into (a list recorded before that image was made again copies elsewhere)
    uint8_t how = 0;    // kChain: 0 at the image's own end of writes or before an overlay over it, 1 as an input of the
                        // back buffer's final pass
    std::shared_ptr<void> keep; // snapshot lease copied with recording/submission records
};
constexpr uint32_t kMaxCopies = 8;   // copies a frame at most (where every place is copied: D3D12)
constexpr size_t kMaxRecs = 4096;    // a list's records kept unread at most

// A draw on a bound back buffer W x H: what is bound for it.
struct Draw {
    bool dsv = false;          // a depth-stencil view bound beside the back buffer
    int8_t depth = -1;         // the depth test: 1 on, 0 off, -1 the API's default (on), -2 not known
    int8_t blend = -1;         // blending on render target 0: 1 on, 0 off, -1 the API's default (off), -2 not known
    bool scissor_on = false;   // the scissor cuts (D3D12: always)
    float vp[4] = {};          // viewport 0: x, y, width, height
    int32_t sc[4] = {};        // scissor 0: left, top, right, bottom
    uint32_t vertices = 0, instances = 0;
    bool indirect = false;     // the counts come from a buffer
};
inline uint8_t draw_kind(const Draw& d, uint32_t back_w, uint32_t back_h) {
    if (d.dsv && d.depth == -2) return kUnknown;   // depth tested or not: not known
    if (d.dsv && d.depth != 0) return kScene;
    if (d.indirect) return kOverlay;
    const float W = float(back_w), H = float(back_h);
    const bool full_vp = d.vp[0] <= 0.5f && d.vp[1] <= 0.5f && d.vp[0] + d.vp[2] >= W - 0.5f && d.vp[1] + d.vp[3] >= H - 0.5f;
    const bool full_sc = !d.scissor_on || (d.sc[0] <= 0 && d.sc[1] <= 0 && float(d.sc[2]) >= W && float(d.sc[3]) >= H);
    if (full_vp && full_sc && d.vertices <= 6 && d.instances <= 1) return d.blend == -2 ? kUnknown : d.blend != 1 ? kScene : kOverlay;
    return kOverlay;
}

// The frame's verdict on its writes `recs` (in the GPU's order) for the back buffer `colour` it presents. `foreign`:
// a write was seen out of that order (a D3D11 deferred context, a D3D12 bundle); `image`: the copy's image (0: none).
enum class Verdict { kCaptured, kClean, kMissed };
inline Verdict verdict(const std::vector<Rec>& recs, uint64_t colour, bool foreign, uint64_t image, std::string& why) {
    uint8_t last = 0;
    bool held = false, copied = false, lost = false, any = false, unknown = false;
    uint64_t last_copy = 0, last_dst = 0;   // one image takes every copy: it holds the last one's, of whatever back buffer
    for (const Rec& r : recs) {
        if (r.kind == kCopy) { last_copy = r.back; last_dst = r.dst; }
        if (r.back != colour && r.kind != kLost) continue;   // another back buffer (an earlier frame's)
        switch (r.kind) {
        case kLost: lost = true; break;
        case kUnknown: unknown = true; break;
        case kScene: last = kScene; held = false; any = true; break;
        case kOverlay: last = kOverlay; any = true; break;
        case kCopy: copied = true; held = last == kScene; break;
        }
    }
    if (foreign) { why = "the game draws on its back buffer from a deferred context or a bundle"; return Verdict::kMissed; }
    if (lost) { why = "the game's writes were not read in time"; return Verdict::kMissed; }
    if (unknown) { why = "the game writes its back buffer in a way this capture cannot judge (a pipeline it does not know, "
                         "indirect work, a secondary command buffer or bundle)"; return Verdict::kMissed; }
    if (!any) { why = "no draw on the back buffer was seen"; return Verdict::kMissed; }
    if (last == kScene) { why = "nothing is drawn after the scene's last write"; return Verdict::kClean; }
    if (held && last_copy == colour && image && last_dst == image) return Verdict::kCaptured;
    if (held && last_copy == colour && image) { why = "the copy went into an earlier image (it was made again)"; return Verdict::kMissed; }
    if (held && image) { why = "another back buffer was copied after this one"; return Verdict::kMissed; }
    why = !image    ? "its image is being made"
          : !copied ? "the HUD's first draw was not where the last frame's was (it is copied there from the next frame)"
                    : "the scene was written again after the copy";
    return Verdict::kMissed;
}

// The chain capture: many games draw no HUD over the finished scene on the back buffer. Their last pass (a copy or a
// full-screen draw: a scene write by the rules above, nothing after it - Verdict::kClean) reads the finished scene from
// an image of the back buffer's size, with the HUD drawn into another image or into that one afterwards. The frame
// without its HUD is then the last kChain copy before the back buffer's last scene write, as long as nothing was copied
// into that copy's image after it. Whether it is that pass's input at all is not known here (bindless games bind
// nothing the hooks could see): the caller checks the copy against the presented frame before trusting it.
// Every such copy (in the GPU's order) before the back buffer's last scene write whose image was not copied into again
// after it - the caller takes a trusted one, else the last; `how` as Rec::how.
struct ChainCopy {
    uint64_t image = 0, dst = 0;
    uint8_t how = 0;
    std::shared_ptr<void> keep;
};
inline std::vector<ChainCopy> chain_picks(const std::vector<Rec>& recs, uint64_t colour, std::string& why) {
    std::vector<ChainCopy> seen, picks;
    bool scene = false;
    size_t at_scene = 0;
    for (const Rec& r : recs) {
        if (r.kind == kChain) {
            seen.push_back({r.back, r.dst, r.how, r.keep});
        } else if (r.kind == kScene && r.back == colour) {
            scene = true;
            at_scene = seen.size();
        }
    }
    if (!scene) { why = "no scene write on the back buffer"; return {}; }
    if (seen.empty()) { why = "no finished picture of the back buffer's size was seen before its last write"; return {}; }
    for (size_t i = 0; i < at_scene; ++i) {
        bool again = false;   // (its copy's image written again later: by a later copy of the same image, or after the pass)
        for (size_t j = i + 1; j < seen.size(); ++j) again = again || seen[j].dst == seen[i].dst;
        if (!again) picks.push_back(seen[i]);
    }
    if (picks.empty()) why = at_scene ? "the copies of the final pass's inputs were overwritten after that pass"
                                      : "no finished picture of the back buffer's size before its last write (only after it)";
    return picks;
}

}  // namespace nr::hudcap
