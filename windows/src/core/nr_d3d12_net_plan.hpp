#pragma once
// The lowered network as the native D3D12 ASD backend runs it (windows/src/pe/nr_d3d12_asd_net.cpp): the same lowering
// the Vulkan runtime builds (nr::Runtime, nr_runtime.cpp: the model choice, the build arguments, NrSession::build), kept
// as data - every dispatch with its kernel, grid and push block, the weight blob with its tail, the noise table's jobs and
// the activation arena's extent - and handed over without a Vulkan device. No Vulkan type in here: the D3D12 side
// includes it alone.
#include <cstdint>
#include <string>
#include <vector>

namespace nr {

struct D3d12NetPlan {
    struct Dispatch {
        std::string kern;          // the graph's kernel name (g_<kern>.spv in spv_dir)
        uint32_t gx = 0, gy = 0, gz = 0;
        std::vector<uint8_t> push;
        int act_at = -1;           // where the push block takes the activation arena's address (put_act_address); -1: none
    };
    struct Noise { uint32_t off, width, height, seed; float noise; };   // NoiseJob (nr_graph.cpp)
    std::vector<Dispatch> disp;
    // The pre block's seed-0 noise table (g_noisefield). noise_in_buffer (the shader folder's manifest noise_buffer):
    // in a buffer of its own at the pre kernels' binding 5, `off` counted in u32 from that buffer's start, made only
    // once frames keep seed 0 (until then the pre push says none, noise_off 0: the shader computes it, the same bytes;
    // Runtime::Impl::make_noise). Else in the weight blob at `off`, filled once at the start.
    std::vector<Noise> noise;
    bool noise_in_buffer = false;
    uint64_t noise_bytes = 0;      // noise_in_buffer: the table buffer's size (NrSession::noise_bytes)
    std::vector<uint8_t> weights;  // the whole weight blob, the tail included (at weights_end)
    uint64_t weights_end = 0, act_total = 0;
    std::string spv_dir;           // the shaders the graph was lowered against (their FNV names the PSDB record)
    std::string model;             // in words, for the log
    std::string note;              // why the model is not the one asked for (empty: it is)
    uint32_t width = 0, height = 0, work_w = 0, work_h = 0;
    bool style_grade = true;       // [Network] Style Grade (false too when the temporal post has no grade twin)
    // The temporal path (Runtime::Runtime's temporal_config.enable): the graph's pre and post block are replaced by the
    // temporal pre / post kernels at their steps, with the same push blocks.
    struct Temporal {
        int pre = -1, post = -1;                   // the indices in disp of the graph's pre / post block
        std::string pre_spv;                       // full paths
        std::string post_spv, post_grade_spv;      // the RGBA32F history's post (post_grade_spv empty: no grade twin)
        // The RGBA16F history's post and its grade twin ([Network] History Precision 16; empty: the folder has none,
        // or not the grade twin where the RGBA32F post has one - then the history stays RGBA32F, as the runtime's h16_ok)
        std::string post16_spv, post16_grade_spv;
        bool history_f16 = true;                   // [Network] History Precision (16, the default) as the ini says it
        uint32_t pre_push_bytes = 0;               // kPrePushBytes: the pre block's push, PushFSwin + PushPreImage
        uint32_t seed_at = 0, noise_field_at = 0;  // kPreSeedAt / kPreNoiseFieldAt in it
        float blend_scale = 1.0f;                  // the model's post_blend_scale
        bool motion_without_depth = true;          // [Network] Motion Without Depth (default 1)
    } temporal;
};

// The model controls of one frame, as the evaluate's Controls6 carries them (nr::Controls' fields of the same names).
struct D3d12NetControls {
    int style = 0;
    float intensity = 1.0f, local_tone = 1.0f, local_structure = 1.0f, skin_structure = -1.0f;
    bool automatic_mask = false;
    bool round_u8 = false;         // the output is 8-bit: the alpha pass rounds as the post block would
    uint32_t reversible = 0;       // reversible_of(Controls): part of the input domain a history was made under
    float history_strength = -1.0f;   // Controls::history_strength (< 0: the session's, 1)
    bool noise_every_frame = false;
    bool seed_fixed = false;       // Controls::seed_fixed, including temporal frames
};

// nr_runtime.cpp. The network for a `width` x `height` frame of the installation at `root` (the folder with
// dlssnr-amd\ in it), the model as the Vulkan runtime picks it (DLSSNR_MODEL over dlssnr-amd.ini [Network] Model) unless
// `model` (nr::kOriginal ...) is given; `experimental` false: CC_SMALL_FP8_EXPERIMENTAL is planned as CC_SMALL_FP8.
// False, and `why`: no network.
bool plan_d3d12_network(const std::string& root, uint32_t width, uint32_t height, bool experimental, D3d12NetPlan* out,
                        std::string* why, int model = -1);
// The frame's controls into one dispatch's push block (Runtime::Impl::patch_push, the same code).
void d3d12_net_controls(D3d12NetPlan::Dispatch& d, const D3d12NetControls& c, bool grade);

}  // namespace nr
