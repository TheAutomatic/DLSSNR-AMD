#include "core/nr_pipeline_binary.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::string base_dir = ".";
    if (argc > 1) {
        base_dir = argv[1];
    }

    std::string bundle = base_dir + "/windows/data/aco/rdna3";
    std::string shells = bundle + "/shells";
    std::string shell_fallback = bundle + "/shell";
    std::string shaders = base_dir + "/windows/data/aco/rdna3/shaders";

    // Load aliases from shell-aliases.txt
    std::map<std::string, std::string> aliases;
    std::ifstream alias_file(bundle + "/shell-aliases.txt");
    if (!alias_file) {
        alias_file.open(shells + "/shell-aliases.txt");
    }
    if (alias_file) {
        std::string line;
        while (std::getline(alias_file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream iss(line);
            std::string from, to;
            if (iss >> from >> to) {
                aliases[from] = to;
            }
        }
        std::printf("[test] Loaded %zu aliases\n", aliases.size());
    } else {
        std::printf("[test] Warning: Could not open shell-aliases.txt from %s\n", bundle.c_str());
    }

    // Configure binary mode as in nr_pe_config.cpp
    nr::binary::configure(bundle, {shells, shell_fallback, shaders}, "", std::move(aliases));

    struct TestCase {
        std::string query;
        std::string expected_suffix;
        size_t max_allowed_size; // should be small shell (~3KB), NOT the native ~400KB shader!
    };

    std::vector<TestCase> test_cases = {
        {"g_attn.spv", "shells/g_attn.spv", 30000},
        {"temporal_post_fp32.spv", "shells/temporal/temporal_post_fp32.spv", 5000},
        {"temporal_post_fp32_grade.spv", "shells/temporal/temporal_post_fp32_grade.spv", 5000},
        {"temporal_pre_fp32nh.spv", "shells/temporal/temporal_pre_fp32nh.spv", 5000},
        {"temporal_post_fp32h16.spv", "shells/temporal/temporal_post_fp32h16.spv", 5000},
        // Also test full paths (Unix & Windows style)
        {"shaders/temporal/temporal_post_fp32.spv", "shells/temporal/temporal_post_fp32.spv", 5000},
        {"shaders\\temporal\\temporal_post_fp32.spv", "shells/temporal/temporal_post_fp32.spv", 5000},
        {"shaders\\temporal\\temporal_pre_fp32nh.spv", "shells/temporal/temporal_pre_fp32nh.spv", 5000},
    };

    bool all_passed = true;
    for (const auto& tc : test_cases) {
        std::string resolved = nr::binary::resolve_template(tc.query);
        std::printf("[test] Query: %-42s -> Resolved: %s\n", tc.query.c_str(), resolved.c_str());

        if (resolved.empty()) {
            std::fprintf(stderr, "  [FAIL] Failed to resolve: %s\n", tc.query.c_str());
            all_passed = false;
            continue;
        }

        // Normalize slashes for comparison
        std::string norm = resolved;
        for (char& c : norm) if (c == '\\') c = '/';

        if (norm.rfind(tc.expected_suffix) == std::string::npos) {
            std::fprintf(stderr, "  [FAIL] Expected suffix '%s' in '%s'\n", tc.expected_suffix.c_str(), norm.c_str());
            all_passed = false;
        }

        // Check file size
        std::ifstream f(resolved, std::ios::binary | std::ios::ate);
        if (!f) {
            std::fprintf(stderr, "  [FAIL] File not found or cannot open: %s\n", resolved.c_str());
            all_passed = false;
        } else {
            size_t size = f.tellg();
            std::printf("         Size: %zu bytes (max allowed: %zu bytes)\n", size, tc.max_allowed_size);
            if (size > tc.max_allowed_size) {
                std::fprintf(stderr, "  [FAIL] Size %zu exceeds limit %zu (wrong shader selected!)\n", size, tc.max_allowed_size);
                all_passed = false;
            }
        }
    }

    if (all_passed) {
        std::printf("\n>>> ALL TEMPLATE RESOLVER TESTS PASSED! <<<\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n>>> SOME TESTS FAILED! <<<\n");
        return 1;
    }
}
