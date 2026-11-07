// NR Control's lines for the native D3D12 direct-ELF backend (Diagnostics; nr_pe_menu.cpp draws them through
// menu::set_extra_diagnostics, set by InitializeASI). The .asi only.
#include "nr_d3d12_asd.hpp"
#include "nr_control_ui.hpp"

namespace nr::pe::asd {

void draw_panel() {
    const Status s = status();
    if (s.folder.empty()) return;   // no dlssnr-amd-asd.ini: nothing to show
    ImGui::Separator();
    ImGui::TextUnformatted("D3D12 passthrough (direct ELF)");
    nr::control_ui::text_dim_wrapped("%s at launch (%s)", s.enabled ? "on" : "off", s.folder.c_str());
    if (s.enabled) {
        nr::control_ui::text_dim_wrapped("Canary: %s", s.canary.c_str());
        nr::control_ui::text_dim_wrapped("Backend: %s", s.backend.c_str());
        if (!s.network.empty()) nr::control_ui::text_dim_wrapped("Network: %s", s.network.c_str());
        nr::control_ui::text_dim_wrapped("Frames: %llu on the game's D3D12 device, %llu through the Vulkan bridge",
                                         (unsigned long long)s.runs, (unsigned long long)s.fallbacks);
    }
    bool on = s.enabled;
    static int written = -1;   // what the checkbox wrote (takes effect at the next launch)
    if (written >= 0) on = written == 1;
    if (ImGui::Checkbox("D3D12 passthrough (next launch)", &on)) { set_enabled(on); written = on ? 1 : 0; }
}

}  // namespace nr::pe::asd
