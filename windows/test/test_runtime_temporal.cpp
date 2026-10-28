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
    if (std::filesystem::is_directory(data_dir / "aco/rdna3")) {
        const char* f = std::getenv("DLSSNR_FORCE_ARCH");
        if (f && std::strcmp(f, "rdna4") == 0 && std::filesystem::is_directory(data_dir / "aco/rdna4")) {
            bundle = (data_dir / "aco/rdna4").string();
        } else {
            bundle = (data_dir / "aco/rdna3").string();
        }
    }
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
    std::vector<std::string> template_dirs = {shells, shell_fb};
    if (std::filesystem::is_directory(data_dir / "shaders/rdna4"))
        template_dirs.push_back((data_dir / "shaders/rdna4").string());
    if (std::filesystem::is_directory(data_dir / "shaders/rdna3"))
        template_dirs.push_back((data_dir / "shaders/rdna3").string());
    template_dirs.push_back(shaders);
    nr::binary::configure(bundle, template_dirs, "", std::move(aliases));

    nrvk::Context ctx;
    try {
        ctx.create();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Context create failed: %s\n", e.what());
        return 1;
    }
    std::printf("Active GPU: %s (%s)\n", ctx.gpu_name.c_str(), ctx.target_name());

    nr::HostDevice host{};
    host.instance = ctx.instance;
    host.physical = ctx.physical;
    host.device = ctx.device;
    host.queue = ctx.queue;
    host.queue_family = ctx.family;
    host.buffer_device_address = true;
    host.rdna3 = ctx.rdna3 ? 1 : 0;

    nr::RuntimeConfig rc{};
    rc.root = pkg_root.string();
    rc.width = 640;
    rc.height = 360;
    rc.colour_format = VK_FORMAT_R32G32B32A32_SFLOAT;
    rc.network = 1; // small model
    rc.max_passes = 1;
    rc.rdna3 = ctx.rdna3 ? 1 : 0;

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
        double sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
        const size_t num_pixels = rc.width * rc.height;
        for (size_t p = 0; p < num_pixels; ++p) {
            sum_r += out_data[p * 4 + 0];
            sum_g += out_data[p * 4 + 1];
            sum_b += out_data[p * 4 + 2];
            sum_a += out_data[p * 4 + 3];
        }
        std::printf("[test] Frame %d results: non-zero: %.2f%%, NaNs: %zu, AvgAbs: %.4f\n",
                    frame_idx, (100.0 * non_zero) / out_data.size(), has_nan, avg);
        std::printf("       Channels Avg: R=%.4f, G=%.4f, B=%.4f, A=%.4f\n",
                    sum_r / num_pixels, sum_g / num_pixels, sum_b / num_pixels, sum_a / num_pixels);
        std::printf("       Sample center pixel: R=%.4f, G=%.4f, B=%.4f, A=%.4f\n",
                    out_data[(rc.height / 2 * rc.width + rc.width / 2) * 4 + 0],
                    out_data[(rc.height / 2 * rc.width + rc.width / 2) * 4 + 1],
                    out_data[(rc.height / 2 * rc.width + rc.width / 2) * 4 + 2],
                    out_data[(rc.height / 2 * rc.width + rc.width / 2) * 4 + 3]);

        if (has_nan > 0) {
            std::fprintf(stderr, "[FAIL] Frame %d contains %zu NaNs!\n", frame_idx, has_nan);
            return 1;
        }
        if (non_zero == 0) {
            std::fprintf(stderr, "[FAIL] Frame %d is entirely black (0 non-zero)!\n", frame_idx);
            return 1;
        }
    }

    std::printf("[test] Setting up Engine Frame (direct_source in-place sampling with motion vectors)...\n");
    auto motion_img = ctx.image(rc.width, rc.height, VK_FORMAT_R16G16_SFLOAT, true, true);
    ctx.transition(motion_img, VK_IMAGE_LAYOUT_GENERAL);
    std::vector<uint16_t> zero_mv(rc.width * rc.height * 2, 0);
    ctx.upload(motion_img, zero_mv.data(), zero_mv.size() * sizeof(uint16_t));

    nr::EngineFrame ef{};
    ef.colour = cf;
    ef.colour.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ef.motion.image = motion_img.handle;
    ef.motion.format = VK_FORMAT_R16G16_SFLOAT;
    ef.motion.width = rc.width;
    ef.motion.height = rc.height;
    ef.motion.before = VK_IMAGE_LAYOUT_GENERAL;
    ef.motion.after = VK_IMAGE_LAYOUT_GENERAL;
    ef.motion.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ef.feature = 1;
    ef.reset = false;

    std::printf("[test] Recording and evaluating Frame 4 (Engine direct_source path)...\n");
    ctx.one_shot([&](VkCommandBuffer cmd) {
        runtime->record_engine(cmd, ef, controls);
    });

    std::vector<float> engine_out(rc.width * rc.height * 4);
    ctx.download(input_img, engine_out.data(), engine_out.size() * sizeof(float));

    size_t e_non_zero = 0, e_has_nan = 0;
    double e_sum_r = 0, e_sum_g = 0, e_sum_b = 0, e_sum_a = 0;
    const size_t num_pixels = rc.width * rc.height;
    for (size_t p = 0; p < num_pixels; ++p) {
        float r = engine_out[p * 4 + 0], g = engine_out[p * 4 + 1], b = engine_out[p * 4 + 2], a = engine_out[p * 4 + 3];
        if (std::isnan(r) || std::isnan(g) || std::isnan(b) || std::isnan(a) ||
            std::isinf(r) || std::isinf(g) || std::isinf(b) || std::isinf(a)) ++e_has_nan;
        if (r != 0.0f || g != 0.0f || b != 0.0f || a != 0.0f) ++e_non_zero;
        e_sum_r += r; e_sum_g += g; e_sum_b += b; e_sum_a += a;
    }
    std::printf("[test] Frame 4 (Engine direct_source) results: non-zero: %.2f%%, NaNs: %zu\n",
                (100.0 * e_non_zero) / num_pixels, e_has_nan);
    std::printf("       Channels Avg: R=%.4f, G=%.4f, B=%.4f, A=%.4f\n",
                e_sum_r / num_pixels, e_sum_g / num_pixels, e_sum_b / num_pixels, e_sum_a / num_pixels);
    std::printf("       Sample center pixel: R=%.4f, G=%.4f, B=%.4f, A=%.4f\n",
                engine_out[(rc.height / 2 * rc.width + rc.width / 2) * 4 + 0],
                engine_out[(rc.height / 2 * rc.width + rc.width / 2) * 4 + 1],
                engine_out[(rc.height / 2 * rc.width + rc.width / 2) * 4 + 2],
                engine_out[(rc.height / 2 * rc.width + rc.width / 2) * 4 + 3]);

    if (e_has_nan > 0) {
        std::fprintf(stderr, "[FAIL] Frame 4 contains %zu NaNs!\n", e_has_nan);
        return 1;
    }
    if (e_non_zero == 0) {
        std::fprintf(stderr, "[FAIL] Frame 4 is entirely black!\n");
        return 1;
    }

    std::printf("\n>>> TEMPORAL & ENGINE RUNTIME VERIFICATION SUCCESSFUL (all frames evaluated without crash)! <<<\n");
    return 0;
}
