#pragma once
// The menus' [RenoDX] section, drawn from nr_renodx_keys.hpp's table: NR Control (the OptiScaler route's own menu,
// linux/src/pe/nr_pe_menu.cpp) and the ReShade add-on's page (linux/src/pe/nr_reshade_addon.cpp) show the same
// controls in the same groups. ImGui only; the caller owns the context and the values.
//
// `A` gives each key's value by its table index: bool& b(int), uint32_t& u(int), float& f(int), std::string& s(int).
// Sliders and text are committed when the control is let go (one write a drag, not one a frame); a checkbox or a
// combo at once. Returns whether anything was committed this frame.
#include "imgui.h"
#include "nr_renodx_keys.hpp"
#include "nr_lang.hpp"
#include "pe/nr_engine_panel.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nr::control_ui {

enum class Route { kOptiScaler, kReShade, kOptiScalerPresent };

inline int key_index(const char* name) {
    for (int i = 0; i < renodx_keys::kCount; ++i)
        if (std::strcmp(renodx_keys::kKeys[i].name, name) == 0) return i;
    return -1;
}

// A tooltip, wrapped at ~40 characters' width (a long one on one line ran off the screen)
inline void tooltip(const char* text) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.f);
    ImGui::TextUnformatted(tr(text));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
// Text that wraps at the window's edge (a reason, a note, an event: of any length - an unwrapped long line widened the
// menu's content until the rest seemed gone); the dim one as TextDisabled
inline void text_dim_wrapped(const char* fmt, ...) IM_FMTARGS(1);
inline void text_dim_wrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrappedV(tr(fmt), args);
    ImGui::PopStyleColor();
    va_end(args);
}
inline void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled(tr("(?)"));
    if (ImGui::IsItemHovered()) tooltip(text);
}

// NR Control's paged layout (nr_pe_menu.cpp): a page's parts are titled groups, always open. Elsewhere (the ReShade
// Add-ons page) they stay collapsible headers.
inline bool& paged() { static bool on = false; return on; }
inline bool section(const char* title, ImGuiTreeNodeFlags flags = 0) {
    if (!paged()) return ImGui::CollapsingHeader(tr(title), flags);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    ImGui::SeparatorText(tr(title));
    ImGui::PopStyleColor();
    return true;
}

// One key's control, key `i` of `table` (nr_renodx_keys.hpp's shape). `pending` holds a dragged slider's value until it
// is let go.
template <class A>
bool draw_key_in(const renodx_keys::Key* table, int i, A& a, float hi_limit = 0.f) {
    const renodx_keys::Key& k = table[i];
    const float hi = hi_limit > 0.f ? hi_limit : k.hi;
    static std::map<int, float> pending;
    static std::map<int, std::string> text;
    const int id = table == renodx_keys::kKeys ? i : 1000 + i;   // another table's keys after the RenoDX ones
    bool changed = false;
    ImGui::PushID(id);
    switch (k.kind) {
        case renodx_keys::kBool: {
            bool v = a.b(i);
            if (ImGui::Checkbox(tr(k.label), &v)) { a.b(i) = v; changed = true; }
            break;
        }
        case renodx_keys::kUint: {
            if (k.items) {
                // An item named "~..." is a value a file may hold but the menu does not offer (it does what another
                // item does); it is shown, without the "~", while it is the value.
                const int v = int(a.u(i));
                std::vector<std::pair<int, const char*>> items;
                for (const char* p = k.items; *p; p += std::strlen(p) + 1) items.push_back({int(items.size()), p});
                const char* shown = v >= 0 && v < int(items.size()) ? items[size_t(v)].second : "";
                if (*shown == '~') ++shown;
                if (ImGui::BeginCombo(tr(k.label), tr(shown))) {
                    for (const auto& [n, name] : items) {
                        if (*name == '~') continue;
                        if (ImGui::Selectable(tr(name), n == v) && n != v) { a.u(i) = uint32_t(n); changed = true; }
                    }
                    ImGui::EndCombo();
                }
            } else {
                auto it = pending.find(id);
                int v = it != pending.end() ? int(it->second) : int(a.u(i));
                if (ImGui::SliderInt(tr(k.label), &v, int(k.lo), int(hi), "%d", ImGuiSliderFlags_AlwaysClamp)) pending[id] = float(v);
                if (ImGui::IsItemDeactivatedAfterEdit() && pending.count(id)) {
                    a.u(i) = uint32_t(pending[id]); pending.erase(id); changed = true;
                }
            }
            break;
        }
        case renodx_keys::kFloat: {
            auto it = pending.find(id);
            float v = it != pending.end() ? it->second : a.f(i);
            char fmt[8];
            std::snprintf(fmt, sizeof fmt, "%%.%uf", unsigned(k.decimals));
            if (ImGui::SliderFloat(tr(k.label), &v, k.lo, k.hi, fmt, ImGuiSliderFlags_AlwaysClamp)) pending[id] = v;
            if (ImGui::IsItemDeactivatedAfterEdit() && pending.count(id)) {
                a.f(i) = pending[id]; pending.erase(id); changed = true;
            }
            break;
        }
        case renodx_keys::kText: {
            auto it = text.find(id);
            std::string& t = it != text.end() ? it->second : (text[id] = a.s(i));
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s", t.c_str());
            if (ImGui::InputText(tr(k.label), buf, sizeof buf)) t = buf;
            if (ImGui::IsItemDeactivatedAfterEdit()) { a.s(i) = t; text.erase(id); changed = true; }
            else if (!ImGui::IsItemActive()) text.erase(id);
            break;
        }
    }
    if (k.kind == renodx_keys::kFloat || (k.kind == renodx_keys::kUint && !k.items)) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Reset"))) {
            if (k.kind == renodx_keys::kFloat) a.f(i) = k.def;
            else a.u(i) = uint32_t(k.def);
            pending.erase(id);
            changed = true;
        }
    }
    help(k.help);
    ImGui::PopID();
    return changed;
}
template <class A>
bool draw_key(A& a, int i, float hi_limit = 0.f) { return draw_key_in(renodx_keys::kKeys, i, a, hi_limit); }

// A whole key table (the .asi's wilsjo2 layer page), group by group in its order. True when anything was committed.
template <class A>
bool draw_table(const renodx_keys::Key* table, int count, A& a, const std::function<void()>& after_first = {}) {
    bool changed = false;
    std::vector<const char*> groups;
    for (int i = 0; i < count; ++i) {
        bool seen = false;
        for (const char* g : groups) seen |= std::strcmp(g, table[i].group) == 0;
        if (!seen) groups.push_back(table[i].group);
    }
    // the wilsjo2 Resolution group: what applies at this Model resolution (greyed with the reason, the value kept)
    int working = -1, transfer = -1;
    for (int i = 0; i < count; ++i) {
        if (std::strcmp(table[i].name, "WorkingScale") == 0) working = i;
        if (std::strcmp(table[i].name, "Transfer") == 0) transfer = i;
    }
    // `after_first`: drawn after the first group (the wilsjo2 page's Passes after Network, as RenoDX has them)
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        if (gi == 1 && after_first) after_first();
        const char* g = groups[gi];
        if (!ImGui::CollapsingHeader(tr(g))) continue;   // every group folded at first
        for (int i = 0; i < count; ++i) {
            if (std::strcmp(table[i].group, g) != 0) continue;
            const char* n = table[i].name;
            const char* why = nullptr;
            if (working >= 0) {
                const float scale = a.f(working);
                if (scale >= 1.0f && (std::strcmp(n, "Downscale") == 0 || std::strcmp(n, "Transfer") == 0 ||
                                      std::strcmp(n, "ClassicScaler") == 0))
                    why = NR_N("Below 100 % Model resolution only.");
                if (scale <= 1.0f && std::strcmp(n, "ScalingDownscaler") == 0) why = NR_N("Above 100 % Model resolution only.");
            }
            if (!why && transfer >= 0 && std::strcmp(n, "ClassicScaler") == 0 && a.u(transfer) != 4u)
                why = NR_N("Enlargement Classic only.");
            ImGui::BeginDisabled(why != nullptr);
            changed |= draw_key_in(table, i, a);
            ImGui::EndDisabled();
            if (why) text_dim_wrapped("%s", tr(why));
        }
    }
    if (groups.size() < 2 && after_first) after_first();
    return changed;
}

// Placement, output rectangles, game exposure and frame-generation timing belong to the OptiScaler route.
inline bool on_route(const char* name, Route route) {
    if (route == Route::kOptiScaler) return true;
    // Present is still the OptiScaler route: its frame-generation controls remain available.
    if (route == Route::kOptiScalerPresent &&
        (std::strcmp(name, "NRPresentFrames") == 0 || std::strcmp(name, "NRPresentFgSource") == 0)) return true;
    return std::strcmp(name, "NRHookPoint") != 0 && std::strcmp(name, "NRFeedMode") != 0 &&
           std::strcmp(name, "NROutputRect") != 0 && std::strcmp(name, "NRPresentFrames") != 0 &&
           std::strcmp(name, "NRPresentFgSource") != 0;
}

// The whole section, group by group in the table's order. `running`: the option is on in this run (its keys apply);
// off, only Enabled is offered (its other settings apply once it is on after a restart). `screenshot`: the menu's own
// capture button and status, drawn at the end of the Screenshot group.
// `keep` (optional): a key it returns false for is left out (the .asi's layer page: the layer's own Enabled and Placement
// replace Enabled and NRHookPoint, and NR on / off and the screenshot keys are NR Control's, for all layers).
template <class A>
bool draw_renodx(A& a, Route route, bool running, const std::function<void()>& screenshot = {},
                 const std::function<bool(const char*)>& keep = {}, bool have_motion = false) {
    using namespace renodx_keys;
    bool changed = false;
    std::vector<const char*> groups;
    for (int i = 0; i < kCount; ++i) {
        bool seen = false;
        for (const char* g : groups) seen |= std::strcmp(g, kKeys[i].group) == 0;
        if (!seen) groups.push_back(kKeys[i].group);
    }
    auto value_of = [&](const char* name) -> float {
        const int i = key_index(name);
        if (i < 0) return 0.f;
        switch (kKeys[i].kind) {
            case kBool: return a.b(i) ? 1.f : 0.f;
            case kUint: return float(a.u(i));
            case kFloat: return a.f(i);
            default: return 0.f;
        }
    };
    // One group's keys, greyed where the value does nothing now; `pass`: a "Pass N" group's N (0: none)
    auto keys_of = [&](const char* g, int pass) {
        for (int i = 0; i < kCount; ++i) {
            const Key& k = kKeys[i];
            if (std::strcmp(k.group, g) != 0 || !on_route(k.name, route) || (keep && !keep(k.name))) continue;
            if (!running && std::strcmp(k.name, "Enabled") != 0) continue;
            bool off = false;   // greyed out: the value is kept but does nothing now
            if (std::strncmp(k.name, "NRLook", 6) == 0 && std::strcmp(k.name, "NRLookMode") != 0 &&
                std::strcmp(k.name, "NRLookUpsample") != 0)
                off = value_of("NRLookMode") == 0.f;
            if ((std::strcmp(k.name, "NRLookStabilizeMs") == 0 || std::strcmp(k.name, "NRLookStabilizeDetail") == 0) &&
                value_of("NRLookStabilize") == 0.f)
                off = true;
            if (std::strcmp(k.name, "NRSkinStructure") == 0) off = value_of("NRAutoMask") == 0.f;
            if (pass) {
                char follow[32];
                std::snprintf(follow, sizeof follow, "NRPass%dFollowPass1", pass);
                const char* steering[] = {"Style", "LocalTone", "Structure", "Skin", "AutoMask"};
                for (const char* s : steering) {
                    char n[32];
                    std::snprintf(n, sizeof n, "NRPass%d%s", pass, s);
                    if (std::strcmp(k.name, n) == 0) off = value_of(follow) != 0.f;
                }
            }
            // NRPasses: RenoDX's 4 unless NRUnlockPasses
            const float hi = std::strcmp(k.name, "NRPasses") == 0 && value_of("NRUnlockPasses") == 0.f ? 4.f : 0.f;
            ImGui::BeginDisabled(off);
            changed |= draw_key(a, i, hi);
            ImGui::EndDisabled();
        }
    };
    for (const char* g : groups) {
        const bool first = std::strcmp(g, "Neural Rendering") == 0;
        if (!running && !first) break;
        int pass = 0;   // "Pass 2", "Pass 3", "Pass 4 and later"
        if (std::strncmp(g, "Pass ", 5) == 0) pass = g[5] - '0';
        if (pass) continue;   // drawn inside Passes (legacy)
        bool any = false;   // a group with nothing left to show (the .asi's layer page) gets no header
        for (int i = 0; i < kCount && !any; ++i)
            any = std::strcmp(kKeys[i].group, g) == 0 && on_route(kKeys[i].name, route) && (!keep || keep(kKeys[i].name)) &&
                  (running || std::strcmp(kKeys[i].name, "Enabled") == 0);
        if (!any && !(screenshot && std::strcmp(g, "Screenshot") == 0)) continue;
        if (std::strcmp(g, "Passes") == 0) {
            // One Passes header with the mode inside it, folded at first.
            const char* test_open = std::getenv("NR_CONTROL_GROUP_OPEN");
            if (test_open) ImGui::SetNextItemOpen(std::strcmp(test_open, g) == 0);
            if (!ImGui::CollapsingHeader(tr("Passes"))) continue;
            const int mode_key = key_index("NRPassesMode");
            bool fast = a.u(mode_key) == 1u;
            if (ImGui::RadioButton(tr("Passes (legacy)"), !fast)) { a.u(mode_key) = 0u; fast = false; changed = true; }
            ImGui::SameLine();
            if (ImGui::RadioButton(tr("Passes (fast)"), fast)) { a.u(mode_key) = 1u; fast = true; changed = true; }
            // Only the chosen mode's settings, no sub-headers; the other mode's are hidden, their values kept
            if (!fast) {
                changed |= draw_key(a, key_index("NRPasses"), value_of("NRUnlockPasses") ? 30.f : 4.f);
                changed |= draw_key(a, key_index("NRUnlockPasses"));
                changed |= draw_key(a, key_index("NRChainedHistory"));
                // each later pass's own settings, under the passes they belong to: only the passes that run
                for (const char* pg : groups) {
                    if (std::strncmp(pg, "Pass ", 5) != 0) continue;
                    const int n = pg[5] - '0';
                    if (value_of("NRPasses") < float(n)) continue;
                    if (ImGui::TreeNodeEx(tr(pg), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                        keys_of(pg, n);
                        ImGui::TreePop();
                    }
                }
            } else {
                const int method = key_index("NRFastMethod");
                const uint32_t m = std::min(a.u(method), 3u);
                const char* names[] = {NR_N("Extrapolation"), NR_N("Feedback"), NR_N("Hybrid"), NR_N("Edit strength")};
                if (ImGui::BeginCombo(tr("Method"), tr(names[m]))) {
                    for (uint32_t j = 0; j < 4; ++j) {
                        ImGui::BeginDisabled((j == 1 || j == 2) && !have_motion);
                        if (ImGui::Selectable(tr(names[j]), m == j)) { a.u(method) = j; changed = true; }
                        ImGui::EndDisabled();
                    }
                    ImGui::EndCombo();
                }
                help(kKeys[method].help);
                if (!have_motion && (m == 1u || m == 2u)) ImGui::TextDisabled(tr("No motion vectors: Feedback / Hybrid use Extrapolation."));
                // the chosen method's own settings (each method keeps its own)
                if (m == 3u) changed |= draw_key(a, key_index("NRFastEditStrength"));
                else {
                    const std::string pre = std::string("NRFast") + names[m];
                    changed |= draw_key(a, key_index((pre + "Passes").c_str()));
                    changed |= draw_key(a, key_index((pre + "HaloLimit").c_str()));
                    changed |= draw_key(a, key_index((pre + "Fine").c_str()));
                }
            }
            if (const char* g = std::getenv("NR_CONTROL_GROUP_OPEN"); g && std::strcmp(g, "Passes") == 0)
                ImGui::SetScrollHereY(0.f);
            continue;
        }
        // Tests: photograph one settings group with NR_CONTROL_DUMP.
        const char* test_group = std::getenv("NR_CONTROL_GROUP_OPEN");
        if (test_group) ImGui::SetNextItemOpen(std::strcmp(test_group, g) == 0);
        if (!ImGui::CollapsingHeader(tr(g))) continue;   // every group folded at first
        if (test_group) ImGui::SetScrollHereY(0.f);
        keys_of(g, pass);
        if (screenshot && std::strcmp(g, "Screenshot") == 0) screenshot();
    }
    if (!running)
        ImGui::TextDisabled(tr("RenoDX style is off in this run: check it above and restart the game."));
    return changed;
}

// The engine-data panel: what the colours mean, then one line (nr::pe::panel::split).
inline void engine_legend() {
    float c[4];
    const char* const parts[][2] = {{"+", NR_N("from the game")}, {"~", NR_N("filled in (not from the game)")}, {"-", NR_N("missing (nothing fills it in)")}};
    for (int i = 0; i < 3; ++i) {
        nr::pe::panel::state_colour(parts[i][0][0], c);
        if (i) { ImGui::SameLine(); ImGui::TextDisabled(tr("/")); ImGui::SameLine(); }
        ImGui::TextColored(ImVec4(c[0], c[1], c[2], c[3]), "%s", tr(parts[i][1]));
    }
}
inline void engine_line(const std::string& line) {
    // translated as a whole after its state mark (the language file has the texts without it), then split
    const bool mark = !line.empty() && !std::isalnum(static_cast<unsigned char>(line[0]));
    const std::string l = mark ? line.substr(0, 1) + tr_text(line.substr(1)) : tr_text(line);
    const nr::pe::panel::Drawn d = nr::pe::panel::split(l);
    const ImVec4 c(d.rgba[0], d.rgba[1], d.rgba[2], d.rgba[3]);
    if (d.whole) ImGui::PushStyleColor(ImGuiCol_Text, c);
    if (!d.label.empty()) {
        ImGui::TextColored(c, "%s", d.label.c_str());
        ImGui::SameLine(0.f, 0.f);
    }
    ImGui::TextWrapped("%s", d.rest.c_str());
    if (d.whole) ImGui::PopStyleColor();
}

}  // namespace nr::control_ui
