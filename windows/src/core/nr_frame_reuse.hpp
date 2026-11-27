#pragma once
// Frame reuse runs the configured network on alternate rendered frames.
// Each intermediate picture uses both adjacent NR answers. Split recordings
// distribute work over the pair, and the ready pictures appear two frames late.
#include "nr_renodx.hpp"
#include <functional>
#include <memory>
namespace nr::frame_reuse {
class Stage {
public:
    struct GuideExtent { uint32_t motion_width{}, motion_height{}, depth_width{}, depth_height{}; };
    struct Config {
        uint32_t width{}, height{};
        VkFormat frame_format{};
        GuideExtent guides{};   // the game's motion and depth textures' sizes (the guides' grids)
        // The only shipped schedule. The constructor also enforces these
        // values for old replay callers that pass an explicit cadence.
        uint32_t compute = 1, period = 2;
        float fast_speed = 0.f;
        uint32_t hint_lag = 2;
        // compose.comp's rules (bits): stay test, look-alike donors, same-surface donors, fill of uncovered pixels
        uint32_t protection = 336625664u;
        // The shown edit's temporal filter (display.comp) on every frame
        bool display_filter = true;
        // A rebuilt frame uses both adjacent computed answers.
        bool bidi = true;
        float merge_k = 0.f;
        float stay_fixed = 2.f;   // compose's fixed-overlay test, codes a pixel may change in place (the hosts set 6)
        bool fwd_check = false;   // the future side's source checked (on screen, maps back within ~2 px)
        // The two-sided merge weighs each side by its outline context: a side whose source lies nearer a nearer surface's
        // outline than the pixel does now (it would bring that outline's rim - the network's context - to a place one
        // frame of relative motion away: an orbit camera's figure over a sliding wall) counts less. Two-sided only.
        bool context = true;
        bool pair_kernel = false; // inner member; the chain owns scheduling
        bool batch_only = false;  // scheduler; the member stages own reconstruction
        bool scrgb = false;      // native linear FP16 at 80 nits per unit
        // Measurement: called between the steps of a recording (a timestamp each): 0 start, 1 guides and the motion
        // measure / decision, 2 the frame copies, 3 the network (its callback), 4 compose / fill / carry (7: after compose, before the fill), 5 the answer
        // stored, 6 the display filter and the output. Empty: nothing.
        std::function<void(VkCommandBuffer, uint32_t)> mark;
        // The next Present executes the queued tail. After startup, every
        // picture has the same two-frame delay.
        bool delay = true;
        float split = 0.5f;          // the share to start from (then balanced: balance())
        bool balance_split = true;
    };
    // Inside the Complete callback of a delayed stage: the recording to go on in after `share` of the network's work
    // (the steps of every pass together); tail null: no split (not delayed).
    struct Split { VkCommandBuffer tail = VK_NULL_HANDLE; float share = 0; };
    struct Result {
        bool ok=true;
        uint8_t shown_late = 0, next_late = 0;
        bool completed = false;    // The configured network ran on this anchor.
        bool decided = false;      // Compatibility field for older replay readers; always false.
        uint32_t network_calls = 0, correction_age = 0;
    };
    // The GPU's decisions so far (read a few frames late): frames, of them with NR, of them planned as rebuilt.
    struct Stats { uint64_t frames = 0, computed = 0, planned = 0; float speed = 0; };   // speed: the latest frame's motion
    // Records the complete NR of the frame it is handed (its colour / output / motion / depth are the stage's).
    using Complete = std::function<uint32_t(VkCommandBuffer, const renodx::StageFrame&)>;
    // Compatibility argument retained for old CPU/GPU replay hosts. The fixed cadence does not call it.
    using HistoryPairs = std::function<std::vector<std::pair<ColourFrame, ColourFrame>>()>;
    // Native colour formats processed by the transport. Depth is optional.
    static bool supports(VkFormat colour, VkFormat output);
    static bool supports(VkFormat f) { return supports(f, f); }
    Stage(const HostDevice&, const Config&, const std::string& shaders);
    ~Stage();
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
    Result record(VkCommandBuffer, const renodx::StageFrame&, const Complete&, const HistoryPairs&);
    static void pass_input(VkCommandBuffer, const renodx::StageFrame&);
    using PairNetwork=std::function<uint32_t(VkCommandBuffer&,const renodx::StageFrame&)>;
    // A member of a jointly scheduled layer chain. Both colours are returned in
    // native units. The command cursor follows the single cut across all layers.
    uint32_t render_pair(VkCommandBuffer&,renodx::StageFrame*,renodx::StageFrame&,const PairNetwork&);
    using Batch=std::function<uint32_t(VkCommandBuffer&,renodx::StageFrame*,renodx::StageFrame&,VkCommandBuffer,float)>;
    Result record_batch(VkCommandBuffer,const renodx::StageFrame&,const Batch&);
    void reset();
    // Terminal discard/retirement. Owner keeps this object AND its callback's NR state until recordings_done.
    void cancel();
    bool recordings_done() const;
    uint64_t image_bytes() const;
    const Config& config() const;
    // Inside the Complete callback: how many frames old the history it continues is (1, or 2 after a rebuilt frame).
    uint32_t gap() const;
    // Inside the Complete callback: where to split the network (delayed stage).
    Split split() const;
    // A delayed stage: the share of the network in a frame's first part now (balanced on the GPU's times; Config::split
    // to start), and how often it moved.
    float balance(uint32_t* moves = nullptr) const;
    // A delayed stage: a frame's second part waits for the next recording, which runs it first. flush(cmd) runs it in
    // `cmd` with its answer going nowhere: before NR runs without this stage (paused, switched off, NR off for a frame,
    // retired) - else the network's state the CPU side assumes is not on the GPU. Nothing waiting: nothing recorded.
    bool pending() const;
    bool can_flush_on(VkDevice, uint32_t queue_family) const;
    void flush(VkCommandBuffer);
    // All preceding layers run at this same Present. Finish the previous network
    // tail before they can reuse its scratch. The owner still presents the result.
    void advance(VkCommandBuffer);
    // The latest motion measure taken (fraction of the screen a frame), -1 before the first one.
    float speed() const;
    Stats stats() const;
    // Inside the Complete callback, after the network's parameters of a pass are written (Runtime::HistoryWeightHook):
    // on a frame after a GPU-decided one, records the GPU's choice of the history weight into them; else nothing.
    void history_weight(VkCommandBuffer, VkBuffer params, uint32_t offset, float weight);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
