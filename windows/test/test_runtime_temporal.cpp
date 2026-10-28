#include "core/nr_runtime.hpp"
#include "core/nrvk.hpp"
#include "core/nr_pipeline_binary.hpp"
#include "core/nr_log.hpp"
#include <cstdio>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

extern "C" void nr_vk_load(void);

int main(int argc, char** argv) {
    nr_vk_load();
    std::printf("============================================================\n");
    std::printf("  RDNA3 Temporal Path End-to-End Validation\n");
    std::printf("============================================================\n");

    std::string data_root = "artifacts/windows/RDNA3&3.5-DLSSNR-GameTest-v1.0.2/dlssnr-amd";
    if (argc > 1) {
        data_root = argv[1];
    }

    std::filesystem::path root_path = std::filesystem::canonical(data_root);
    std::filesystem::path data_dir = (root_path.filename() == "dlssnr-amd") ? root_path : (root_path / "dlssnr-amd");
    std::filesystem::path pkg_root = (root_path.filename() == "dlssnr-amd") ? root_path.parent_path() : root_path;

    std::string bundle = (data_dir / "aco").string();
    std::string shells = bundle + "/shells";
    std::string shell_fb = bundle + "/shell";
    std::string shaders = (data_dir / "shaders").string();

    // Load aliases
    std::map<std::string, std::string> aliases;
    std::ifstream alias_file(bundle + "/shell-aliases.txt");
    if (alias_file) {
        std::string line;
        while (std::getline(alias_file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream iss(line);
            std::string from, to;
            if (iss >> from >> to) aliases[from] = to;
        }
    }
    std::printf("[test] Configured ACO bundle from: %s (%zu aliases)\n", bundle.c_str(), aliases.size());

    // Configure binary mode BEFORE creating Vulkan Context so pipeline_binary extension is enabled on device
    nr::binary::configure(bundle, {shells, shell_fb, shaders}, "", std::move(aliases));

    nrvk::Context ctx;
    try {
        ctx.create();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Context create failed: %s\n", e.what());
        return 1;
    }
    std::printf("Active GPU: %s\n", ctx.gpu_name.c_str());

    nr::HostDevice host{};
    host.instance = ctx.instance;
    host.physical = ctx.physical;
    host.device = ctx.device;
    host.queue = ctx.queue;
    host.queue_family = ctx.family;
    host.buffer_device_address = true;

    nr::RuntimeConfig rc{};
    rc.root = pkg_root.string();
    rc.width = 640;
    rc.height = 360;
    rc.colour_format = VK_FORMAT_R32G32B32A32_SFLOAT;
    rc.network = 1; // small model

    nr::TemporalConfig tc{};
    tc.enable = true;
    tc.history_strength = 1.0f;

    std::printf("[test] Constructing Runtime with TemporalConfig{enable = true}...\n");
    std::unique_ptr<nr::Runtime> runtime;
    try {
        runtime = std::make_unique<nr::Runtime>(host, rc, nr::ControlMaskConfig{}, tc);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[FAIL] Runtime construction failed: %s\n", e.what());
        return 1;
    }
    std::printf("[test] Runtime constructed successfully!\n");

    // Create test image (640x360 RGBA32F)
    auto input_img = ctx.image(rc.width, rc.height, VK_FORMAT_R32G32B32A32_SFLOAT, true, true);
    ctx.transition(input_img, VK_IMAGE_LAYOUT_GENERAL);

    // Fill with gradient pattern
    std::vector<float> input_data(rc.width * rc.height * 4);
    for (uint32_t y = 0; y < rc.height; ++y) {
        for (uint32_t x = 0; x < rc.width; ++x) {
            size_t idx = (y * rc.width + x) * 4;
            input_data[idx + 0] = float(x) / float(rc.width);
            input_data[idx + 1] = float(y) / float(rc.height);
            input_data[idx + 2] = 0.5f;
            input_data[idx + 3] = 1.0f;
        }
    }
    ctx.upload(input_img, input_data.data(), input_data.size() * sizeof(float));

    nr::ColourFrame cf{};
    cf.image = input_img.handle;
    cf.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    cf.width = rc.width;
    cf.height = rc.height;
    cf.before = VK_IMAGE_LAYOUT_GENERAL;
    cf.after = VK_IMAGE_LAYOUT_GENERAL;

    nr::Controls controls{};
    controls.enabled = true;
    controls.apply_model = true;
    controls.intensity = 1.0f;
    controls.passes = 1;

    for (int frame_idx = 1; frame_idx <= 3; ++frame_idx) {
        std::printf("[test] Recording and evaluating Frame %d (temporal enabled)...\n", frame_idx);
        nr::TemporalFrame tf{};
        tf.reset = (frame_idx == 1); // reset on frame 1, history on frames 2+
        tf.feature = 1;

        ctx.one_shot([&](VkCommandBuffer cmd) {
            runtime->record_temporal(cmd, cf, controls, tf);
        });

        // Download result and inspect
        std::vector<float> out_data(rc.width * rc.height * 4);
        ctx.download(input_img, out_data.data(), out_data.size() * sizeof(float));

        size_t non_zero = 0;
        size_t has_nan = 0;
        double sum = 0.0;
        for (float v : out_data) {
            if (std::isnan(v) || std::isinf(v)) ++has_nan;
            if (v != 0.0f) ++non_zero;
            sum += std::abs(v);
        }
        double avg = sum / out_data.size();
        std::printf("[test] Frame %d results: non-zero: %.2f%%, NaNs: %zu, AvgAbs: %.4f\n",
                    frame_idx, (100.0 * non_zero) / out_data.size(), has_nan, avg);

        if (has_nan > 0) {
            std::fprintf(stderr, "[FAIL] Frame %d contains %zu NaNs!\n", frame_idx, has_nan);
            return 1;
        }
        if (non_zero == 0) {
            std::fprintf(stderr, "[FAIL] Frame %d is entirely black (0 non-zero)!\n", frame_idx);
            return 1;
        }
    }

    std::printf("\n>>> TEMPORAL RUNTIME VERIFICATION SUCCESSFUL (3 frames evaluated without crash)! <<<\n");
    return 0;
}
