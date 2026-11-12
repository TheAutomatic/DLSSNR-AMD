// NR Control's lines for the D3D12 passthrough (nr_pe_menu.cpp draws them through menu::set_extra_network and
// menu::set_extra_diagnostics, set by InitializeASI): the switch and a short state on the Network page, the details in
// Diagnostics. The .asi only.
#include "nr_d3d12_passthrough.hpp"
#include "nr_control_ui.hpp"
#include "nr_lang.hpp"

namespace nr::pe::d3d12_passthrough {

using nr::lang::tr;

namespace {
int g_written = -1;   // what the checkbox wrote this run (takes effect at the next launch)
}

void draw_network() {
    const Status s = status();
    if (!nr::control_ui::section("D3D12 passthrough (experimental)")) return;
    bool on = g_written >= 0 ? g_written == 1 : s.enabled;
    if (ImGui::Checkbox(tr("D3D12 passthrough (next launch)"), &on)) { set_enabled(on); g_written = on ? 1 : 0; }
    nr::control_ui::help("On the OptiScaler route in D3D12 games on an RX 9000 or RX 7000: NR's network runs on the game's "
                         "own D3D12 device instead of through the Vulkan bridge. A game it cannot serve uses the bridge.");
    nr::control_ui::text_dim_wrapped("%s at launch (dlssnr-amd.ini [D3D12Passthrough] Enabled)", s.enabled ? tr("On") : tr("Off"));
    if (s.enabled) {
        nr::control_ui::text_dim_wrapped("Check: %s", s.canary.c_str());
        nr::control_ui::text_dim_wrapped("Frames: %llu on the game's D3D12 device, %llu through the Vulkan bridge, %llu waiting (no NR)",
                                         (unsigned long long)s.runs, (unsigned long long)s.fallbacks, (unsigned long long)s.waits);
    }
}

void draw_panel() {
    const Status s = status();
    ImGui::Separator();
    ImGui::TextUnformatted(tr("D3D12 passthrough (experimental)"));
    nr::control_ui::text_dim_wrapped("%s at launch (dlssnr-amd.ini [D3D12Passthrough] Enabled)", s.enabled ? tr("On") : tr("Off"));
    if (!s.enabled) return;
    if (!s.adapter.empty()) nr::control_ui::text_dim_wrapped("GPU: %s", s.adapter.c_str());
    nr::control_ui::text_dim_wrapped("Check: %s", s.canary.c_str());
    nr::control_ui::text_dim_wrapped("Backend: %s", s.backend.c_str());
    if (!s.network.empty()) nr::control_ui::text_dim_wrapped("Network: %s", s.network.c_str());
    nr::control_ui::text_dim_wrapped("Frames: %llu on the game's D3D12 device, %llu through the Vulkan bridge, %llu waiting (no NR)",
                                     (unsigned long long)s.runs, (unsigned long long)s.fallbacks, (unsigned long long)s.waits);
}

}  // namespace nr::pe::d3d12_passthrough
