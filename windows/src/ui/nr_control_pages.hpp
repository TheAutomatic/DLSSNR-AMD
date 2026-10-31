#pragma once
// NR Control's pages that the .asi's menu (linux/src/pe/nr_pe_menu.cpp) and the ReShade add-on's page
// (linux/src/pe/nr_reshade_addon.cpp) draw alike: the layers' status, the video memory, the OSD's lines, the report for a
// bug report, and the NR chain's editor (the layer list and the selected layer's page). Windows code (both are PE
// modules); ImGui only, the caller owns the context. What each route shows comes in an nr::control::Shared page (the .asi:
// control.bin; the add-on fills one of its own), what differs between the routes in a ChainRoute.
#include "nr_path_u8.hpp"
#include "nr_number.hpp"
#include "imgui.h"
#include "nr_control_shared.hpp"
#include "nr_control_ui.hpp"
#include "nr_ini_edit.hpp"
#include "nr_ini_network.hpp"
#include "nr_pe_events.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nr::control_ui {

// A text's size, the room left on the line, the cursor on the screen. ReShade's ImGui (the add-on's page) is a function
// table built by MSVC, which returns an ImVec2 through a hidden pointer, where mingw's g++ expects it in a register
// (MSVC's rule for a type with a constructor): called directly, those entries crash in ReShade. NR_IMGUI_MSVC_TABLE
// (the add-on) calls them the MSVC way.
inline ImVec2 text_size(const char* t) {
#ifdef NR_IMGUI_MSVC_TABLE
    using F = ImVec2* (*)(ImVec2*, const char*, const char*, bool, float);
    ImVec2 r;
    reinterpret_cast<F>(imgui_function_table_instance()->CalcTextSize)(&r, t, nullptr, false, -1.0f);
    return r;
#else
    return ImGui::CalcTextSize(t);
#endif
}
inline float text_width(const char* t) { return text_size(t).x; }
inline ImVec2 room() {
#ifdef NR_IMGUI_MSVC_TABLE
    using F = ImVec2* (*)(ImVec2*);
    ImVec2 r;
    reinterpret_cast<F>(imgui_function_table_instance()->GetContentRegionAvail)(&r);
    return r;
#else
    return ImGui::GetContentRegionAvail();
#endif
}
inline float room_x() { return room().x; }
inline ImVec2 cursor_screen_pos() {
#ifdef NR_IMGUI_MSVC_TABLE
    using F = ImVec2* (*)(ImVec2*);
    ImVec2 r;
    reinterpret_cast<F>(imgui_function_table_instance()->GetCursorScreenPos)(&r);
    return r;
#else
    return ImGui::GetCursorScreenPos();
#endif
}

// The chain has rows (the .asi; the add-on always fills at least one)
inline bool chain_rows(const nr::control::Shared* s) {
    for (const auto& r : s->layers)
        if (r.number) return true;
    return false;
}
// Text at the right edge of its table cell (numbers line up by their last digit).
inline void cell_right(const char* t) {
    const float w = text_width(t), room = room_x();
    if (room > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - w);
    ImGui::TextUnformatted(t);
}
inline void cell_ms(bool known, float ms) {
    char t[32];
    std::snprintf(t, sizeof t, known ? "%.2f" : "--", double(ms));
    cell_right(t);
}
// NR's video memory by network and kind, and the process's whole use (control page, nr_vram_ledger.hpp).
inline std::string mib(uint32_t kib) {
    char t[32];
    const double m = double(kib) / 1024.0;
    std::snprintf(t, sizeof t, m >= 100.0 ? "%.0f" : "%.1f", m);
    return t;
}
inline void draw_vram(const nr::control::Shared* s) {
    if (!s->vram_known) return;
    uint32_t ours = 0;
    for (const auto& owner : s->vram_kib)
        for (uint32_t k : owner) ours += k;
    if (s->vram_process_kib)
        ImGui::Text(tr("Video memory: NR %s MiB; this process %s MiB of a %s MiB budget"), mib(ours).c_str(),
                    mib(s->vram_process_kib).c_str(), mib(s->vram_budget_kib).c_str());
    else
        ImGui::Text(tr("Video memory: NR %s MiB"), mib(ours).c_str());
    // folded at first (NR_CONTROL_VRAM_OPEN=1 starts open, for tests)
    static const bool open_at_start = [] { const char* e = std::getenv("NR_CONTROL_VRAM_OPEN"); return e && e[0] == '1'; }();
    if (!ImGui::TreeNodeEx(tr("Video memory, in detail"),
                           ImGuiTreeNodeFlags_SpanAvailWidth | (open_at_start ? ImGuiTreeNodeFlags_DefaultOpen : 0)))
        return;
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
    static const char* const kHead[] = {"", NR_N("Weights"), NR_N("Activations"), NR_N("Images, buffers"), NR_N("Total MiB")};
    static const char* const kHeadHelp[] = {
        nullptr, NR_N("The network's weights (one copy for every layer and size that runs it)."),
        NR_N("The network's working memory for its layers' intermediate results (shared by its sizes). \"shared\": "
             "it has none of its own, it runs in another network's that is not running (after a network switch)."),
        NR_N("The network's images at each size it runs (inputs, history, outputs) and small buffers."),
        NR_N("All of the row, in MiB."), nullptr};
    // as wide as its columns need: with a larger Text size the page scrolls sideways rather than the columns running
    // into each other
    float label_w = text_width(tr("NR in all"));
    for (int n : nr::kNetworkOrder) label_w = std::max(label_w, text_width(nr::network_codename(n)));
    for (const char* l : {NR_N("RenoDX layers"), NR_N("NR route"), NR_N("Frame copies"), NR_N("wilsjo2 layers"),
                          NR_N("NR Control"), NR_N("Other"), NR_N("The game and others")})
        label_w = std::max(label_w, text_width(tr(l)));
    float vram_w = label_w;
    for (int c = 1; c < 5; ++c) vram_w += std::max(text_width(tr(kHead[c])), text_width("00000.0"));
    vram_w += 10.f * ImGui::GetStyle().CellPadding.x;
    if (ImGui::BeginTable("vram", 5, flags, ImVec2(std::max(room_x(), vram_w), 0.f))) {
        const float number_w = text_width("00000.0");
        for (int c = 0; c < 5; ++c)
            ImGui::TableSetupColumn(tr(kHead[c]), c ? ImGuiTableColumnFlags_WidthFixed : 0,
                                    c ? std::max(text_width(tr(kHead[c])), number_w) : 0.f);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < 5; ++c) {
            ImGui::TableSetColumnIndex(c);
            if (c) cell_right(kHead[c]); else ImGui::TextUnformatted(tr("MiB"));
            if (kHeadHelp[c] && ImGui::IsItemHovered()) tooltip(kHeadHelp[c]);
        }
        auto row = [&](const char* name, const char* help, const uint32_t* parts, int count) {
            uint32_t sum = 0;
            for (int i = 0; i < count; ++i) sum += parts[i];
            if (!sum) return;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(tr(name));
            if (help && ImGui::IsItemHovered()) tooltip(help);
            for (int i = 0; i < 3; ++i) {
                ImGui::TableNextColumn();
                // no arena of its own: it runs in another network's (only while that one does not run)
                if (count == 3) cell_right(i == 1 && !parts[1] && parts[0] ? "shared" : mib(parts[i]).c_str());
            }
            ImGui::TableNextColumn();
            cell_right(mib(sum).c_str());
        };
        for (int n : nr::kNetworkOrder) {
            const uint32_t* k = s->vram_kib[n];
            const uint32_t parts[3] = {k[nr::vram::kWeights], k[nr::vram::kActivations],
                                       k[nr::vram::kUnset] + k[nr::vram::kCopies] + k[nr::vram::kWilsjo2] +
                                           k[nr::vram::kMenu] + k[nr::vram::kRenoDX]};
            row(nr::network_codename(n), nullptr, parts, 3);
        }
        uint32_t by_kind[nr::vram::kKinds] = {};
        for (int k = 0; k < nr::vram::kKinds; ++k) by_kind[k] = s->vram_kib[nr::control::kNoNet][k];
        row("RenoDX layers", "The RenoDX processing's textures around the network (each layer and place its own): "
                             "the frame in the network's encoding, the passes' inputs and answers.",
            &by_kind[nr::vram::kRenoDX], 1);
        row("NR route", "Shared by every layer: the frame handed to and from the network, screenshots.",
            &by_kind[nr::vram::kUnset], 1);
        row("Frame copies", "The .asi's copies: the frame before the upscaler, its guides for Present layers, the "
                            "Present layers' textures. The ReShade add-on's: HUD FIX's frames, the PQ conversion's, the "
                            "motion copies, Inspect's.", &by_kind[nr::vram::kCopies], 1);
        row("wilsjo2 layers", "The wilsjo2 processing's textures around the network (each layer its own): the "
                              "network's input, the original, its answer.",
            &by_kind[nr::vram::kWilsjo2], 1);
        row("NR Control", "This menu's image.", &by_kind[nr::vram::kMenu], 1);
        const uint32_t other_weights = by_kind[nr::vram::kWeights] + by_kind[nr::vram::kActivations];
        row("Other", nullptr, &other_weights, 1);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(tr("NR in all"));
        ImGui::TableSetColumnIndex(4);
        cell_right(mib(ours).c_str());
        if (s->vram_process_kib >= ours) {
            const uint32_t game = s->vram_process_kib - ours;
            row("The game and others", "This process's use of the video memory heap (VK_EXT_memory_budget) less "
                                       "NR's: the game, its D3D layer, the upscaler, frame generation, overlays.",
                &game, 1);
        }
        ImGui::EndTable();
    }
    if (s->vram_heap_kib)
        ImGui::TextDisabled(tr("Budget %s MiB of %s MiB video memory (the driver's: what this process may use now)."),
                            mib(s->vram_budget_kib).c_str(), mib(s->vram_heap_kib).c_str());
    ImGui::TreePop();
}

// The video memory's residency as the session last decided it (nr_pe_events.hpp): its mode, the budget with the
// reserve, each network held and where, what it does now and why, what a switch would need, and its last events.
inline void draw_residency() {
    const nr::pe::events::Residency r = nr::pe::events::residency();
    if (!r.known) return;
    static const bool open_at_start = [] { const char* e = std::getenv("NR_CONTROL_VRAM_OPEN"); return e && e[0] == '1'; }();   // tests
    if (!ImGui::TreeNodeEx(tr("Residency"), ImGuiTreeNodeFlags_SpanAvailWidth | (open_at_start ? ImGuiTreeNodeFlags_DefaultOpen : 0))) return;
    static const char* const kMode[] = {"auto", "resident", "offload", "minimal"};
    const auto mib = [](uint64_t b) { return double(b) / (1 << 20); };
    ImGui::Text(tr("Mode: %s ([Network] Residency)"), kMode[std::clamp(r.mode, 0, 3)]);
    if (r.budget) {
        const uint64_t free = r.budget > r.usage ? r.budget - r.usage : 0;
        ImGui::Text(tr("Budget %.0f MiB, used %.0f MiB (%.0f %%), remaining %.0f MiB, reserve %.0f MiB"), mib(r.budget),
                    mib(r.usage), 100.0 * double(r.usage) / double(r.budget), mib(free), mib(r.reserve));
        // the bar: used of the budget, the reserve's line where the remaining room would fall below it
        const ImVec2 at = cursor_screen_pos();
        const float w = std::max(room_x(), 50.f), h = ImGui::GetFontSize() * 0.6f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float used = std::min(1.f, float(double(r.usage) / double(r.budget)));
        const float line = r.reserve < r.budget ? 1.f - float(double(r.reserve) / double(r.budget)) : 0.f;
        dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(255, 255, 255, 30));
        dl->AddRectFilled(at, ImVec2(at.x + w * used, at.y + h), used > line ? IM_COL32(240, 140, 77, 220) : IM_COL32(90, 170, 230, 220));
        dl->AddLine(ImVec2(at.x + w * line, at.y - 2.f), ImVec2(at.x + w * line, at.y + h + 2.f), IM_COL32(255, 255, 255, 220), 2.f);
        ImGui::Dummy(ImVec2(w, h));
        if (ImGui::IsItemHovered())
            tooltip("Used of this process's budget (VK_EXT_memory_budget). The white line: the reserve - when the "
                    "remaining room falls below it (max(1 GiB, 10 % of the video memory)), auto moves networks "
                    "that do not run to RAM, the least recently used first.");
    }
    static const char* const kState[] = {NR_N("not loaded"), NR_N("serving"), NR_N("kept in video memory"), NR_N("moving to RAM"),
                                         NR_N("in RAM")};
    for (const auto& n : r.nets)
        ImGui::BulletText(tr("%s at %ux%u: %s"), n.net >= 0 ? nr::network_codename(n.net) : "?", n.width, n.height,
                          tr(kState[std::clamp(n.state, 0, 4)]));
    if (!r.status.empty()) ImGui::TextWrapped(tr("Now: %s%s%s"), r.status.c_str(), r.reason.empty() ? "" : " - ", r.reason.c_str());
    else text_dim_wrapped("Now: nothing to do%s%s", r.reason.empty() ? "" : " - ", r.reason.c_str());
    if (r.need && r.budget) {
        const uint64_t free = r.budget > r.usage ? r.budget - r.usage : 0;
        text_dim_wrapped("Another network at %ux%u needs about %.0f MiB: %s", r.need_w, r.need_h, mib(r.need),
                            tr(r.need <= free ? "it fits" : "it does not fit now - networks that do not run go to RAM first (auto)"));
    }
    // its last events
    const uint64_t now = GetTickCount64();
    int shown = 0;
    const auto events = nr::pe::events::recent();
    for (auto it = events.rbegin(); it != events.rend() && shown < 5; ++it) {
        if (it->category != nr::pe::events::kResidency) continue;
        text_dim_wrapped("%5.0f s ago  %s%s", double(now - it->ms) / 1000.0, it->text,
                            it->count > 1 ? (" (x" + std::to_string(it->count) + ")").c_str() : "");
        ++shown;
    }
    ImGui::TreePop();
}

// A layer switching between running and not many times a second (nr_pe_events.hpp flapping): one line at the top.
inline void draw_flapping() {
    const nr::pe::events::Flapping f = nr::pe::events::flapping();
    if (!f.on) return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(1.f, .55f, .3f, 1.f), tr("NR keeps stopping and starting: %s. The Diagnostics page has the events."),
                       f.what.c_str());
    ImGui::PopTextWrapPos();
}

// Diagnostics' own lines: the last stop and the events, by category (the report button is the caller's).
inline void draw_events() {
    const uint64_t now = GetTickCount64();
    const nr::pe::events::LastStop stop = nr::pe::events::last_stop();
    if (stop.ms) ImGui::TextWrapped(tr("Last stop: Layer %d, %s (%.0f s ago)"), stop.layer, stop.why.c_str(), double(now - stop.ms) / 1000.0);
    else ImGui::TextDisabled(tr("Last stop: none this run"));
    static bool show[nr::pe::events::kCategories] = {true, true, true, true};
    ImGui::TextUnformatted(tr("Events:"));
    for (int c = 0; c < nr::pe::events::kCategories; ++c) {
        ImGui::SameLine();
        ImGui::Checkbox(tr(nr::pe::events::category_name(nr::pe::events::Category(c))), &show[c]);
    }
    if (ImGui::BeginChild("events", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 10), ImGuiChildFlags_Borders)) {
        const auto events = nr::pe::events::recent();
        for (auto it = events.rbegin(); it != events.rend(); ++it) {   // newest first
            if (!show[it->category]) continue;
            ImGui::TextDisabled(tr("%7.1f s"), double(now - it->ms) / 1000.0);
            ImGui::SameLine();
            ImGui::TextWrapped("%-8s %s%s", tr(nr::pe::events::category_name(it->category)), it->text,
                        it->count > 1 ? (" (x" + std::to_string(it->count) + ")").c_str() : "");
        }
        if (events.empty()) ImGui::TextDisabled(tr("none yet"));
    }
    ImGui::EndChild();
}

// The layers as they last ran: a row each (Shared::layers), and the sum.
inline void draw_layer_status(const nr::control::Shared* s) {
    std::vector<const nr::control::LayerStatus*> rows;
    for (const auto& r : s->layers)
        if (r.number) rows.push_back(&r);
    std::sort(rows.begin(), rows.end(), [](auto* a, auto* b) { return a->number < b->number; });
    if (rows.empty()) {   // nothing has run yet: no table of headers alone
        ImGui::TextDisabled(tr("No layer has run yet."));
        return;
    }
    const uint32_t now = uint32_t(GetTickCount64());
    static const char* const kWhere[] = {NR_N("after upscaler"), NR_N("before upscaler"), NR_N("Present")};
    float total = 0.f, network = 0.f;
    int running = 0;
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
    // Columns 4..7 hold numbers: their headers and cells sit at the right edge.
    static const char* const kHead[] = {NR_N("Layer"), NR_N("Where"), NR_N("Network"), NR_N("Size"), NR_N("Passes"), NR_N("Net ms"),
                                        NR_N("Proc ms"), NR_N("Total ms"), NR_N("State")};
    static const char* const kHeadHelp[] = {
        nullptr, NR_N("Where the layer ran this frame."), NR_N("The network the layer runs."), NR_N("The frame the layer works on."),
        NR_N("Legacy: network passes a run. Fast: strength referred to legacy passes, with one network run."),
        NR_N("The network's own kernels (pre / post blocks included), every pass, averaged. Everything else is processing."),
        NR_N("Everything outside the network's own kernels: the layer's processing, adapters and composition."),
        NR_N("Net ms + Proc ms: the layer's measured GPU time; includes work from the TOP_OF_PIPE start."), nullptr};
    const float number_w = text_width("000.00");
    // As wide as its columns need (the widest text each can hold): with a larger Text size in the same window the page
    // scrolls sideways rather than the columns running into each other
    {
        float need = 9.f * 2.f * ImGui::GetStyle().CellPadding.x;
        float c0 = std::max(text_width(tr(kHead[0])), text_width("8 wilsjo2")), c1 = text_width(tr(kHead[1]));
        float c2 = text_width(tr(kHead[2])), c3 = std::max(text_width(tr(kHead[3])), text_width("7680x4320"));
        for (const char* w : kWhere) c1 = std::max(c1, text_width(tr(w)));
        for (const nr::control::LayerStatus* r : rows) {
            if (r->net < nr::control::kNoNet) c2 = std::max(c2, text_width(nr::network_codename(int(r->net))));
            if (r->nr_width) c3 = std::max(c3, text_width(tr("network %ux%u")) + text_width("0000"));
        }
        need += c0 + c1 + c2 + c3;
        for (int c = 4; c <= 7; ++c)
            need += std::max(text_width(tr(kHead[c])), c == 4 ? text_width(tr("%u (legacy)")) : number_w);
        need += std::max(text_width(tr(kHead[8])), text_width(tr("building its network")));
        if (!ImGui::BeginTable("layer-status", 9, flags, ImVec2(std::max(room_x(), need), 0.f))) return;
    }
    for (int c = 0; c < 9; ++c) {
        if (c >= 4 && c <= 7)   // as wide as the header or the widest number (headers of a manual row do not size)
            ImGui::TableSetupColumn(tr(kHead[c]), ImGuiTableColumnFlags_WidthFixed,
                                    std::max(text_width(tr(kHead[c])), c == 4 ? text_width(tr("%u (legacy)")) : number_w));
        else
            ImGui::TableSetupColumn(tr(kHead[c]), c == 8 ? ImGuiTableColumnFlags_WidthStretch : 0);
    }
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < 9; ++c) {
        ImGui::TableSetColumnIndex(c);
        if (c >= 4 && c <= 7) cell_right(tr(kHead[c])); else ImGui::TextUnformatted(tr(kHead[c]));
        if (kHeadHelp[c] && ImGui::IsItemHovered()) tooltip(kHeadHelp[c]);
    }
    for (const nr::control::LayerStatus* r : rows) {
        const bool fresh = r->updated && now - r->updated < 1500;
        const bool ran = r->state == 1 && fresh && r->gpu_ms > 0.f;
        const bool split = ran && r->network_ms > 0.f;
        char t[64];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%u %s", r->number, r->processing == 1 ? "wilsjo2" : "RenoDX");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r->state == 0 ? "-" : tr(kWhere[std::min(r->placement, 2u)]));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r->net < nr::control::kNoNet ? nr::network_codename(int(r->net)) : "-");
        ImGui::TableNextColumn();
        if (!r->width) ImGui::TextUnformatted(tr("-"));
        else if (r->nr_width && (r->nr_width != r->width || r->nr_height != r->height)) {
            // the network's own size below the frame's (one line of both made the table wider than the page)
            ImGui::Text("%ux%u", r->width, r->height);
            ImGui::TextDisabled(tr("network %ux%u"), r->nr_width, r->nr_height);
        } else
            ImGui::Text("%ux%u", r->width, r->height);
        ImGui::TableNextColumn();
        if (r->fast_passes > 0.f) std::snprintf(t, sizeof t, tr("%.2f (fast)"), double(r->fast_passes));
        else std::snprintf(t, sizeof t, r->passes ? "%u (legacy)" : "-", r->passes);
        cell_right(t);
        // the network's own cost, the layer's processing around it, and both (a run, averaged)
        ImGui::TableNextColumn();
        cell_ms(split, r->network_ms);
        ImGui::TableNextColumn();
        cell_ms(split, std::max(r->gpu_ms - r->network_ms, 0.f));
        ImGui::TableNextColumn();
        cell_ms(ran, r->gpu_ms);
        if (ran) {
            total += r->gpu_ms;
            network += r->network_ms;
            ++running;
        }
        ImGui::TableNextColumn();
        const char* state = r->state == 2 ? NR_N("building its network")
                            : r->state == 3 ? NR_N("not running")
                            : r->state == 0 || !fresh ? NR_N("waiting")
                            : NR_N("running");
        if (r->note[0]) ImGui::TextWrapped("%s: %s", tr(state), tr_text(r->note));
        else ImGui::TextUnformatted(tr(state));
    }
    if (rows.size() > 1) {   // the sum, a frame
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(tr("All layers"));
        ImGui::TableSetColumnIndex(5);
        cell_ms(running > 0, network);
        ImGui::TableNextColumn();
        cell_ms(running > 0, std::max(total - network, 0.f));
        ImGui::TableNextColumn();
        cell_ms(running > 0, total);
    }
    ImGui::EndTable();
}

// [Network] OSD's text (the caller's window around it): while a switch builds, its progress; NR off; a line a layer
// (`mode` 3, cost: with each layer's GPU time split into the network and the processing around it, and the sums);
// else the running network.
// v2.2: the network runs as ACO through ASD on the game's D3D12 device (the model's report: aco_why "via ASD, ...")
inline bool osd_via_asd(const nr::control::Shared* s) {
    return s->aco_state == 1 && std::strncmp(s->aco_why, "via ASD", 7) == 0;
}
inline bool osd_switching(const nr::control::Shared* s) {
    return s->building && s->building_net < nr::control::kNoNet && s->building_net != s->net_running;
}
// "  2.31 ms (network 1.90, processing 0.41)": the parts as the layer table splits them; the total alone while the
// network's part is not known
inline std::string osd_cost(float gpu_ms, float network_ms) {
    char t[96];
    if (network_ms > 0.f)
        std::snprintf(t, sizeof t, tr("  %.2f ms (network %.2f, processing %.2f)"), double(gpu_ms), double(network_ms),
                      double(std::max(gpu_ms - network_ms, 0.f)));
    else
        std::snprintf(t, sizeof t, "  %.2f ms", double(gpu_ms));
    return t;
}
// The OSD's window, both routes alike: in the bottom-right corner, a dark translucent panel with a thin accent edge,
// stacked upwards (`slot` 0 the lowest: the network OSD; 1 above it: the Preprocess notice). Its height is kept for
// the slot above.
inline float& osd_height(int slot) { static float h[2] = {}; return h[std::clamp(slot, 0, 1)]; }
inline void osd_window(const char* id, int slot, const std::function<void()>& body) {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float margin = ImGui::GetFontSize() * 1.4f;
    float y = ds.y - margin;
    for (int i = 0; i < slot; ++i) y -= osd_height(i) + ImGui::GetFontSize() * 0.4f;
    ImGui::SetNextWindowPos(ImVec2(ds.x - margin, y), ImGuiCond_Always, ImVec2(1.f, 1.f));
    const ImVec4 edge = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 6.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.07f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(edge.x, edge.y, edge.z, 0.55f));
    if (ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                      ImGuiWindowFlags_NoNav))
        body();
    osd_height(slot) = ImGui::GetWindowHeight();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

inline void draw_osd_lines(const nr::control::Shared* s, int mode) {
    if (osd_switching(s)) {   // the build's progress as a bar, its stage written on it
        ImGui::Text(tr(s->net_running < nr::control::kNoNet ? "NR: switching to %s ... %.0f%%" : "NR: building %s ... %.0f%%"),
                    nr::network_codename(int(s->building_net)), double(s->build_progress) * 100.0);
        char phase[sizeof s->build_phase + 1] = {};
        std::memcpy(phase, s->build_phase, sizeof s->build_phase);   // (the page may not end it)
        ImGui::ProgressBar(std::clamp(s->build_progress, 0.f, 1.f), ImVec2(ImGui::GetFontSize() * 18.f, 0.f), phase);
    } else if (!s->nr_on) {
        ImGui::TextUnformatted(tr("NR: off"));
    } else if (chain_rows(s)) {   // a line a layer, by number
        std::vector<const nr::control::LayerStatus*> rows;
        for (const auto& r : s->layers)
            if (r.number && r.net < nr::control::kNoNet) rows.push_back(&r);
        std::sort(rows.begin(), rows.end(), [](auto* a, auto* b) { return a->number < b->number; });
        const uint32_t now = uint32_t(GetTickCount64());
        float total = 0.f, network = 0.f;
        bool split = true;   // every layer that ran has its network's part
        for (const nr::control::LayerStatus* r : rows) {
            const bool ran = r->state == 1 && now - r->updated < 1500 && r->gpu_ms > 0.f;
            if (mode == 3 && ran) {
                ImGui::Text(tr("NR layer %u: %s%s"), r->number, nr::network_codename(int(r->net)),
                            osd_cost(r->gpu_ms, r->network_ms).c_str());
                total += r->gpu_ms;
                network += r->network_ms;
                split = split && r->network_ms > 0.f;
            } else {
                ImGui::Text(tr("NR layer %u: %s"), r->number, nr::network_codename(int(r->net)));
            }
        }
        if (mode == 3 && rows.size() > 1) ImGui::Text(tr("NR in all: %s"), osd_cost(total, split ? network : 0.f).c_str() + 2);   // (its two leading spaces)
    } else if (s->net_running >= nr::control::kNoNet) {   // nothing runs (the .asi: no layer on)
        ImGui::TextUnformatted(tr("NR: not running"));
    } else if (mode == 3 && s->gpu_ms > 0.f) {
        ImGui::Text(tr("NR: %s%s"), nr::network_codename(int(s->net_running)), osd_cost(s->gpu_ms, s->network_ms).c_str());
    } else {
        ImGui::Text(tr("NR: %s"), nr::network_codename(int(s->net_running)));
    }
    if (osd_via_asd(s)) ImGui::TextUnformatted(tr("NR: ACO via ASD, active"));   // v2.2: the native D3D12 ASD backend runs
}

// ---- the menus' frame (NR Control and the Add-ons page): the bar of facts, the sidebar's foot, NR's cost ----------
// "D3D12 (vkd3d-proton)  |  AMD Radeon RX 9070 XT (RADV GFX1201)  |  Mesa 26.2.5": the API the game draws with (under
// Wine the layer that translates it), the GPU and driver NR runs on (empty until NR made its device)
inline std::string info_line(std::string api, const std::string& gpu, std::string driver) {
    static const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    if (api.empty()) api = "-";
    if (wine && api == "D3D12") api += " (vkd3d-proton)";
    else if (wine && (api == "D3D11" || api == "D3D10" || api == "D3D9")) api += " (DXVK)";
    if (const size_t cut = driver.find(" - "); cut != std::string::npos) driver.resize(cut);   // "Mesa 26.2.5 - kisak-mesa PPA"
    return api + "  |  " + (gpu.empty() ? std::string(tr("GPU: known once NR runs")) : gpu) + (driver.empty() ? "" : "  |  ") + driver;
}
inline void info_bar(const std::string& line) {
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y;
    ImGui::BeginChild("##head", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    const float w = text_width(line.c_str());
    ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x, (ImGui::GetWindowWidth() - w) * 0.5f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(line.c_str());
    ImGui::EndChild();
}
// The networks the layers ran last, by their full names in the layers' order, each once (none ran: `fallback`'s)
inline std::vector<std::string> ran_networks(const nr::control::Shared* s, uint32_t fallback = nr::control::kNoNet) {
    std::vector<std::string> nets;
    const auto add = [&](uint32_t n) {
        if (n >= nr::control::kNoNet) return;
        const std::string name = nr::network_codename(int(n));
        if (std::find(nets.begin(), nets.end(), name) == nets.end()) nets.push_back(name);
    };
    if (s) {
        std::vector<const nr::control::LayerStatus*> ran;
        for (const auto& l : s->layers) if (l.number && l.state == 1) ran.push_back(&l);
        std::sort(ran.begin(), ran.end(), [](auto* a, auto* b) { return a->number < b->number; });
        for (const auto* l : ran) add(l->net);
    }
    if (nets.empty()) add(fallback);
    return nets;
}
// The sidebar's foot (OptiScaler's menu has its own words in the same place): short rows, then "Net" and the networks,
// broken after an underscore where one is wider than the sidebar (CC_SILVER_AARDWOLD_INT4_MIXED) - beside "Net" when it
// fits there, else below. Drawn at the bottom of the current child when there is room.
struct FootRow { const char* label; std::string value; bool lit = false; };
// ACO Mode in this run (the Network page says why when off): "On (RDNA4)" lit, "Off", "-" until NR first runs
inline FootRow aco_row(const nr::control::Shared* s) {
    const uint32_t a = s ? s->aco_state : 0;
    if (a == 1) return {NR_N("ACO"), std::string(tr("On")) + (s->aco_rdna3 ? " (RDNA3)" : " (RDNA4)"), true};
    return {NR_N("ACO"), a >= 2 && a <= 4 ? tr("Off") : tr("-"), false};
}
// The device the network runs on (Shared::device_path): "Bridge" (a Vulkan device of NR's own beside the game's D3D),
// "Vulkan" (the game's own Vulkan device), "D3D12" (the game's D3D12 device itself), "-" until NR first runs
inline FootRow path_row(const nr::control::Shared* s) {
    static const char* const kPath[] = {"-", NR_N("Bridge"), NR_N("Vulkan"), "D3D12"};
    const uint32_t p = s ? s->device_path : 0;
    return {NR_N("Path"), tr(kPath[p < 4 ? p : 0]), false};
}
// The same in words (the Network page, the report): what the network runs on and how the frame gets there
inline const char* path_text(uint32_t path) {
    switch (path) {
        case 1: return NR_N("Path: the bridge - the network runs on a Vulkan device of NR's own beside the game's D3D, the "
                            "frame crosses over through shared textures and fences");
        case 2: return NR_N("Path: the game's own Vulkan device (a Vulkan game, or D3D9 through DXVK)");
        case 3: return NR_N("Path: the game's own D3D12 device");
        default: return NR_N("Path: not known until NR first runs");
    }
}
inline void nav_foot(const std::vector<FootRow>& rows, const std::vector<std::string>& nets) {
    // the values' column after the widest label and a gap (ReShade's font is not NR Control's); SameLine counts from
    // the window's edge, its padding included
    float col = text_width(tr("Net"));
    for (const FootRow& r : rows) col = std::max(col, text_width(tr(r.label)));
    col += ImGui::GetStyle().WindowPadding.x + 2 * ImGui::GetStyle().ItemSpacing.x;
    const float wide = room_x();
    std::vector<std::string> lines;
    const bool beside = nets.size() == 1 && text_width(nets[0].c_str()) <= ImGui::GetStyle().WindowPadding.x + wide - col;
    if (beside) lines = nets;
    for (size_t k = 0; !beside && k < nets.size(); ++k) {
        std::string line, word;
        const std::string& n = nets[k];
        for (size_t i = 0; i <= n.size(); ++i) {
            if (i < n.size()) word += n[i];
            if (i < n.size() && n[i] != '_') continue;
            if (!line.empty() && text_width((line + word).c_str()) > wide) { lines.push_back(line); line.clear(); }
            line += word;
            word.clear();
        }
        if (!line.empty()) lines.push_back(line);
    }
    const float h = float(rows.size() + 1 + (beside || lines.empty() ? 0 : lines.size())) * ImGui::GetTextLineHeightWithSpacing();
    if (room().y <= h + ImGui::GetStyle().ItemSpacing.y) return;
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - h - ImGui::GetStyle().WindowPadding.y);
    ImGui::Separator();
    const ImVec4 green(0.45f, 0.85f, 0.45f, 1.f);
    for (const FootRow& r : rows) {
        ImGui::TextDisabled("%s", tr(r.label)); ImGui::SameLine(col);
        if (r.lit) ImGui::TextColored(green, "%s", r.value.c_str()); else ImGui::TextDisabled("%s", r.value.c_str());
    }
    ImGui::TextDisabled(tr("Net"));
    if (lines.empty()) { ImGui::SameLine(col); ImGui::TextDisabled(tr("-")); }
    else if (beside) { ImGui::SameLine(col); ImGui::TextUnformatted(lines[0].c_str()); }
    else for (const std::string& l : lines) ImGui::TextUnformatted(l.c_str());
}
// NR's GPU time a frame in its parts, summed over the layers that ran (each layer's: the Status page):
// "NR: net 7.38ms / proc 0.26ms", "NR: -" while none runs
inline std::string cost_text(const nr::control::Shared* s) {
    float total = 0.f, network = 0.f;
    bool ran = false;
    if (s) {
        const uint32_t now = uint32_t(GetTickCount64());
        for (const auto& l : s->layers)
            if (l.number && l.state == 1 && l.updated && now - l.updated < 1500 && l.gpu_ms > 0.f) {
                total += l.gpu_ms;
                network += l.network_ms;
                ran = true;
            }
    }
    if (!ran) return tr("NR: -");
    char t[64];
    std::snprintf(t, sizeof t, tr("NR: net %.2fms / proc %.2fms"), double(network), double(std::max(total - network, 0.f)));
    return t;
}

// ---- the menus' language: [NRControl] Language, a file of dlssnr-amd\\lang (nr_lang.hpp) ---------------------------
// Whether the font in use draws every letter of a text (a language's own name, shown in another language's font)
inline bool drawable(const std::string& s) {
    ImFont* font = ImGui::GetFont();
    for (size_t i = 0; font && i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + n > s.size()) break;
        unsigned cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
        for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        i += n;
        if (cp >= 0x80 && (cp > 0xFFFF || !font->IsGlyphInFont(ImWchar(cp)))) return false;
    }
    return true;
}
// `reshade`: the ReShade Add-ons page (its texts use ReShade's font, not one of ours)
inline void draw_language(nr::ini_edit::Ini& ini, const std::string& folder, bool reshade = false) {
    static std::vector<std::pair<std::string, std::string>> files;
    static ULONGLONG listed = 0;
    const ULONGLONG now = GetTickCount64();
    if (files.empty() || now - listed > 2000) { files = nr::lang::list(folder); listed = now; }
    const std::string chosen = ini.get("NRControl", "Language", "");
    // a language's own name where the font draws it, else its file's name ("zh-CN")
    const auto label = [](const std::string& stem, const std::string& name) { return drawable(name) ? name : stem; };
    std::string shown = chosen.empty() ? "English" : chosen;
    for (const auto& [stem, name] : files) if (stem == chosen) shown = label(stem, name);
    if (ImGui::BeginCombo(tr("Language"), shown.c_str())) {
        for (const auto& [stem, name] : files)
            if (ImGui::Selectable(label(stem, name).c_str(), stem == chosen) && stem != chosen) ini.set("NRControl", "Language", stem);
        ImGui::EndCombo();
    }
    help("[NRControl] Language. The menus' texts from a file in dlssnr-amd\\lang; README.txt there says how to make "
         "one from English.txt. A text the file does not translate stays English.");
    const nr::lang::Table& t = nr::lang::table();
    if (!chosen.empty() && t.name != chosen)
        text_dim_wrapped("dlssnr-amd\\lang\\%s.txt was not found or is empty: English.", chosen.c_str());
    else if (nr::lang::suspended() && reshade)
        text_dim_wrapped("ReShade's font does not have this language's letters: English is shown.");
    else if (nr::lang::suspended())
        text_dim_wrapped("The menu's font does not have this language's letters: English is shown (@font in the language file).");
    else if (t.rejected)
        text_dim_wrapped("%d translations of this file are not used: their %%-values (%%d, %%s, ...) are not the English text's.",
                         t.rejected);
}
// Once a second: whether the font in use has every letter of the loaded language (the menus call it inside their
// frame); while it does not, the texts stay English (nr::lang::suspended) - empty boxes help no one.
inline void check_letters() {
    static ULONGLONG at = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - at < 1000) return;
    at = now;
    const nr::lang::Table& t = nr::lang::table();
    bool missing = false;
    if (ImFont* font = ImGui::GetFont())
        for (unsigned c : t.code_points)
            if (!font->IsGlyphInFont(ImWchar(c))) { missing = true; break; }
    nr::lang::suspended() = missing;
}
// [NRControl] TextScale: the text's size on top of the menu's (Menu size), spacing as it is
inline void draw_text_size(nr::ini_edit::Ini& ini) {
    static float pending = -1.f;
    float v = pending > 0.f ? pending : nr::parse_float(ini.get("NRControl", "TextScale", "1"), 1.0f);
    if (!(v >= 0.75f && v <= 2.f)) v = 1.f;
    if (ImGui::SliderFloat(tr("Text size"), &v, 0.75f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) pending = v;
    if (ImGui::IsItemDeactivatedAfterEdit() && pending > 0.f) {
        char s[16];
        std::snprintf(s, sizeof s, "%.2f", double(pending));
        ini.set("NRControl", "TextScale", s);
        pending = -1.f;
    }
    help("[NRControl] TextScale. The text alone, in a window of the same size: longer lines wrap, a table that no longer "
         "fits scrolls sideways (in NR Control on top of Menu size; on the ReShade page this page only, ReShade's own menu "
         "keeps its size). Taken when the slider is let go.");
}

// ---- Troubleshooting: what NR runs and how, in one text for a bug report ------------------------------------------
// `module`: the module the build stamp (Shared::asi_build) is of; `ini` / `log`: the files' paths.
inline std::string report_text(const nr::control::Shared* s, const char* module, const std::string& ini,
                               const std::string& log) {
    std::string t;
    char b[512];
    const auto add = [&](const char* fmt, auto... v) {
        std::snprintf(b, sizeof b, fmt, v...);
        t += b;
        t += "\n";
    };
    SYSTEMTIME now{};
    GetLocalTime(&now);
    add("NR report, %04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    if (s && s->asi_build[0]) add("%s %.*s", module, int(sizeof s->asi_build), s->asi_build);
    {   // (UTF-8: the report is shown and copied as UTF-8)
        std::wstring w(32768, L'\0');
        w.resize(GetModuleFileNameW(nullptr, w.data(), DWORD(w.size())));
        const std::string exe = nr::u8::narrow(w);
        add("Game: %s", exe.substr(exe.find_last_of('\\') + 1).c_str());
    }
    if (s && s->route) {
        add("Neural Rendering: %s", s->nr_on ? "on" : "off");
        if (s->frame_gen[0]) add("%.*s", int(sizeof s->frame_gen), s->frame_gen);
        if (s->render_width) add("Render %ux%u, output %ux%u", s->render_width, s->render_height, s->output_width, s->output_height);
        add("ACO Mode: %s%s%s", s->aco_state == 1 ? (s->aco_rdna3 ? "on (RDNA3)" : "on (RDNA4)")
                                : s->aco_state >= 2 && s->aco_state <= 4 ? "off" : "not known yet",
            s->aco_why[0] ? " - " : "", s->aco_why);
        add("%s", path_text(s->device_path));
        t += "\nNetwork ms = the network's own kernels (pre / post blocks included); everything else is processing. "
             "Timing starts at TOP_OF_PIPE.\n";
        t += "\nLayers (layer, where, network, size, network size, passes, network ms, processing ms, total ms, state):\n";
        static const char* const kWhere[] = {"after the upscaler", "before the upscaler", "Present"};
        for (const auto& r : s->layers) {
            if (!r.number) continue;
            char pass_text[64];
            if (r.fast_passes > 0.f) std::snprintf(pass_text, sizeof pass_text, "%.2f (fast)", double(r.fast_passes));
            else std::snprintf(pass_text, sizeof pass_text, "%u (legacy)", r.passes);
            add("  %u %s | %s | %s | %ux%u | %ux%u | %s | %.2f | %.2f | %.2f | %s%s%.*s", r.number,
                r.processing == 1 ? "wilsjo2" : "RenoDX", r.state ? kWhere[std::min(r.placement, 2u)] : "-",
                r.net < nr::control::kNoNet ? nr::network_codename(int(r.net)) : "-", r.width, r.height, r.nr_width, r.nr_height,
                pass_text,
                double(r.network_ms), double(std::max(0.f, r.gpu_ms - r.network_ms)), double(r.gpu_ms),
                r.state == 1 ? "running" : r.state == 2 ? "building" : r.state == 3 ? "not running" : "waiting",
                r.note[0] ? ": " : "", int(sizeof r.note), r.note);
            for (uint32_t i = 0; i < std::min(r.panel_lines, nr::control::kPanelLines); ++i)
                add("      %.*s", int(sizeof r.panel[i]), r.panel[i]);
        }
        if (!chain_rows(s) && s->width && s->gpu_ms > 0.f) {
            add("NR on %ux%u, network %ux%u, %u passes, %.2f ms (network %.2f)", s->width, s->height, s->nr_width,
                s->nr_height, s->passes, double(s->gpu_ms), double(s->network_ms));
            for (uint32_t i = 0; i < std::min(s->panel_lines, nr::control::kPanelLines); ++i)
                add("      %.*s", int(sizeof s->panel[i]), s->panel[i]);
        } else if (!chain_rows(s)) {
            add("  No layer is on: NR does not run");
        }
        if (s->state[0]) add("State: %.*s", int(sizeof s->state), s->state);
    }
    if (s && s->vram_known) {
        static const char* const kKind[] = {"images/buffers or route", "weights", "activations", "frame copies",
                                            "wilsjo2 layers", "NR Control", "RenoDX layers"};
        t += "\nVideo memory (MiB):\n";
        for (int o = 0; o < nr::kNetworkChoices + 1; ++o)   // the networks (nr::NetworkChoice), then none (vram::kOwners)
            for (int k = 0; k < nr::vram::kKinds; ++k)
                if (s->vram_kib[o][k])
                    add("  %s, %s: %.1f", o < nr::kNetworkChoices ? nr::network_codename(o) : "not a network's", kKind[k],
                        double(s->vram_kib[o][k]) / 1024.0);
        add("  this process: %.0f, budget %.0f, heap %.0f", double(s->vram_process_kib) / 1024.0,
            double(s->vram_budget_kib) / 1024.0, double(s->vram_heap_kib) / 1024.0);
    }
    // the settings file as it is, and the log's last lines
    const auto file_text = [](const std::string& path, size_t tail_lines) {
        std::string text;
        if (FILE* f = nr::u8::fopen(path.c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        if (tail_lines) {
            size_t at = text.size(), count = 0;
            while (at > 0 && count <= tail_lines) {
                at = text.rfind('\n', at - 1);
                if (at == std::string::npos) { at = 0; break; }
                ++count;
            }
            if (count > tail_lines) text = text.substr(at + 1);
        }
        return text;
    };
    // the residency and the events (nr_pe_events.hpp), oldest first, with their age
    {
        const uint64_t now_ms = GetTickCount64();
        const nr::pe::events::Residency r = nr::pe::events::residency();
        if (r.known) {
            static const char* const kMode[] = {"auto", "resident", "offload", "minimal"};
            static const char* const kState[] = {"not loaded", "serving", "kept in video memory", "moving to RAM", "in RAM"};
            add("\nResidency: %s, budget %.0f MiB, used %.0f MiB, reserve %.0f MiB%s%s", kMode[std::clamp(r.mode, 0, 3)],
                double(r.budget) / (1 << 20), double(r.usage) / (1 << 20), double(r.reserve) / (1 << 20),
                r.status.empty() ? "" : "; now: ", r.status.c_str());
            for (const auto& n : r.nets)
                add("  %s at %ux%u: %s", n.net >= 0 ? nr::network_codename(n.net) : "?", n.width, n.height,
                    kState[std::clamp(n.state, 0, 4)]);
        }
        const nr::pe::events::LastStop stop = nr::pe::events::last_stop();
        if (stop.ms) add("Last stop: Layer %d, %s, %.0f s ago", stop.layer, stop.why.c_str(), double(now_ms - stop.ms) / 1000.0);
        t += "\nEvents (age in seconds, category, what):\n";
        for (const auto& e : nr::pe::events::recent())
            add("  %8.1f %-8s %s%s", double(now_ms - e.ms) / 1000.0, nr::pe::events::category_name(e.category), e.text,
                e.count > 1 ? (" (x" + std::to_string(e.count) + ")").c_str() : "");
    }
    t += "\ndlssnr-amd.ini:\n" + file_text(ini, 0);
    t += "\ndlssnr-amd.log (last lines):\n" + file_text(log, 80);
    return t;
}
// The report copied to the clipboard and saved as `path`; what happened, in a line.
inline std::string copy_report(const std::string& text, const std::string& path) {
    bool saved = false;
    if (FILE* f = nr::u8::fopen(path.c_str(), "wb")) {
        saved = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
    }
    bool copied = false;
    const int wide = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()) + 1, nullptr, 0);
    if (wide > 0 && OpenClipboard(nullptr)) {
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, size_t(wide) * sizeof(wchar_t))) {
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()) + 1, static_cast<wchar_t*>(GlobalLock(g)), wide);
            GlobalUnlock(g);
            EmptyClipboard();
            copied = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
            if (!copied) GlobalFree(g);
        }
        CloseClipboard();
    }
    return tr(copied && saved ? "Copied to the clipboard, and saved as dlssnr-amd\\nr-report.txt."
              : copied        ? "Copied to the clipboard."
              : saved         ? "Saved as dlssnr-amd\\nr-report.txt (the clipboard could not be used)."
                              : "The report could not be copied or saved.");
}

// ---- NR Control's Global section: dlssnr-amd.ini's keys for every layer, written in place ------------------------
// A 0/1 key as a checkbox, written at once.
inline void ini_flag(nr::ini_edit::Ini& ini, const char* label, const char* section, const char* key, const char* def,
                     const char* text) {
    std::string v = ini.get(section, key, def);
    for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
    bool on = v == "1" || v == "true" || v == "on" || v == "yes";
    if (ImGui::Checkbox(tr(label), &on)) ini.set(section, key, on ? "1" : "0");
    help(text);
}
// A hotkey as text, written when the field is let go (the DLL reads the file again and takes it at once).
inline void ini_hotkey(nr::ini_edit::Ini& ini, const char* label, const char* section, const char* text,
                       const char* key = "Hotkey", const char* def = "") {
    static std::unordered_map<std::string, std::string> editing;
    auto it = editing.find(label);
    std::string& t = it != editing.end() ? it->second : (editing[label] = ini.get(section, key, def));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s", t.c_str());
    if (ImGui::InputText(tr(label), buf, sizeof buf)) t = buf;
    if (ImGui::IsItemDeactivatedAfterEdit()) { ini.set(section, key, t); editing.erase(label); }
    else if (!ImGui::IsItemActive()) editing.erase(label);
    help(text);
}

// [NRControl] Layers (off by default: layers were taken for Passes): off, the chain is its first [LayerN] alone
// (read_chain keeps that one too); the others stay in the file, not run, not shown
inline bool layers_on(const nr::ini_edit::Ini& ini) {
    std::string v = ini.get("NRControl", "Layers", "0");
    for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
    return v == "1" || v == "true" || v == "on" || v == "yes";
}
// N of a [LayerN] section, 0 for any other
inline int layer_section_number(const std::string& section) {
    if (section.size() < 6) return 0;
    std::string head = section.substr(0, 5);
    for (char& c : head) c = char(std::tolower(static_cast<unsigned char>(c)));
    const std::string digits = section.substr(5);
    if (head != "layer" || digits.empty() || digits.size() > 4 || digits.find_first_not_of("0123456789") != std::string::npos)
        return 0;
    return std::atoi(digits.c_str());
}
// The file's [LayerN] that run as the menu shows them: every one with Layers on, else the first (0: none at all)
inline bool layer_shown(const nr::ini_edit::Ini& ini, int n) {
    if (n < 1) return false;
    if (layers_on(ini)) return true;
    int first = 0;
    for (const std::string& sec : ini.sections())
        if (const int k = layer_section_number(sec); k > 0 && (!first || k < first)) first = k;
    return n == first;
}

// ---- Hotkeys: every key NR reads, in one section (NR Control's and the Add-ons page's) ----------------------------
// The keys stay in the sections that have always held them ([NRControl] MenuKey, [RenoDX] NRToggleKey /
// NRScreenshotKey, [Preprocess] Hotkey, [Network] Hotkey, [LayerN] ToggleKey); only the menu gathers them.
// A key as dlssnr-amd.ini writes it, from ImGui's: empty for one the DLL's hotkeys cannot read (parse_hotkey:
// F1-F24, A-Z, 0-9, Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock).
inline std::string key_word(ImGuiKey k) {
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F24) return "F" + std::to_string(int(k) - int(ImGuiKey_F1) + 1);
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return std::string(1, char('A' + (int(k) - int(ImGuiKey_A))));
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return std::string(1, char('0' + (int(k) - int(ImGuiKey_0))));
    switch (k) {
        case ImGuiKey_Insert: return "Insert"; case ImGuiKey_Delete: return "Delete"; case ImGuiKey_Home: return "Home";
        case ImGuiKey_End: return "End"; case ImGuiKey_PageUp: return "PageUp"; case ImGuiKey_PageDown: return "PageDown";
        case ImGuiKey_Pause: return "Pause"; case ImGuiKey_ScrollLock: return "ScrollLock";
        default: return {};
    }
}
// A key's text in one form ("ctrl+shift+f10"), whatever its order and case, to find two actions on one key; empty: none.
inline std::string key_id(const std::string& text) {
    bool ctrl = false, shift = false, alt = false;
    std::string key, part;
    const auto take = [&] {
        if (part == "ctrl" || part == "control") ctrl = true;
        else if (part == "shift") shift = true;
        else if (part == "alt") alt = true;
        else if (!part.empty()) key = part;
        part.clear();
    };
    for (char c : text) {
        if (c == '+') take();
        else if (!std::isspace(static_cast<unsigned char>(c))) part += char(std::tolower(static_cast<unsigned char>(c)));
    }
    take();
    if (key.empty()) return {};
    return std::string(ctrl ? "ctrl+" : "") + (shift ? "shift+" : "") + (alt ? "alt+" : "") + key;
}
// The hotkey taking its key: which one, until when (each frame it is drawn renews it). While it lasts the menus pass
// every key to it (not the menu's own key, not Esc) and their owner pauses NR's hotkeys (nr::pe::pause_hotkeys).
struct KeyCapture { std::string id; ULONGLONG until = 0; };
inline KeyCapture& key_capture() { static KeyCapture c; return c; }
inline bool capturing_key() { return !key_capture().id.empty() && GetTickCount64() < key_capture().until; }

struct HotkeysMenu {
    bool menu_key = false;              // NR Control's own key ([NRControl] MenuKey; the .asi's menu)
    // keys something else here already uses, and what (OptiScaler's menu key, ReShade's overlay)
    std::vector<std::pair<std::string, std::string>> taken;
};
// A virtual key as dlssnr-amd.ini writes keys ("End", "F7"); empty for one parse_hotkey cannot read.
inline std::string vk_word(int vk) {
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    switch (vk) {
        case 0x2D: return "Insert"; case 0x2E: return "Delete"; case 0x24: return "Home"; case 0x23: return "End";
        case 0x21: return "PageUp"; case 0x22: return "PageDown"; case 0x13: return "Pause"; case 0x91: return "ScrollLock";
        default: return {};
    }
}
inline void draw_hotkeys(nr::ini_edit::Ini& ini, nr::ini_edit::KeyValues& renodx, const HotkeysMenu& m) {
    if (const char* e = std::getenv("NR_CONTROL_HOTKEYS_OPEN")) {   // tests: open, in view
        ImGui::SetNextItemOpen(e[0] != '0');
        if (e[0] != '0') ImGui::SetScrollHereY(0.f);
    }
    if (!section("Hotkeys")) {
        if (key_capture().id.size()) key_capture() = KeyCapture{};
        return;
    }
    struct Row { std::string label, section, key, def, help; bool renodx = false; };
    std::vector<Row> rows;
    if (m.menu_key)
        rows.push_back({tr("Open / close NR Control"), "NRControl", "MenuKey", "End",
                        tr("[NRControl] MenuKey. Esc closes the menu too. With no key NR Control cannot be opened until one "
                           "is set again in dlssnr-amd.ini.")});
    rows.push_back({tr("NR on / off (this run)"), "RenoDX", "NRToggleKey", "F6",
                    tr("[RenoDX] NRToggleKey. Every layer off or on again, as the switch at the top of this menu; for this "
                       "run only, never written to the file."), true});
    rows.push_back({tr("Screenshot"), "RenoDX", "NRScreenshotKey", "F5",
                    tr("[RenoDX] NRScreenshotKey. The same frame without and with NR, into NR Screenshots in the game's "
                       "folder."), true});
    rows.push_back({tr("Preprocess on / off (this run)"), "Preprocess", "Hotkey", "Ctrl+F10",
                    tr("[Preprocess] Hotkey. The whole preprocess off or on for this run, never written to the file.")});
    rows.push_back({tr("Next network"), "Network", "Hotkey", "Ctrl+F11",
                    tr("[Network] Hotkey. Cycles through the networks that can run (written to [Network] Model).")});
    for (const std::string& sec : ini.sections()) {
        if (!layer_shown(ini, layer_section_number(sec))) continue;   // (Layers off: the first layer's only)
        const std::string digits = sec.substr(5);
        char label[64], text[320];
        std::snprintf(label, sizeof label, tr("Layer %s on / off (this run)"), digits.c_str());
        std::snprintf(text, sizeof text, tr("[%s] ToggleKey. This layer off or on again for this run only (its Enabled box "
                                            "stays as the file has it). None by default."), sec.c_str());
        rows.push_back({label, sec, "ToggleKey", "", text});
    }
    std::vector<std::string> value(rows.size()), id(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        value[i] = ini.get(rows[i].section, rows[i].key, rows[i].def);
        id[i] = key_id(value[i]);
    }
    const auto write = [&](const Row& r, const std::string& text) {
        if (r.renodx) {
            const int k = key_index(r.key.c_str());
            if (k >= 0) { renodx.s(k) = text; renodx.store(ini); }
        } else {
            ini.set(r.section, r.key, text);
        }
    };
    ImGui::TextWrapped(tr("Click a key, then press the new one (with Ctrl, Shift or Alt if wanted); Esc cancels. Applied at once."));
    KeyCapture& cap = key_capture();
    if (!cap.id.empty() && GetTickCount64() >= cap.until) cap = KeyCapture{};   // the section was closed meanwhile
    const float u = ImGui::GetFontSize() / 17.f;
    if (ImGui::BeginTable("hotkeys", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        // the action and its key fixed, the clash note in what is left (it wraps there: in a column of no width it
        // wrapped after every letter)
        ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##clash", ImGuiTableColumnFlags_WidthStretch);
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            const std::string row_id = r.section + "." + r.key;
            ImGui::PushID(row_id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.label.c_str());
            help(r.help.c_str());
            ImGui::TableNextColumn();
            const bool taking = cap.id == row_id;
            const std::string shown = taking ? tr("Press a key... (Esc: cancel)") : value[i].empty() ? tr("(none)") : value[i];
            if (ImGui::Button(shown.c_str(), ImVec2(230 * u, 0))) {
                if (taking) cap = KeyCapture{};
                else { cap.id = row_id; cap.until = GetTickCount64() + 300; }
            } else if (taking) {
                cap.until = GetTickCount64() + 300;
                const ImGuiIO& io = ImGui::GetIO();
                for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                    if (!ImGui::IsKeyPressed(ImGuiKey(k), false)) continue;
                    if (ImGuiKey(k) == ImGuiKey_Escape) { cap = KeyCapture{}; break; }
                    const std::string word = key_word(ImGuiKey(k));
                    if (word.empty()) continue;   // a modifier, or a key the hotkeys cannot read
                    write(r, std::string(io.KeyCtrl ? "Ctrl+" : "") + (io.KeyShift ? "Shift+" : "") + (io.KeyAlt ? "Alt+" : "") + word);
                    cap = KeyCapture{};
                    break;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Clear")) && !value[i].empty()) write(r, "");
            if (!r.def.empty() && key_id(value[i]) != key_id(r.def)) {
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Default"))) write(r, r.def);
            }
            ImGui::TableNextColumn();
            std::string clash;
            for (size_t j = 0; j < rows.size(); ++j)
                if (j != i && !id[i].empty() && id[j] == id[i]) clash += (clash.empty() ? "" : ", ") + rows[j].label;
            for (const auto& [key, by] : m.taken)
                if (!id[i].empty() && id[i] == key_id(key)) clash += (clash.empty() ? "" : ", ") + by;
            if (!clash.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.3f, 1.0f));
                ImGui::TextWrapped(tr("also: %s"), clash.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// NR on / off and the screenshot keys, NR Control's for every layer ([RenoDX], as the OptiScaler route keeps them).
// `take`: the screenshot button; `status`: the screenshot's last word (empty: none).
inline void draw_nr_switch(nr::ini_edit::Ini& ini, nr::ini_edit::KeyValues& renodx, const std::function<void()>& take,
                           const std::string& status, const char* note = nullptr) {
    if (!section("Screenshot and NR at start")) return;
    bool changed = false;
    for (int i = 0; i < nr::renodx_keys::kCount; ++i) {
        const std::string name = nr::renodx_keys::kKeys[i].name;
        // (the keys: Hotkeys)
        if (name == "NeuralUplift" || (name.rfind("NRScreenshot", 0) == 0 && name != "NRScreenshotKey"))
            changed |= draw_key(renodx, i);
    }
    if (changed) renodx.store(ini);
    ImGui::TextDisabled(tr("Neural Rendering above: whether NR is on when the game starts. Keys: see Hotkeys."));
    if (ImGui::Button(tr("Take a screenshot now")) && take) take();
    if (note) text_dim_wrapped("%s", note);
    if (!status.empty()) ImGui::TextWrapped(tr("Screenshot: %s"), status.c_str());
}

// [Network]: which network runs (the three by the DLL's codenames); one that cannot run in this run is greyed out with
// the reason the DLL found (net_why), and the DLL skips it. (not "Network": the RenoDX table has a group of that name,
// and the same label is the same header)
struct NetworkOptions {
    const char* resize_help = NR_N("[Network] Resize History. Only when NR runs before the upscaler and the game changes its render size\n"
                              "(dynamic resolution):\n"
                              "Off (default): NR's previous result is dropped and NR starts again at the new size, as NVIDIA's DLL does.\n"
                              "On: the previous result is scaled to the new size and kept. Steadier on our tests when the size changes\n"
                              "often, but not what NVIDIA's DLL does. Games with jittered input (most DLSS games) hide most of the\n"
                              "restart, so the default is off.");
};
// [NRControl] Layers on the Network page (layers were taken for Passes): off (the default), one NR run whose choices
// and settings are the NR settings page; on, the NR chain page and a page a layer. The note under it says so in words.
inline void draw_layers_switch(nr::ini_edit::Ini& ini) {
    int sections = 0;
    for (const std::string& sec : ini.sections()) if (layer_section_number(sec) > 0) ++sections;
    bool on = layers_on(ini);
    if (ImGui::Checkbox(tr("Layers"), &on)) {
        ini.set("NRControl", "Layers", on ? "1" : "0");
        // Compare's layer no longer runs: the final picture
        const int compared = std::atoi(ini.get("Inspect", "Layer", "0").c_str());
        if (!on && compared > 0 && sections && !layer_shown(ini, compared)) ini.set("Inspect", "Layer", "0");
    }
    help("[NRControl] Layers. On: several NR layers (the NR chain page), each a whole NR run of its own with its own "
         "processing, network, place and settings. Off (default): one NR run, its choices and settings on the NR settings "
         "page. Layers are not the way to run NR more than once on a frame: that is Passes.");
    ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1.0f), "%s", tr("For ordinary stacking use Passes."));
}

inline void draw_network_options(nr::ini_edit::Ini& ini, const nr::control::Shared* s, const NetworkOptions& o) {
    if (const char* e = std::getenv("NR_CONTROL_GLOBAL_OPEN")) ImGui::SetNextItemOpen(e[0] != '0');   // tests
    if (!section("Network model and options", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const int file_model = std::max(0, nr::network_choice(ini.get("Network", "Model", "CC_SMALL_FP8")));
    const int shown = s && s->net_known && s->net_want < nr::control::kNoNet ? int(s->net_want) : file_model;
    if (ImGui::BeginCombo(tr("Network##model"), nr::network_codename(shown))) {
        for (int n : nr::kNetworkOrder) {
            const char* why = s && s->net_known ? tr_text(s->net_why[n]) : "";
            const bool ok = !why[0];
            if (ImGui::Selectable(nr::network_codename(n), n == shown, ok ? 0 : ImGuiSelectableFlags_Disabled))
                ini.set("Network", "Model", nr::network_codename(n));
            if (!ok) {   // the reason under it, wrapped (a popup grows with its widest line)
                ImGui::Indent();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 26.f * ImGui::GetFontSize());
                ImGui::TextDisabled("%s", why);
                ImGui::PopTextWrapPos();
                ImGui::Unindent();
            }
        }
        ImGui::EndCombo();
    }
    help("[Network] Model. CC_SMALL_FP8: 310.9.16's default network. CC_SMALL_FP8_EXPERIMENTAL: ours, "
         "CC_SMALL_FP8 made faster. CC_SILVER_AARDWOLD: "
         "310.8.0's network (310.9.16's preset 1). CC_SILVER_AARDWOLD_INT4_MIXED: that network with part "
         "of its arithmetic in int4, faster, a slightly different picture. Switching builds the new network in "
         "the background; the running one serves until it is ready.");
    for (int n : nr::kNetworkOrder)
        if (s && s->net_known && s->net_why[n][0]) text_dim_wrapped("%s: %s", nr::network_codename(n), tr_text(s->net_why[n]));
    // ACO Mode (not a setting): whether this run's network is ACO's machine code, and why not
    if (s) {
        switch (s->aco_state) {
            case 1:
                ImGui::TextWrapped(tr("ACO Mode: on in this run (machine code for %s)"),
                                   s->aco_rdna3 ? tr("RDNA3: RX 7000, Radeon 700M / 800M / 8000S") : tr("RDNA4: RX 9000"));
                break;
            case 2: case 3: case 4:
                ImGui::TextWrapped(tr("ACO Mode: off in this run%s%s - an RX 9000 runs the network compiled by the AMD driver; "
                                      "RX 7000 and Radeon 700M / 800M / 8000S graphics have no NR"),
                                   s->aco_why[0] ? ": " : "", tr_text(s->aco_why));
                break;
            default: ImGui::TextDisabled("%s", tr("ACO Mode: not known until NR first runs")); break;
        }
        help("ACO Mode: the network runs as machine code made in advance by the ACO compiler (the one the Linux version "
             "uses), loaded by the AMD driver. It is not a setting; when the driver cannot take it, the line above and "
             "dlssnr-amd.log say why.");
        text_dim_wrapped("%s", tr(path_text(s->device_path)));
    }
    {
        const std::string k = ini.get("Network", "Hotkey", "Ctrl+F11");
        ImGui::TextDisabled(tr("Next network: %s (see Hotkeys)"), k.empty() ? tr("no key") : k.c_str());
    }
    ini_flag(ini, "Sound##net", "Network", "Sound", "1",
             "[Network] Sound. A cue on every switch: one note CC_SMALL_FP8, a higher one "
             "CC_SMALL_FP8_EXPERIMENTAL, two going down CC_SILVER_AARDWOLD, two going up "
             "CC_SILVER_AARDWOLD_INT4_MIXED.");
    static const char* const residencies[] = {NR_N("auto"), NR_N("resident"), NR_N("offload"), NR_N("minimal")};
    const std::string r = ini.get("Network", "Residency", "auto");
    int ri = 0;
    for (int k = 0; k < 4; ++k) if (r == residencies[k]) ri = k;
    // "##net": the status section above has a tree node named "Residency" in the same ID scope (a CollapsingHeader
    // pushes no ID); with one ID the combo did not open
    const char* shown_residencies[4];   // the file keeps the English word; the menu shows its translation
    for (int k = 0; k < 4; ++k) shown_residencies[k] = tr(residencies[k]);
    if (ImGui::Combo(tr("Residency##net"), &ri, shown_residencies, 4)) ini.set("Network", "Residency", residencies[ri]);
    help("[Network] Residency: what is kept of a network not in use. The networks share one "
         "activation memory, so mostly their weights. auto (default): the weights stay in video "
         "memory, switching back is quick; moved to RAM when video memory runs short, freed when RAM "
         "does too. resident: kept whole, switching back is instant, the most video memory. offload: "
         "the weights in RAM, all video memory freed. minimal: everything freed, switching back builds "
         "anew. While the other network gets ready, the running one keeps serving. NR off for 10 s: "
         "offload and minimal do the same to the running network, auto only when video memory is short.");
    static const char* const osds[] = {NR_N("off"), NR_N("switch"), NR_N("always"), NR_N("cost")};
    const std::string os = ini.get("Network", "OSD", "switch");
    int oi = 1;
    for (int k = 0; k < 4; ++k) if (os == osds[k]) oi = k;
    const char* shown_osds[4];
    for (int k = 0; k < 4; ++k) shown_osds[k] = tr(osds[k]);
    if (ImGui::Combo(tr("OSD"), &oi, shown_osds, 4)) ini.set("Network", "OSD", osds[oi]);
    help("[Network] OSD: the running network in the corner of the picture. switch (default): while "
         "a switch builds, its progress; then the new network for 3 s. always: all the time. cost: "
         "all the time, with each layer's GPU time split into the network and its processing (a run, as the "
         "layer table splits it), and their sums. off: never.");
    ini_flag(ini, "Style grade (Natural / Cinematic)", "Network", "Style Grade", "1",
             "[Network] Style Grade. On (default): Natural and Cinematic are graded after the network as NVIDIA's DLL "
             "does. Off: the picture these styles had up to 0.0.6, without that grade. Standard is not affected. "
             "Takes effect at once.");
    ini_flag(ini, "Motion without depth", "Network", "Motion Without Depth", "1",
             "[Network] Motion Without Depth. Where the network's previous-frame lookup reads its motion vector.\n"
             "On (default): the pixel's own vector, depth not used, as NVIDIA's DLL does.\n"
             "Off: with depth, the vector of the nearest of the pixel and its four diagonal neighbours.\n"
             "Only moving edges between frames with motion vectors and depth differ. Takes effect after a restart.");
    ini_flag(ini, "Resize history", "Network", "Resize History", "0", o.resize_help);
    ini_flag(ini, "Memory guard", "Network", "Memory Guard", "1",
             "[Network] Memory Guard. On (default): a network is built only while 1 GB of video memory stays free for "
             "the game, and the running one steps aside while video memory runs out. Off: no such check - NR builds and "
             "runs in whatever video memory is left; a game left short of it may stutter or fail to load its own data. "
             "Applied at once.");
}

// [Inspect]: NR Control's comparison for the whole NR chain. `where`: what the
// route's text says of where the final picture's comparison is made.
// `zoom`: the route scales the halves of side by side (the .asi's routes); without it each half is shown at 1:1.
inline void draw_inspect(nr::ini_edit::Ini& ini, const char* where) {
    if (!section("Compare")) return;
    auto num = [&](const char* key, float def) {
        const std::string v = ini.get("Inspect", key, "");
        return v.empty() ? def : nr::parse_float(v, def);
    };
    static const char* const modes[] = {NR_N("Off"), NR_N("Side by side"), NR_N("Wipe")};
    int mode = std::clamp(int(num("Compare", 0.f)), 0, 2);
    if (ImGui::Combo(tr("Compare##insp"), &mode, modes, 3)) ini.set("Inspect", "Compare", std::to_string(mode));
    help("[Inspect] Compare. The picture without NR against the picture with it, on the screen. Side by side: each "
         "half the whole picture; Wipe: one picture cut at Split.");
    // what is compared: the final picture, or one layer
    std::vector<int> layers;
    bool any_section = false;
    for (const std::string& sec : ini.sections()) {
        const int n = layer_section_number(sec);
        if (n < 1) continue;
        any_section = true;
        if (!layer_shown(ini, n)) continue;   // Layers off: the first layer's only
        const std::string en = ini.get(sec, "Enabled", "1");
        if (en == "0" || en == "false") continue;
        layers.push_back(n);
    }
    // no [LayerN] at all: the chain's one layer of [RenoDX] / [wilsjo2] (row 1); every [LayerN] off: none
    const bool implicit = !any_section;
    if (implicit) layers.push_back(1);
    const int layer = std::max(0, int(num("Layer", 0.f)));
    char shown[48];
    if (layer) std::snprintf(shown, sizeof shown, tr("Layer %d"), layer); else std::snprintf(shown, sizeof shown, "%s", tr("The final picture"));
    if (ImGui::BeginCombo(tr("What##insp"), shown)) {
        if (ImGui::Selectable(tr("The final picture"), layer == 0)) ini.set("Inspect", "Layer", "0");
        for (int n : layers) {
            char name[48];
            std::snprintf(name, sizeof name, tr(implicit ? "Layer %d (the one layer)" : "Layer %d"), n);
            if (ImGui::Selectable(name, layer == n)) ini.set("Inspect", "Layer", std::to_string(n));
        }
        ImGui::EndCombo();
    }
    help(where);
    auto slider = [&](const char* label, const char* key, float lo, float hi, float def, const char* text) {
        static std::unordered_map<std::string, float> pending;
        const auto it = pending.find(key);
        float v = it != pending.end() ? it->second : num(key, def);
        if (ImGui::SliderFloat(tr(label), &v, lo, hi, "%.2f", ImGuiSliderFlags_AlwaysClamp)) pending[key] = v;
        if (ImGui::IsItemDeactivatedAfterEdit() && pending.count(key)) {
            char t[32];
            std::snprintf(t, sizeof t, "%.2f", double(pending[key]));
            ini.set("Inspect", key, t);
            pending.erase(key);
        }
        help(text);
    };
    if (mode == 2) slider("Split##insp", "Split", 0.f, 1.f, 0.5f, "[Inspect] Split. Where the wipe cuts, 0 .. 1 of the width.");
    if (mode == 1)
        slider("Zoom##insp", "Zoom", 1.f, 2.f, 1.f, "[Inspect] Zoom. 1: each half the whole picture (bars above "
                                                     "and below); 2: the half filled (its sides cropped).");
    if (mode) ini_flag(ini, "Swap sides##insp", "Inspect", "Swap", "0", "[Inspect] Swap. NR on the left.");
    if (mode) {
        ini_flag(ini, "Tags##insp", "Inspect", "Tags", "0",
                 "[Inspect] Tags. \"NR off\" / \"NR on\" along the top of the two sides, with the menu open or closed.");
        const std::string t = ini.get("Inspect", "Tags", "0");
        if (t == "1" || t == "true") slider("Tag size##insp", "TagScale", 0.5f, 4.f, 1.5f, "[Inspect] TagScale. The tags' size.");
    }
    ini_flag(ini, "Apply NR##insp", "Inspect", "ApplyModel", "1",
             "[Inspect] ApplyModel. Off: the compared layers still run (the same cost), their answer is not put on the "
             "picture - the picture without NR, to switch back and forth with.");
    ImGui::SeparatorText(tr("Debug views (one layer)"));
    if (!layer) {
        ImGui::TextDisabled(tr("Choose a layer in What for its debug views."));
        return;
    }
    static const char* const views[] = {NR_N("Off"), NR_N("Network input"), NR_N("Network answer"), NR_N("Change x20")};
    int dv = std::clamp(int(num("DebugView", 0.f)), 0, 3);
    if (ImGui::Combo(tr("Debug view##insp"), &dv, views, 4)) ini.set("Inspect", "DebugView", std::to_string(dv));
    help("[Inspect] DebugView (the chosen layer). Network input / answer: what the network was shown and what it "
         "gave back; Change x20: its change around grey (flat grey: no change). wilsjo2 layers as the fork shows "
         "them; RenoDX layers in the network's own encoding (on an SDR picture the picture's own values).");
    ini_flag(ini, "Skin mask##insp", "Inspect", "SkinMask", "0",
             "[Inspect] SkinMask (the chosen layer, wilsjo2): the skin weight instead of the picture, white = skin.");
    ini_flag(ini, "Hold frame##insp", "Inspect", "HoldFrame", "0",
             "[Inspect] HoldFrame (the chosen layer). Its input is kept once and given to it every frame: the same frame "
             "for tuning its settings (its history settles on it; motion and the frames around it are not "
             "representative). Off: back to the live frame.");
    ini_flag(ini, "Spatial outlines##insp", "Inspect", "SpatialOutlines", "0",
             "[Inspect] SpatialOutlines (the chosen layer, wilsjo2 with SpatialCompression): its working region (orange) "
             "and centre band (cyan) over the picture, with the menu open or closed.");
}

// [HudFix]: NR's change kept off the game's HUD (the layers at Present). Mode: Off, Auto (the best method that can run
// this frame) or one method; a method that cannot run here is greyed out in the list with its reason. `why` by Mode's
// number (2 frame generation's HUD-less frame, 3 captured before the HUD, 4 estimated): empty when it can run. `now`:
// what HUD FIX does this frame, in the route's words.
struct HudFixMenu {
    std::string why[5];
    std::string now;
    int default_mode = 0;   // Mode where the file does not say it: Off (both routes)
};
inline int hudfix_mode(const nr::ini_edit::Ini& ini, int default_mode = 0) {   // as nr_pe_config.cpp reads Mode
    std::string v = ini.get("HudFix", "Mode", default_mode == 0 ? "off" : "auto");
    for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (v == "off" || v == "0") return 0;
    if (v == "game" || v == "fg" || v == "2") return 2;
    if (v == "captured" || v == "capture" || v == "3") return 3;
    if (v == "estimated" || v == "estimate" || v == "4") return 4;
    if (v == "auto" || v == "1") return 1;
    return default_mode;   // (anything else: as nr_pe_config.cpp, the route's default)
}
inline void draw_hudfix(nr::ini_edit::Ini& ini, const HudFixMenu& m) {
    if (const char* e = std::getenv("NR_CONTROL_GLOBAL_OPEN")) ImGui::SetNextItemOpen(e[0] != '0');   // tests
    if (!section("HUD FIX")) return;
    static const char* const keys[] = {"off", "auto", "game", "captured", "estimated"};
    static const char* const names[] = {NR_N("Off"), NR_N("Auto"), NR_N("Frame generation's HUD-less frame"),
                                        NR_N("Captured before the HUD"), NR_N("Estimated")};
    const int mode = hudfix_mode(ini, m.default_mode);
    const bool unusable = mode >= 2 && !m.why[mode].empty();
    std::string shown = tr(names[mode]);
    if (unusable) shown += tr(" (cannot run here)");
    if (ImGui::BeginCombo(tr("Method##hud"), shown.c_str())) {
        for (int i = 0; i < 5; ++i) {
            const bool grey = i >= 2 && !m.why[i].empty();
            ImGui::BeginDisabled(grey);
            if (ImGui::Selectable(tr(names[i]), mode == i)) ini.set("HudFix", "Mode", keys[i]);
            ImGui::EndDisabled();
            if (grey) {
                ImGui::Indent();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 26.f * ImGui::GetFontSize());
                ImGui::TextDisabled("%s", tr_text(m.why[i].c_str()));
                ImGui::PopTextWrapPos();
                ImGui::Unindent();
            }
        }
        ImGui::EndCombo();
    }
    help("[HudFix] Mode. HUD FIX keeps the game's HUD (crosshair, bars, subtitles, menus) as the game drew it while NR "
         "changes the rest; for the layers placed at Present. Off (default): the HUD goes through NR like the rest. Auto: "
         "the best method that can run, each frame. Frame generation's HUD-less frame: the frame without its HUD that the game hands its frame "
         "generation (FSR 3.1, XeSS, DLSS-G through Streamline or NGX; D3D12 games); NR runs on it and the HUD is put "
         "back as it was; where the game keeps it until the present, frame generation is given NR's answer in its "
         "place. OptiScaler's own frame generation gets no "
         "such frame from the game: Captured serves those games. Captured before the HUD: "
         "the frame copied where the game starts drawing its HUD; NR runs on it and the HUD is put back as it was. "
         "Estimated: pixels that stay put while the picture moves are taken for the HUD and kept (NR still sees the "
         "HUD). A method that cannot run here is greyed out with its reason.");
    if (unusable) ImGui::TextWrapped(tr("%s cannot run here: %s"), names[mode], tr_text(m.why[mode].c_str()));
    if (!m.now.empty()) text_dim_wrapped("Now: %s", tr_text(m.now.c_str()));
    ini_flag(ini, "Show mask##hud", "HudFix", "ShowMask", "0",
             "[HudFix] ShowMask. The HUD as HUD FIX sees it instead of the picture: white = kept as the game drew it.");
}

// [Inspect]'s marks over the picture, in NR Control's overlay with the menu open or closed: the comparison's tags
// ("NR off" / "NR on" flanking the divider along the top, each clipped to its own side; Tags, TagScale) and a wilsjo2
// layer's peripheral-compression outlines (SpatialOutlines: its working region orange, its centre band cyan), drawn
// where and as the fork's overlay draws them.
struct InspectMarks {
    uint32_t compare = 0;
    float split = 0.5f, tag_scale = 1.5f;
    bool swap = false, tags = false;
    bool outlines = false;                 // the rects below are the chosen layer's, its layout active
    float centre[4] = {}, work[4] = {};    // fractions of the frame: left, top, right, bottom
    bool any() const { return (tags && compare) || outlines; }
};
// [Inspect]'s keys as the file has them (the rects are the caller's).
inline InspectMarks inspect_marks(const nr::ini_edit::Ini& ini) {
    InspectMarks m;
    const auto num = [&](const char* key, float def) {
        const std::string v = ini.get("Inspect", key, "");
        return v.empty() ? def : nr::parse_float(v, def);
    };
    const auto flag = [&](const char* key) {
        const std::string v = ini.get("Inspect", key, "0");
        return v == "1" || v == "true";
    };
    m.compare = uint32_t(std::clamp(int(num("Compare", 0.f)), 0, 2));
    m.split = std::clamp(num("Split", 0.5f), 0.f, 1.f);
    m.tag_scale = std::clamp(num("TagScale", 1.5f), 0.5f, 4.f);
    m.swap = flag("Swap");
    m.tags = flag("Tags");
    return m;
}
inline void draw_inspect_marks(const InspectMarks& m) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();   // over the picture, under the menu
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    if (screen.x < 2.f || screen.y < 2.f) return;
    if (m.outlines) {
        const auto rect = [&](const float r[4], ImU32 colour) {
            const ImVec2 lo{r[0] * screen.x, r[1] * screen.y}, hi{r[2] * screen.x, r[3] * screen.y};
            dl->AddRect(lo, hi, IM_COL32(0, 0, 0, 220), 0.f, 0, 4.f);
            dl->AddRect(lo, hi, colour, 0.f, 0, 2.f);
        };
        rect(m.work, IM_COL32(255, 115, 0, 255));
        rect(m.centre, IM_COL32(0, 210, 255, 255));
    }
    if (!m.tags || !m.compare) return;
    const float cut = (m.compare == 1 ? 0.5f : m.split) * screen.x;
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * m.tag_scale, margin = 10.f * m.tag_scale;
    const char* left = tr(m.swap ? "NR on" : "NR off");
    const char* right = tr(m.swap ? "NR off" : "NR on");
    // the text's size at `size` (ImGui::CalcTextSize at the font's own, scaled: ReShade's ImGui table has no
    // ImFont::CalcTextSizeA)
    const auto measure = [&](const char* text) {
        const ImVec2 t = text_size(text);
        const float k = size / std::max(ImGui::GetFontSize(), 1.f);
        return ImVec2(t.x * k, t.y * k);
    };
    const auto tag = [&](const char* text, float x, ImVec2 clip_lo, ImVec2 clip_hi) {
        const ImVec2 ts = measure(text);
        x = std::min(std::max(x, 0.f), screen.x - ts.x);
        const float y = std::max(std::min(margin, screen.y - ts.y - margin), 0.f);
        dl->PushClipRect(clip_lo, clip_hi, true);
        dl->AddText(font, size, ImVec2(x + 2.f, y + 2.f), IM_COL32(0, 0, 0, 210), text);
        dl->AddText(font, size, ImVec2(x, y), IM_COL32(255, 255, 255, 255), text);
        dl->PopClipRect();
    };
    const ImVec2 ls = measure(left);
    tag(left, cut - margin - ls.x, ImVec2(0.f, 0.f), ImVec2(cut, screen.y));
    tag(right, cut + margin, ImVec2(cut, 0.f), ImVec2(screen.x, screen.y));
}
// A wilsjo2 layer's outlines from its layout (nr::wilsjo2::SpatialLayout, the fork's centerBounds / workBounds).
template <class Layout> inline bool spatial_rects(const Layout& l, float centre[4], float work[4]) {
    if (!l.active || !l.native_w || !l.native_h) return false;
    const float n[2] = {float(l.native_w), float(l.native_h)};
    for (int i = 0; i < 2; ++i) {
        const auto& a = l.axis[i];
        centre[i] = (a.band_centre - a.half_band) / n[i];
        centre[i + 2] = (a.band_centre + a.half_band) / n[i];
        work[i] = (a.band_centre - a.half_band - a.allotted[0]) / n[i];
        work[i + 2] = (a.band_centre + a.half_band + a.allotted[1]) / n[i];
    }
    return true;
}

// What the game does not hand a layer that matters to it as it runs: one line each, from the '-' lines of the layer's
// engine-data panel (nr_engine_panel.hpp, the Engine data page's test), what is missing -> what it affects. `ini`: the
// layer's Passes (fast) method (Feedback / Hybrid need motion vectors); nothing is drawn when nothing is missing.
inline void draw_missing_data(const nr::control::LayerStatus* rows, uint32_t count, const nr::ini_edit::Ini* ini) {
    const uint32_t now = uint32_t(GetTickCount64());
    static const ImVec4 warn(1.f, .55f, .3f, 1.f);   // nr_engine_panel's colour for a line nothing covers
    std::vector<const nr::control::LayerStatus*> live;
    for (uint32_t i = 0; i < count; ++i)
        if (rows[i].number && rows[i].state == 1u && rows[i].updated && now - rows[i].updated < 1500) live.push_back(&rows[i]);
    std::sort(live.begin(), live.end(), [](auto* a, auto* b) { return a->number < b->number; });
    const auto feedback = [&](uint32_t layer) {
        if (!ini) return false;
        const std::string own = "Layer" + std::to_string(layer), sec = ini->has_section(own) ? own : "RenoDX";
        const std::string mode = ini->get(sec, "NRPassesMode", "0"), method = ini->get(sec, "NRFastMethod", "0");
        return (mode == "1" || mode == "fast") && (method == "1" || method == "2" || method == "Feedback" || method == "Hybrid");
    };
    ImGui::PushTextWrapPos(0.0f);
    for (const nr::control::LayerStatus* r : live)
        for (uint32_t k = 0; k < std::min(r->panel_lines, nr::control::kPanelLines); ++k) {
            const char* line = r->panel[k];
            if (line[0] != '-') continue;
            const std::string what(line + 1, strnlen(line + 1, sizeof r->panel[k] - 1));
            if (what.rfind("Exposure:", 0) == 0)
                ImGui::TextColored(warn, tr("Layer %u: the game gives no exposure -> a fixed white point is used: the picture "
                                   "NR works on can be too dark or too bright when the game's exposure changes"), r->number);
            else if (what.rfind("Motion vectors:", 0) == 0)
                ImGui::TextColored(warn, tr("Layer %u: no motion vectors -> NR runs without its frame history (less stable over "
                                   "time)%s"), r->number,
                                   feedback(r->number) ? "; Passes (fast) Feedback / Hybrid run as Extrapolation" : "");
            else if (what.rfind("Depth:", 0) == 0)
                ImGui::TextColored(warn, tr("Layer %u: no depth from the game -> the motion lookup runs without depth"), r->number);
        }
    ImGui::PopTextWrapPos();
}

// [Preprocess]: the picture the network is shown, for every layer.
inline void draw_preprocess(nr::ini_edit::Ini& ini) {
    if (const char* e = std::getenv("NR_CONTROL_GLOBAL_OPEN")) ImGui::SetNextItemOpen(e[0] != '0');   // tests
    if (!section("Preprocess", ImGuiTreeNodeFlags_DefaultOpen)) return;   // the first of Global, open
    ini_flag(ini, "Preprocess##on", "Preprocess", "Enabled", "0", "[Preprocess] Enabled. Changes the picture the network is shown.");
    static const char* const exposures[] = {NR_N("off"), NR_N("auto"), NR_N("fixed")};
    static const char* const curves[] = {NR_N("none"), NR_N("neutral"), NR_N("reinhard"), NR_N("filmic"), NR_N("gt"), NR_N("aces"), NR_N("agx")};
    auto combo = [&](const char* label, const char* key, const char* const* items, int n, int def, const char* text) {
        const std::string v = ini.get("Preprocess", key, items[def]);
        int i = def;
        for (int k = 0; k < n; ++k) if (v == items[k]) i = k;
        std::vector<const char*> shown(items, items + n);   // the file keeps the English word
        for (const char*& s : shown) s = tr(s);
        if (ImGui::Combo(tr(label), &i, shown.data(), n)) ini.set("Preprocess", key, items[i]);
        help(text);
    };
    auto slider = [&](const char* label, const char* key, float lo, float hi, float def, const char* text) {
        static std::unordered_map<std::string, float> pending;
        const auto it = pending.find(key);
        float v = it != pending.end() ? it->second : nr::parse_float(ini.get("Preprocess", key, ""), def);
        if (it == pending.end() && ini.get("Preprocess", key, "").empty()) v = def;
        if (ImGui::SliderFloat(tr(label), &v, lo, hi, "%.2f", ImGuiSliderFlags_AlwaysClamp)) pending[key] = v;
        if (ImGui::IsItemDeactivatedAfterEdit() && pending.count(key)) {
            char t[32];
            std::snprintf(t, sizeof t, "%.2f", double(pending[key]));
            ini.set("Preprocess", key, t);
            pending.erase(key);
        }
        help(text);
    };
    // a key the file does not have: the code's default (nr::Preprocess in nr_runtime.hpp: exposure off, filmic)
    combo("Exposure", "Exposure", exposures, 3, 0, "[Preprocess] Exposure. auto: Unreal-style auto exposure; fixed: Exposure bias only.");
    slider("Exposure bias (EV)", "ExposureBias", -8.f, 8.f, 0.f, "[Preprocess] ExposureBias.");
    combo("Curve", "Curve", curves, 7, 3, "[Preprocess] Curve. The display curve of the picture NR is shown.");
    slider("Contrast", "Contrast", 0.5f, 2.f, 1.f, "[Preprocess] Contrast.");
    slider("Saturation", "Saturation", 0.05f, 2.f, 1.f, "[Preprocess] Saturation.");
    ini_flag(ini, "Inverse##prep", "Preprocess", "Inverse", "1",
             "[Preprocess] Inverse. On: NR's change comes back through the inverse of the curve, contrast and saturation "
             "(full strength in the highlights). Off: as its ratio in the picture NR was shown (gentler highlights).");
    {
        const std::string k = ini.get("Preprocess", "Hotkey", "Ctrl+F10");
        ImGui::TextDisabled(tr("On / off for this run: %s (see Hotkeys)"), k.empty() ? "no key" : k.c_str());
    }
    ini_flag(ini, "Sound##prep", "Preprocess", "Sound", "1",
             "[Preprocess] Sound. A cue when it switches: two notes going up = on, going down = off.");
}

// [Log]: read once when the game starts
inline void draw_log(nr::ini_edit::Ini& ini) {
    if (!section("Log")) return;
    ini_flag(ini, "Write the log", "Log", "Enabled", "1",
             "[Log] Enabled. dlssnr-amd.log in the game's folder. Takes effect after a restart.");
    ini_flag(ini, "Clear on start", "Log", "ClearOnStart", "1",
             "[Log] ClearOnStart. On: each start of the game begins a new log; off: appends to the last one. "
             "Takes effect after a restart.");
}

// ---- the NR chain's editor: [Layer1], [Layer2], ... (nr_pe_config.hpp read_chain) ----------------------------------
// What differs between the routes: where layers may run, what a file without [LayerN] runs as, the wilsjo2 processing's
// key table and its passes editor, the texts.
// placement: after / before / present
struct ChainRow { int number = 0; bool enabled = true; std::string processing, network, placement; };
struct ChainRoute {
    bool present_only = false;                   // the ReShade route: every layer at Present
    const char* placement_default = "present";   // a layer without Placement (Present by default)
    const char* added_processing = "RenoDX";     // a new layer's
    const char* added_placement = "present";   // the default placement
    const char* chain_help = "";
    const char* wilsjo2_about = "";              // the first line of a wilsjo2 layer's page
    const renodx_keys::Key* wilsjo2_table = nullptr;
    int wilsjo2_count = 0;
    const char* mask_label = NR_N("Character mask");
    const char* passes_help = "";
    const char* pass_same_help = "";
    // A file without [LayerN]: the one layer it runs (number 1)
    std::function<ChainRow(const nr::ini_edit::Ini&)> legacy_row;
    // Where its values are for the layer page: a file and the section holding them under the layer keys' names (the
    // file itself, or one made from it)
    std::function<std::pair<const nr::ini_edit::Ini*, std::string>(const nr::ini_edit::Ini&, const ChainRow& row)> legacy_source;
    // Its first change: the one layer written as [Layer1], its settings copied over
    std::function<void(nr::ini_edit::Ini&)> materialize;
    // The route's own Route for a RenoDX layer's keys at a placement
    std::function<Route(const std::string& placement)> renodx_route;
};

struct ChainEditor {
    nr::ini_edit::Ini& ini;
    const ChainRoute& route;
    int layer = 0;                       // the selected layer's number (0: none yet)
    nr::ini_edit::KeyValues layer_values;   // the selected layer's section, by its processing's table
    std::string layer_key;               // what layer_values was loaded for (section and processing)
    // Set by the owner before draw_chain: NR on in this run (the switch / NRToggleKey), and a layer its ToggleKey turned
    // round for this run (nr::pe::layer_flipped)
    bool nr_on = true;
    std::function<bool(int)> flipped;
    bool picked = false;   // a layer's name was clicked (the paged menu opens that layer's page)

    ChainEditor(nr::ini_edit::Ini& i, const ChainRoute& r, int selected = 0) : ini(i), route(r), layer(selected) {}

    static int layer_number(const std::string& section) { return layer_section_number(section); }
    static std::string section_of(int n) { return "Layer" + std::to_string(n); }
    // The layers that run ([NRControl] Layers off: the first alone; `all`: every [LayerN] of the file)
    std::vector<ChainRow> layers(bool all = false) const {
        std::vector<ChainRow> rows;
        for (const std::string& sec : ini.sections()) {
            const int n = layer_number(sec);
            if (n < 1 || std::any_of(rows.begin(), rows.end(), [n](const ChainRow& r) { return r.number == n; })) continue;
            if (!all && !layer_shown(ini, n)) continue;
            ChainRow r;
            r.number = n;
            r.enabled = ini.get(sec, "Enabled", "1") != "0";
            std::string p = ini.get(sec, "Processing", "RenoDX");
            for (char& c : p) c = char(std::tolower(static_cast<unsigned char>(c)));
            r.processing = p == "wilsjo2" ? "wilsjo2" : "RenoDX";
            r.network = ini.get(sec, "Network", "");
            r.placement = route.present_only ? "present" : ini.get(sec, "Placement", route.placement_default);
            rows.push_back(r);
        }
        std::sort(rows.begin(), rows.end(), [](const ChainRow& a, const ChainRow& b) { return a.number < b.number; });
        return rows;
    }
    bool legacy() const { return layers().empty(); }
    ChainRow legacy_row() const { return route.legacy_row(ini); }
    void materialize() {
        if (legacy()) route.materialize(ini);
    }
    // A layer's processing changed: its settings start again at that processing's defaults
    void set_processing(int n, const std::string& processing) {
        const std::string sec = section_of(n);
        const std::string toggle = ini.get(sec, "ToggleKey", "");
        const std::string enabled = ini.get(sec, "Enabled", "1"), network = ini.get(sec, "Network", ""),
                          placement = ini.get(sec, "Placement", route.placement_default);
        ini.remove_section(sec);
        ini.set(sec, "Enabled", enabled);
        ini.set(sec, "Processing", processing);
        if (!network.empty()) ini.set(sec, "Network", network);
        ini.set(sec, "Placement", placement);
        if (!toggle.empty()) ini.set(sec, "ToggleKey", toggle);
    }

    // [NRControl] Layers (draw_layers_switch, the Network page)
    bool many() const { return layers_on(ini); }
    // The network's entry: its own, or [Network] Model's by name ("[Global] CC_SMALL_FP8")
    std::string network_label(int net) const {
        if (net >= 0) return nr::network_codename(net);
        char t[96];
        std::snprintf(t, sizeof t, tr("[Global] %s"),
                      nr::network_codename(std::max(0, nr::network_choice(ini.get("Network", "Model", "CC_SMALL_FP8")))));
        return t;
    }
    // On in this run: the file's box, turned round by the layer's ToggleKey
    bool turned(const ChainRow& row) const { return !legacy() && flipped && flipped(row.number); }
    bool runs(const ChainRow& row) const { return nr_on && row.enabled != turned(row); }
    // Where a layer ran (its status, fresh): 0 after, 1 before, 2 Present; -1 not known. `why`: its status note.
    int ran_at(const nr::control::Shared* s, const ChainRow& row, const char** why) const {
        if (!s || route.present_only || !runs(row)) return -1;
        for (const auto& st : s->layers)
            if (st.number == uint32_t(row.number) && st.state && st.updated &&
                uint32_t(GetTickCount64()) - st.updated < 1500 && st.placement <= 2) {
                if (why) *why = st.note;
                return int(st.placement);
            }
        return -1;
    }
    // A layer's processing, network and place: a table row's cells (the NR chain), or labelled lines (the one layer's
    // page with Layers off)
    void draw_choices(const ChainRow& row, const nr::control::Shared* s, bool cells) {
        static const char* const kProcessing[] = {"RenoDX", "wilsjo2"};
        static const char* const kPlacement[] = {"after", "before", "present"};
        static const char* const kPlacementLabel[] = {NR_N("After the upscaler"), NR_N("Before the upscaler"), NR_N("Present")};
        const std::string sec = section_of(row.number);
        const float field = std::max(ImGui::GetFontSize() * 18.f, room_x() * 0.6f);   // (room(): ReShade's MSVC table)
        // a cell filled, or a line with its name after the field
        const auto place = [&](const char* id, const char* name) {
            if (cells) { ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-FLT_MIN); return std::string(id); }
            ImGui::SetNextItemWidth(field);
            return std::string(tr(name)) + id;
        };
        int pi = row.processing == "wilsjo2" ? 1 : 0;
        if (ImGui::Combo(place("##processing", NR_N("Processing")).c_str(), &pi, kProcessing, 2) && kProcessing[pi] != row.processing) {
            materialize();
            set_processing(row.number, kProcessing[pi]);
        }
        if (!cells) help("Processing. How the frame is handed to the network and its answer put back: RenoDX or wilsjo2; "
                         "each has its own settings below.");
        const int net = row.network.empty() ? -1 : nr::network_choice(row.network);
        if (ImGui::BeginCombo(place("##network", NR_N("Network")).c_str(), network_label(net).c_str())) {
            if (ImGui::Selectable(network_label(-1).c_str(), net < 0)) { materialize(); ini.set(sec, "Network", ""); }
            for (int n : nr::kNetworkOrder)
                if (ImGui::Selectable(nr::network_codename(n), n == net)) { materialize(); ini.set(sec, "Network", nr::network_codename(n)); }
            ImGui::EndCombo();
        }
        if (!cells) help("Network. [Global]: the Network page's model ([Network] Model, also switched by its hotkey); or "
                         "one network for this NR alone.");
        int pl = row.placement == "before" ? 1 : row.placement == "present" ? 2 : 0;
        // Where it ran (the layer's status, fresh): another place than set - a frame may not allow before, a
        // Present layer may wait for its Present - is
        // what the box shows, marked, with the reason; the file keeps the place set, taken again when it can.
        const char* why = "";
        const int at = ran_at(s, row, &why);
        const int ran = at < 0 ? pl : at;   // (0 after, 1 before, 2 Present: the entries' order)
        const std::string id = place("##placement", NR_N("Place"));
        if (route.present_only) {   // the one place this route has
            ImGui::BeginDisabled();
            const char* shown_places[3];
            for (int k = 0; k < 3; ++k) shown_places[k] = tr(kPlacementLabel[k]);
            ImGui::Combo(id.c_str(), &pl, shown_places, 3);
            ImGui::EndDisabled();
        } else {
            const bool moved = ran != pl;
            std::string shown = tr(kPlacementLabel[moved ? ran : pl]);
            if (moved) shown += tr(" (for now)");
            if (moved) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.3f, 1.0f));
            const bool opened = ImGui::BeginCombo(id.c_str(), shown.c_str());
            if (moved) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    char t[512];
                    std::snprintf(t, sizeof t, tr("Set: %s. Runs %s now%s%s%s"), tr(kPlacementLabel[pl]),
                                  tr(ran == 1 ? "before the upscaler" : ran == 2 ? "at Present" : "after the upscaler"),
                                  why[0] ? ": " : ".", why, why[0] ? "." : "");
                    tooltip(t);
                }
            }
            if (opened) {
                for (int k = 0; k < 3; ++k)
                    if (ImGui::Selectable(tr(kPlacementLabel[k]), k == pl) && k != pl) {
                        materialize();
                        ini.set(sec, "Placement", kPlacement[k]);
                    }
                if (moved) ImGui::TextDisabled(tr("Runs %s now: %s"), tr(ran == 0 ? "after the upscaler" : kPlacementLabel[ran]), why);
                ImGui::EndCombo();
            }
        }
        if (!cells) help(route.present_only ? "Place. On this route NR runs at Present, on the picture ReShade hands over."
                                            : "Place. Before or after the upscaler, or at Present (the finished picture).");
    }

    // `s`: where each layer actually ran (its status row); a layer running elsewhere than set shows that place
    void draw_chain(const nr::control::Shared* s = nullptr) {
        ImGui::SeparatorText(tr("NR chain"));
        const bool many = this->many();
        const bool old_file = legacy();
        const std::vector<ChainRow> rows = old_file ? std::vector<ChainRow>{legacy_row()} : layers();
        int move = 0, move_layer = 0;   // -1 up, +1 down, 2 remove
        if (!nr_on)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1.0f), tr("NR is off for this run (the switch at the top, or its "
                               "key): no layer runs. The boxes below are what the file says."));
        if (ImGui::BeginTable("chain", many ? 5 : 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            // the page's width shared out (the combos fill their columns): the network's names are the longest
            // (CC_SILVER_AARDWOLD_INT4_MIXED whole at the default size), then the places'; the layer's name as wide as
            // it is ("Layer 2 (off now)")
            ImGui::TableSetupColumn("##layer", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("##processing", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("##network", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("##placement", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            if (many)
                ImGui::TableSetupColumn("##move", ImGuiTableColumnFlags_WidthFixed,
                                        2 * ImGui::GetFrameHeight() + text_width(tr("Remove")) + 4 * ImGui::GetStyle().ItemSpacing.x);
            for (const ChainRow& row : rows) {
                const std::string sec = section_of(row.number);
                ImGui::PushID(row.number);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool on = row.enabled;
                const bool turn = turned(row);
                if (!nr_on || turn) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                if (ImGui::Checkbox("##on", &on)) { materialize(); ini.set(sec, "Enabled", on ? "1" : "0"); }
                if (!nr_on || turn) ImGui::PopStyleVar();
                if (turn && ImGui::IsItemHovered())
                    ImGui::SetTooltip(tr("Its key (ToggleKey) turned this layer %s for this run; the box is the file's."),
                                      row.enabled ? tr("off") : tr("on"));
                ImGui::SameLine();
                char label[48];
                std::snprintf(label, sizeof label, tr(turn ? (row.enabled ? "Layer %d (off now)" : "Layer %d (on now)") : "Layer %d"),
                              row.number);
                if (ImGui::Selectable(label, layer == row.number, 0, ImVec2(0, 0))) { layer = row.number; picked = true; }
                draw_choices(row, s, true);
                if (many) ImGui::TableNextColumn();
                if (many && !old_file) {
                    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { move = -1; move_layer = row.number; }
                    ImGui::SameLine();
                    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) { move = 1; move_layer = row.number; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("Remove"))) { move = 2; move_layer = row.number; }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (many && ImGui::Button(tr("Add a layer"))) {
            materialize();
            int n = 1;
            for (const ChainRow& r : layers(true)) n = std::max(n, r.number + 1);
            ini.set(section_of(n), "Enabled", "1");
            ini.set(section_of(n), "Processing", route.added_processing);
            ini.set(section_of(n), "Placement", route.added_placement);
            layer = n;
            layer_key.clear();
        }
        if (many) help(route.chain_help);
        if (move && move_layer) {
            const std::vector<ChainRow> now = layers();
            const auto at = std::find_if(now.begin(), now.end(), [&](const ChainRow& r) { return r.number == move_layer; });
            if (at != now.end()) {
                if (move == 2) {
                    ini.remove_section(section_of(move_layer));
                    layer_key.clear();
                    if (layer == move_layer) layer = 0;
                } else {
                    const auto other = move < 0 ? (at == now.begin() ? now.end() : at - 1) : at + 1;
                    if (other != now.end()) {   // the two sections swap numbers
                        ini.rename_section(section_of(at->number), "LayerSwap");
                        ini.rename_section(section_of(other->number), section_of(at->number));
                        ini.rename_section("LayerSwap", section_of(other->number));
                        layer = other->number;
                        layer_key.clear();
                    }
                }
            }
        }
    }

    // A wilsjo2 layer's passes: Passes (legacy) or Passes (fast), as a RenoDX layer has them (PassesMode). Legacy:
    // Passes, UnlockPasses, and for each later pass either pass 1's values (its keys at auto; the local tone then 0) or
    // its own Pass{N}... keys, written all at once when it gets its own. Fast: FastMethod and each method's own Fast<Method>Passes / HaloLimit /
    // Fine (FastEditStrength for Edit strength).
    // Read from `src`'s section `sec`; a file without [LayerN] gets its [Layer1] at the first change, and the change
    // goes there. `have_motion`: the layer's last run had the game's motion vectors.
    void draw_wilsjo2_passes(const nr::ini_edit::Ini& src, const std::string& sec, bool old_file, bool have_motion) {
        static const bool passes_test = [] { const char* g = std::getenv("NR_CONTROL_GROUP_OPEN"); return g && std::strcmp(g, "Passes") == 0; }();
        if (passes_test) ImGui::SetNextItemOpen(true, ImGuiCond_Once);   // tests: open
        if (!ImGui::CollapsingHeader(tr("Passes##w2"))) return;
        const auto put = [&](const std::string& key, const std::string& value) {
            if (old_file) materialize();
            ini.set(old_file ? section_of(1) : sec, key, value);
        };
        const auto num = [&](const char* key, float def) {
            const std::string v = src.get(sec, key, "");
            if (v.empty() || v == "auto") return def;
            size_t end = 0;
            const float f = nr::parse_float(v, def, &end);
            return end == 0 ? def : f;
        };
        const auto flag_of = [&](const char* key, bool def) {
            const std::string v = src.get(sec, key, "");
            return v.empty() || v == "auto" ? def : (v == "1" || v == "true" || v == "on");
        };
        const auto fmt = [](float v) { char b[32]; std::snprintf(b, sizeof b, "%g", double(v)); return std::string(b); };
        const float u = ImGui::GetFontSize() / 17.f;
        // Reset beside a number, as draw_key_in has it on the key pages (the ReShade page's Passes had none)
        const auto reset = [&](const char* id, const std::string& key, const std::string& value) {
            ImGui::SameLine();
            const std::string label = std::string("Reset##") + id;   // (no PushID(const char*): ReShade's table)
            if (ImGui::SmallButton(tr(label.c_str()))) put(key, value);
        };
        std::string mode = src.get(sec, "PassesMode", "");
        for (char& c : mode) c = char(std::tolower(static_cast<unsigned char>(c)));
        bool fast = mode == "fast" || mode == "1";
        if (ImGui::RadioButton(tr("Passes (legacy)##w2"), !fast)) { put("PassesMode", "legacy"); fast = false; }
        ImGui::SameLine();
        if (ImGui::RadioButton(tr("Passes (fast)##w2"), fast)) { put("PassesMode", "fast"); fast = true; }
        bool colour_limit = flag_of("ColourLimit", true);
        if (ImGui::Checkbox(tr("Limit colour change (RenoDX's way)##w2"), &colour_limit)) put("ColourLimit", colour_limit ? "true" : "false");
        help("ColourLimit. On (default): the colour of the network's change is bounded each pass as RenoDX bounds it, and "
             "where the highlight guard brightens a pixel it keeps the pixel's own colour, so dark face edges do not turn "
             "red or orange (worse when stacking). Off: the original wilsjo2 composition.");
        // Only the chosen mode's settings, no sub-headers; the other mode's are hidden, their values kept
        if (!fast) draw_wilsjo2_legacy(src, sec, put, num, flag_of, fmt, u, reset);
        else {
            const uint32_t method = uint32_t(std::clamp(int(num("FastMethod", 0.f)), 0, 3));
            const char* names[] = {NR_N("Extrapolation"), NR_N("Feedback"), NR_N("Hybrid"), NR_N("Edit strength")};
            ImGui::SetNextItemWidth(230 * u);
            if (ImGui::BeginCombo(tr("Method##w2f"), tr(names[method]))) {
                for (uint32_t j = 0; j < 4; ++j) {
                    ImGui::BeginDisabled((j == 1 || j == 2) && !have_motion);
                    if (ImGui::Selectable(tr(names[j]), method == j)) put("FastMethod", std::to_string(j));
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            help("FastMethod. Extrapolation pushes this frame's one-pass edit further along its own direction; no "
                 "motion vectors needed, works from the first frame. Feedback feeds the previous frame's edit, moved "
                 "by the motion vectors, back into the network input; needs motion vectors and builds up over the "
                 "first frames. Hybrid uses Feedback where history holds and Extrapolation on new surfaces, cuts and "
                 "edges. Edit strength scales the network's edit in stops, as Adjust NR's look's Edit strength does. "
                 "Each method keeps its own settings.");
            if (!have_motion && (method == 1u || method == 2u)) ImGui::TextDisabled(tr("No motion vectors: Feedback / Hybrid use Extrapolation."));
            // the chosen method's own settings (Fast<Method>Passes / HaloLimit / Fine; Edit strength its own)
            if (method == 3u) {
                float e = std::clamp(num("FastEditStrength", 2.f), 0.f, 4.f);
                ImGui::SetNextItemWidth(250 * u);
                if (ImGui::SliderFloat(tr("Edit strength##w2f"), &e, 0.f, 4.f, "%.2f")) put("FastEditStrength", fmt(e));
                reset("w2f-edit", "FastEditStrength", "2");
                help("FastEditStrength. The network's edit in stops times this (1 as made, 0 none), as Adjust NR's look's Edit strength with a wider range.");
            } else {
                const std::string pre = std::string("Fast") + names[method];
                const std::string id = std::to_string(method);
                float n = std::clamp(num((pre + "Passes").c_str(), 3.f), 1.f, 8.f);
                ImGui::SetNextItemWidth(230 * u);
                if (ImGui::SliderFloat(tr(("Passes (fast)##w2f" + id).c_str()), &n, 1.f, 8.f, "%.2f")) put(pre + "Passes", fmt(n));
                reset(("w2f-passes" + id).c_str(), pre + "Passes", "3");
                help("Strength referred to legacy pass counts (this method's own).");
                bool halo = flag_of((pre + "HaloLimit").c_str(), false);
                if (ImGui::Checkbox(tr(("Limit added halos##w2f" + id).c_str()), &halo)) put(pre + "HaloLimit", halo ? "true" : "false");
                help("Limits the halos this method adds.");
                const float fine_def = method == 0u ? 0.f : 1.f;
                float fine = std::clamp(num((pre + "Fine").c_str(), fine_def), 0.f, 2.f);
                ImGui::SetNextItemWidth(250 * u);
                if (ImGui::SliderFloat(tr(("Fine detail##w2f" + id).c_str()), &fine, 0.f, 2.f, "%.2f")) put(pre + "Fine", fmt(fine));
                reset(("w2f-fine" + id).c_str(), pre + "Fine", method == 0u ? "0" : "1");
                help(method == 0u ? "How much of the network's fine detail change Extrapolation adds: 0 none (default), 1 as made, 2 twice. Raise it for more fine detail; the game's own film grain grows with it."
                                  : "How much of the network's fine detail change this method adds: 1 as made (default), 0 none, 2 twice.");
            }
        }
    }

    template<class Put, class Num, class Flag, class Fmt, class Reset>
    void draw_wilsjo2_legacy(const nr::ini_edit::Ini& src, const std::string& sec, const Put& put, const Num& num,
                             const Flag& flag_of, const Fmt& fmt, float u, const Reset& reset) {
        bool unlock = flag_of("UnlockPasses", false);
        const int ceiling = unlock ? 30 : 3;
        int passes = std::clamp(int(num("Passes", 1.f)), 1, ceiling);
        ImGui::SetNextItemWidth(230 * u);
        if (ImGui::SliderInt(tr("Passes##w2n"), &passes, 1, ceiling)) put("Passes", std::to_string(passes));
        reset("w2n-passes", "Passes", "1");
        help(route.passes_help);
        if (ImGui::Checkbox(tr("Unlock passes##w2"), &unlock)) put("UnlockPasses", unlock ? "true" : "false");
        help("UnlockPasses. Passes beyond 3, up to 30.");
        static const char* const fields[] = {"Style", "Intensity", "LocalStructure", "LocalTone", "SkinStructure",
                                             "AutoMask"};
        const int field_count = 6;
        for (int n = 2; n <= passes; ++n) {
            char head[32];
            std::snprintf(head, sizeof head, tr("Pass %d##w2p%d"), n, n);
            if (!ImGui::TreeNodeEx(head, ImGuiTreeNodeFlags_SpanAvailWidth)) continue;
            const std::string pre = "Pass" + std::to_string(n);
            bool own = false;
            for (int f = 0; f < field_count; ++f) {
                const std::string v = src.get(sec, pre + fields[f], "");
                own |= !v.empty() && v != "auto";
            }
            bool same = !own;
            char label[48];
            std::snprintf(label, sizeof label, tr("Same as pass 1##w2s%d"), n);
            if (ImGui::Checkbox(label, &same)) {
                if (same) {
                    for (int f = 0; f < field_count; ++f) put(pre + fields[f], "auto");
                } else {   // its own values, starting from what it inherits
                    put(pre + "Style", fmt(num("Style", 0.f)));
                    put(pre + "Intensity", fmt(num("Intensity", 1.f)));
                    put(pre + "LocalStructure", fmt(num("LocalStructure", 1.f)));
                    put(pre + "LocalTone", "0");
                    put(pre + "SkinStructure", fmt(num("SkinStructure", -1.f)));
                    put(pre + "AutoMask", flag_of("AutoMask", true) ? "true" : "false");
                }
            }
            help(route.pass_same_help);
            if (!same) {
                ImGui::PushID(n);
                static const char* const styles[] = {NR_N("Standard"), NR_N("Natural"), NR_N("Cinematic")};
                int style = std::clamp(int(num((pre + "Style").c_str(), 0.f)), 0, 2);
                ImGui::SetNextItemWidth(230 * u);
                const char* const shown_styles[] = {tr(styles[0]), tr(styles[1]), tr(styles[2])};
                if (ImGui::Combo(tr("Style"), &style, shown_styles, 3)) put(pre + "Style", std::to_string(style));
                const auto slider = [&](const char* label, const char* field, float lo, float hi, float def) {
                    float v = num((pre + field).c_str(), def);
                    ImGui::SetNextItemWidth(230 * u);
                    if (ImGui::SliderFloat(tr(label), &v, lo, hi, "%.2f")) put(pre + field, fmt(v));
                    reset(field, pre + field, fmt(def));
                };
                slider("Intensity", "Intensity", 0.f, 2.f, 1.f);
                slider("Local structure", "LocalStructure", 0.f, 2.f, 1.f);
                slider("Local tone", "LocalTone", 0.f, 2.f, 0.f);
                slider("Skin structure", "SkinStructure", -1.f, 2.f, -1.f);
                bool mask = flag_of((pre + "AutoMask").c_str(), true);
                if (ImGui::Checkbox(tr(route.mask_label), &mask)) put(pre + "AutoMask", mask ? "true" : "false");
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }

    // The selected layer's page: its processing's settings, then what the game hands it (`s`'s row for it). `loaded`
    // changes when the file was read again (the page reloads its values then); `accent`: the layer's title colour.
    void draw_layer_page(const nr::control::Shared* s, int64_t loaded, const ImVec4& accent) {
        const bool old_file = legacy();
        const std::vector<ChainRow> rows = old_file ? std::vector<ChainRow>{legacy_row()} : layers();
        if (rows.empty()) return;
        auto it = std::find_if(rows.begin(), rows.end(), [&](const ChainRow& r) { return r.number == layer; });
        if (it == rows.end()) { it = rows.begin(); layer = it->number; }
        const ChainRow& row = *it;
        const bool wilsjo2 = row.processing == "wilsjo2";
        // the values: the layer's section, or the old file's until its first change writes [Layer1]
        const std::string sec = old_file ? std::string(tr("(old file)")) : section_of(row.number);
        const std::string key = sec + "|" + row.processing + "|" + std::to_string(loaded);
        if (layer_key != key) {
            layer_values = nr::ini_edit::KeyValues{};
            if (wilsjo2) { layer_values.table = route.wilsjo2_table; layer_values.count = route.wilsjo2_count; }
            if (old_file) {
                const auto [src, from] = route.legacy_source(ini, row);
                layer_values.section = from;
                layer_values.load(*src);
            } else {
                layer_values.section = sec;
                layer_values.load(ini);
            }
            layer_key = key;
        }
        ImGui::Spacing();
        // Layers off: no NR chain page; this page (NR settings) holds the one NR run's choices too
        const bool one = !many();
        if (one) {
            bool on = row.enabled;
            const bool turn = turned(row);
            if (!nr_on || turn) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
            if (ImGui::Checkbox(tr("Enabled##one"), &on)) { materialize(); ini.set(section_of(row.number), "Enabled", on ? "1" : "0"); }
            if (!nr_on || turn) ImGui::PopStyleVar();
            if (turn && ImGui::IsItemHovered())
                ImGui::SetTooltip(tr("Its key (ToggleKey) turned NR %s for this run; the box is the file's."),
                                  row.enabled ? tr("off") : tr("on"));
            help("Enabled. Off: NR does not run (kept in the file). The switch at the top of the menu turns NR off or on "
                 "for this run only.");
            draw_choices(row, s, false);
            ImGui::Spacing();
        }
        ImGui::BeginChild("layerpage", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        struct End { ~End() { ImGui::EndChild(); } } end_child;
        // its head folds the page (each layer keeps its own state; NR_CONTROL_LAYER_CLOSED=1 starts folded, for tests)
        ImGui::PushID(row.number);
        static const bool closed_at_start = [] { const char* e = std::getenv("NR_CONTROL_LAYER_CLOSED"); return e && e[0] == '1'; }();
        ImGui::SetNextItemOpen(!closed_at_start, ImGuiCond_Once);
        const bool open = ImGui::TreeNodeEx("##layerhead", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen);
        ImGui::PopID();
        ImGui::SameLine();
        if (one) {   // (the processing is the Processing box above)
            ImGui::TextColored(accent, "%s", tr("Processing settings"));
            if (!open) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("(folded)")); }
        } else {
            ImGui::TextColored(accent, tr("Layer %d"), row.number);
            ImGui::SameLine();
            ImGui::Text("%s", row.processing.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr(open ? "- this layer's own settings" : "- this layer's own settings (folded)"));
        }
        if (!open) return;
        ImGui::Separator();
        // each layer and processing keeps its own groups' open state (the groups share names: Network, Composition,
        // Resolution) - switching RenoDX -> wilsjo2 -> RenoDX finds the groups as they were
        // (an int: ReShade's ImGui table takes no PushID(const char*))
        ImGui::PushID(0x4C000 + row.number * 2 + (wilsjo2 ? 1 : 0));
        struct Pop { ~Pop() { ImGui::PopID(); } } pop_id;
        bool changed = false;
        const bool layer_motion = s && std::any_of(std::begin(s->layers), std::end(s->layers), [&](const auto& r) {
            return r.number == uint32_t(row.number) && r.motion != 0;
        });
        if (wilsjo2) {
            ImGui::TextDisabled("%s", tr(route.wilsjo2_about));
            ImGui::Spacing();
            changed = draw_table(route.wilsjo2_table, route.wilsjo2_count, layer_values, [&] {   // Passes after Network
                if (old_file) {
                    const auto [src, from] = route.legacy_source(ini, row);
                    draw_wilsjo2_passes(*src, from, true, layer_motion);
                } else {
                    draw_wilsjo2_passes(ini, sec, false, layer_motion);
                }
            });
        } else {
            changed = draw_renodx(layer_values, route.renodx_route(row.placement), true, {},
                                  [](const char* name) { return !nr::ini_edit::chain_global(name); }, layer_motion);
        }
        if (changed) {
            if (old_file) {
                materialize();
                layer_values.section = section_of(1);
                layer_key.clear();
            }
            layer_values.store(ini);
        }
        // What the game hands this layer, and who covers what it does not (its runs' engine-data lines)
        const nr::control::LayerStatus* status = nullptr;
        if (s)
            for (const auto& r : s->layers)
                if (r.number == uint32_t(row.number)) status = &r;
        static const bool engine_open = [] { const char* e = std::getenv("NR_CONTROL_ENGINE_OPEN"); return e && e[0] == '1'; }();
        if (status && status->panel_lines &&
            ImGui::CollapsingHeader(tr("Engine data##layer"), engine_open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            engine_legend();
            for (uint32_t i = 0; i < std::min(status->panel_lines, nr::control::kPanelLines); ++i) {
                char line[sizeof status->panel[0] + 1];
                std::snprintf(line, sizeof line, "%s", status->panel[i]);
                engine_line(line);
            }
        }
    }
};

}  // namespace nr::control_ui
