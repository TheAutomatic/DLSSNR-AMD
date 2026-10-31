// NR Control's lines for the native D3D12 ASD backend (Diagnostics; nr_pe_menu.cpp draws them through
// menu::set_extra_diagnostics, set by InitializeASI). The .asi only.
#include "nr_d3d12_asd.hpp"
#include "nr_control_ui.hpp"

namespace nr::pe::asd {

void draw_panel() {
    const Status s = status();
    if (s.folder.empty()) return;   // no dlssnr-amd-asd.ini: nothing to show (0.0.8.1 as it was)
    ImGui::Separator();
    ImGui::TextUnformatted("D3D12 ASD backend (M5 test build)");
    nr::control_ui::text_dim_wrapped("%s at launch; Register %d; SODB/PSDB %s (%s)", s.enabled ? "on" : "off",
                                     s.register_on ? 1 : 0, s.files_found ? "found" : "missing", s.folder.c_str());
    if (s.enabled) {
        nr::control_ui::text_dim_wrapped("Registration: %s", s.registration.c_str());
        nr::control_ui::text_dim_wrapped("Canary: %s", s.canary.c_str());
        nr::control_ui::text_dim_wrapped("Backend: %s", s.backend.c_str());
        if (!s.network.empty()) nr::control_ui::text_dim_wrapped("Network: %s", s.network.c_str());
        nr::control_ui::text_dim_wrapped("Frames: %llu by the D3D12 path, %llu by the 0.0.8.1 path",
                                         (unsigned long long)s.runs, (unsigned long long)s.fallbacks);
    }
    bool on = s.enabled;
    static int written = -1;   // what the checkbox wrote (takes effect at the next launch)
    if (written >= 0) on = written == 1;
    if (ImGui::Checkbox("D3D12 ASD backend (next launch)", &on)) { set_enabled(on); written = on ? 1 : 0; }
    if (s.register_on) {
        if (ImGui::Button("Unregister ASD for this game")) unregister_async();
        nr::control_ui::help("Removes this game's ASD registration made by dlssnr-amd.asi (never another installer's) and "
                             "writes [D3D12ASD] Register = 0 to dlssnr-amd-asd.ini, so no launch registers it again.");
    } else if (ImGui::Button("Allow ASD registration again")) {
        allow_register();
    }
}

}  // namespace nr::pe::asd
