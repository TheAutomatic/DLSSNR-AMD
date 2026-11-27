// Split two-sided reconstruction of alternate rendered frames.
#include "nr_frame_reuse.hpp"
#include "nr_frame_pair.hpp"
#include "nr_log.hpp"
#include "nr_gpu_completion.hpp"
#include "nr_runtime_dispatch.hpp"
#include "nrvk.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <optional>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace nr::frame_reuse {
namespace {
using Image=nrvk::Context::Image;
void check(VkResult r) { if(r!=VK_SUCCESS) throw std::runtime_error("frame reuse Vulkan error "+std::to_string(r)); }
VkImageAspectFlags aspects(VkFormat f) {
    switch(f) {
    case VK_FORMAT_D16_UNORM:case VK_FORMAT_X8_D24_UNORM_PACK32:case VK_FORMAT_D32_SFLOAT:return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:case VK_FORMAT_D24_UNORM_S8_UINT:case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    default:return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
void barrier(VkCommandBuffer cmd,VkImage im,VkImageLayout from,VkImageLayout to,VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout=from;b.newLayout=to;b.image=im;b.subresourceRange={aspect,0,1,0,1};
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
void memory(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr);
}
ColourFrame frame(const Image& im) {
    ColourFrame f;f.image=im.handle;f.format=im.format;f.width=im.w;f.height=im.h;
    f.before=f.after=VK_IMAGE_LAYOUT_GENERAL;
    f.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return f;
}
// (`sx`, `sy`: where in `src` the copied region begins - a guide's subrect base)
void copy(VkCommandBuffer cmd,const ColourFrame& src,const Image& dst,uint32_t sx=0,uint32_t sy=0) {
    barrier(cmd,src.image,src.before,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cmd,dst.handle,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={dst.w,dst.h,1};
    c.srcOffset={int32_t(sx),int32_t(sy),0};
    vkCmdCopyImage(cmd,src.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
    barrier(cmd,dst.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    barrier(cmd,src.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,src.after);
}
struct GuidePush {
    uint32_t mgrid[2],dgrid[2],mbase[2],msize[2],dbase[2],dsize[2];float scale[2];uint32_t flags;
};
struct ComposePush {
    uint32_t size[2],grid[2],mode,current_motion,bank,age,valid,previous;float blend,stay_margin;uint32_t protection,inverted;
    uint32_t side;float merge_k;float stay_fixed;uint32_t rebuild_flags;
};
struct ContextPush {uint32_t size[2],mode,which,inverted;};
struct DisplayPush {uint32_t size[2],mbase[2],mgrid[2];float mscale[2];uint32_t dbase[2],dgrid[2],flags,pad[3];};
static_assert(sizeof(GuidePush)==60 && sizeof(ComposePush)==72 && sizeof(DisplayPush)==64 && sizeof(ContextPush)==20);
void dispatch(VkCommandBuffer cmd,const nrvk::Kernel& k,const void* push,uint32_t bytes,uint32_t w,uint32_t h,VkDescriptorSet set={},
              bool groups=false) {
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,k.pipeline);
    const auto s=set?set:k.set;
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,k.layout,0,1,&s,0,nullptr);
    if(bytes)vkCmdPushConstants(cmd,k.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,bytes,push);
    if(groups)vkCmdDispatch(cmd,w,h,1);else vkCmdDispatch(cmd,(w+15)/16,(h+15)/16,1);memory(cmd);
}

}

namespace {
// A raw image copy preserves stored display codes, including from a non-mutable sRGB image.
VkFormat code_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}
const char* kernel_suffix(VkFormat f) {
    switch (f) {
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "10.spv";
    case VK_FORMAT_R16G16B16A16_UNORM: return "16u.spv";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "16f.spv";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "11f.spv";
    default: return ".spv";
    }
}
}
bool Stage::supports(VkFormat colour, VkFormat output) {
    const VkFormat f = code_format(colour);
    return f == code_format(output) &&
           (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
            f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || f == VK_FORMAT_R16G16B16A16_UNORM ||
            f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32);
}

struct Stage::Impl {
    nrvk::Context ctx;
    Config cfg;
    nr::frame_pair::Sources pair_sources;
    uint64_t serial=0;
    renodx::StageFrame pending_input{};
    Image native_input, native_answer;
    nrvk::Kernel encode, decode;
    Image current,anchor[2],edit[2],rendered,shown,motion[3],composed,depth,anchor_depth[2],exposure,anchor_exposure[2];
    // display.comp: the filtered edit's history (two banks, RGBA16F), the filtered frame
    Image display_history[2],display_out,output_copy;
    nrvk::Kernel guides,compose;
    // the two-sided rebuild: the rebuilt frame's depth kept for the next recording, the side (edit, trust: side 1's, then
    // both merged by side 2's mode 2) and side 1's match term, the rebuilt answer in the frame's format, the picture output
    // one recording later; compose_b = compose with the kept depth bound as this frame's
    Image pend_depth,side,side_match,rebuilt8,bidi_held;nrvk::Kernel compose_b;
    // ... and the outline context classes (context.comp) of the kept depth and of the anchors' depths (Config::context);
    // context_tile: the nearest depth per 16 x 16 texels of the depth being classified
    Image context[3],context_tile;nrvk::Kernel context_kernel;
    // ... and the kept frame's depth at every pixel (compose's mode 7) and the nearer surface near each of its pixels (mode
    // 6), read by both sides' mode 2 and the fill
    Image depth_full,pick;
    bool bidi_pending=false,bidi_held_valid=false;uint32_t pend_slot=0,pend_age=1;
    VkSampler linear{};   // compose.comp's filtered reads (anchors, edits, the anchors' depths): linear, clamp to the edge
    // display kernel (its images bound per frame: filter); speed kernels: [motion bank]
    nrvk::Kernel display;
    bool display_valid=false;uint32_t display_bank=0,display_prev=0;   // display_prev: the frame before's input (filter)
    nr::gpu::Uses uses;
    struct Borrow {
        VkDescriptorPool pool{};std::vector<VkImageView> views;nr::gpu::Uses uses;
    };
    std::vector<Borrow> borrowed;
    // Config::delay: the second parts (secondary recordings of the stage's own, run inside the next frame's recording),
    // the one waiting for it, the shown picture it leaves (frame format) for that recording's output
    VkCommandPool tail_pool{};
    VkCommandPool compute_tail_pool{};
    uint32_t compute_family=UINT32_MAX;   // the device's compute-only family (vkd3d-proton's COMPUTE queues), else the host's
    bool family_said=false;
    // The family the recording in progress belongs to: 1 on a compute-only command list (the session's flag), else 0
    uint8_t recording_pool() {
        if(!nrvk::compute_only_recording())return 0;
        if(compute_family==UINT32_MAX) {
            uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical,&n,nullptr);
            std::vector<VkQueueFamilyProperties> f(n);vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical,&n,f.data());
            compute_family=ctx.family;
            for(uint32_t i=0;i<n;++i) if((f[i].queueFlags&VK_QUEUE_COMPUTE_BIT) && !(f[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)) {compute_family=i;break;}
        }
        return compute_family==ctx.family?0:1;
    }
    // A second part's recording has a ticket of its own: everything touched while the frame was recorded (this stage's
    // borrowed sets, the network's runtime and stage) is touched with it, and it is attached to both recordings - the
    // frame's and the next one that runs it - so nothing it uses is reused or freed before it has run.
    // picture: it leaves one in `held`; pool: the queue family it is recorded for (0 the host's, 1 the compute family - a
    // secondary runs only in a primary of its own family: on a COMPUTE list, as DLSS-G's evaluate through Streamline has it,
    // a graphics-family secondary is invalid - RADV's graphics-ring packets on the compute ring hung the GPU)
    // from_bidi: its picture is `bidi_held` itself (nothing after it in the part writes that image), not `held`
    struct Tail {VkCommandBuffer cb{};nr::gpu::Ticket recording;bool ran=true;bool picture=true;uint8_t pool=0;bool from_bidi=false;};
    std::vector<Tail> tails;
    int waiting=-1;
    Image held;bool held_valid=false, advanced=false, held_bidi=false;   // held_bidi: the picture to show is in bidi_held
    // ... and the game's motion vectors of the frame, copied whole (its format, made at the first frame): the network
    // and the stage after it read them in the second part, after the game has rendered the next frame into its own
    // texture
    Image game_motion;
    // ... and the split balanced on the GPU's own times (timing only: the pictures do not depend on where the network
    // is split): every recording timed (start / after the frame before's second part / end) and the frame's GPU decision
    // word kept; in a clean alternation (a frame rebuilt, a computed one, a rebuilt one) the slot with the computed
    // frame's first part is compared with the next slot (its second part), and the share moves toward even.
    static constexpr uint32_t kTimed=16;
    VkQueryPool balance_pool{};double tick_ns=1;
    struct Timed {uint64_t serial=~0ull;char kind=0;};   // 'C' computed, 'D' GPU-decided, 'R' rebuilt (no decision)
    std::array<Timed,kTimed> timed{};
    float share=0.5f;std::vector<float> balance_errors;uint32_t balance_moves=0;
    uint64_t cpu_frames=0,cpu_computed=0;   // fast_speed 0 (no GPU decision): the frames and those with NR, counted here
    VkCommandBuffer splitting{};   // inside Complete: where the network's second part goes
    uint32_t phase=0;
    bool initialized=false,valid=false,previous=false,reset_requested=false,abandoned=false;
    uint32_t latest=0,age=0,guide_count=0,protection=0,inverted=0,current_gap=1;
    Result result{};

    Impl(const HostDevice& host,const Config& c,const std::string& shaders):cfg(c) {
        if(c.compute<1 || c.compute>=c.period || c.period-c.compute>1)
            throw std::invalid_argument("frame reuse cadence: one rebuilt frame after 1..period-1 computed ones");
        if(!c.delay || !c.bidi || c.compute!=1 || c.period!=2 || c.fast_speed!=0)
            throw std::invalid_argument("frame reuse requires split two-sided reconstruction");
        if(!supports(c.frame_format,c.frame_format)) throw std::invalid_argument("frame reuse unsupported frame (format "+std::to_string(int(c.frame_format))+")");
        if(!c.guides.motion_width || !c.guides.motion_height || !c.guides.depth_width || !c.guides.depth_height)
            throw std::invalid_argument("frame reuse needs the game's motion vectors and depth");
        const auto grid=c.guides;
        runtime_dispatch::Scope physical_scope(host.instance,host.physical_dispatch);
        std::string why;
        if(!ctx.adopt(host.instance,host.physical,host.device,host.queue,host.queue_family,&why))
            throw std::runtime_error("frame reuse could not adopt the host device ("+why+")");
        auto make=[&](Image& im,VkFormat fmt,uint32_t w=0,uint32_t h=0) {
            im=ctx.image(w?w:c.width,h?h:c.height,fmt,true,false);im.layout=VK_IMAGE_LAYOUT_GENERAL;
        };
        try {
            const VkFormat fmt = code_format(c.frame_format);
            // display.comp's output format (no BGRA / A2R10G10B10 storage): a stage of its own (not a chain's pair kernel)
            // keeps its shown pictures (held, bidi_held) in it - the filter writes them directly
            const VkFormat shown_fmt = fmt == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM :
                                       fmt == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : fmt;
            const bool direct_shown = c.display_filter && !c.pair_kernel && !c.batch_only;
            // A missing sampler denotes a storage-image descriptor in nrvk.
            auto storage=[](Image& x){Image r=x;r.sampler={};return r;};
            const auto make_guides=[&] {
                Image gm=storage(motion[0]),gd=storage(depth),ge=storage(exposure);
                guides.create(ctx,shaders+"/guides.spv",{},sizeof(GuidePush),{&motion[0],&depth,&exposure,&gm,&gd,&ge});
            };
            const auto make_scheduler=[&] {
                if(!(c.split>=0.f && c.split<=1.f))throw std::invalid_argument("frame reuse: split share 0..1");
                make(held,direct_shown?shown_fmt:fmt);share=c.split;
                VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=kTimed*4;
                check(vkCreateQueryPool(ctx.device,&qi,nullptr,&balance_pool));
                VkPhysicalDeviceProperties pp{};vkGetPhysicalDeviceProperties(ctx.physical,&pp);tick_ns=pp.limits.timestampPeriod;
                VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pi.queueFamilyIndex=host.queue_family;
                check(vkCreateCommandPool(ctx.device,&pi,nullptr,&tail_pool));
            };
            if(c.batch_only) {
                make(current,fmt);make(anchor[0],fmt);make(bidi_held,fmt);
                for(auto& m:motion)make(m,VK_FORMAT_R32G32_SFLOAT,grid.motion_width,grid.motion_height);
                make(depth,VK_FORMAT_R32_SFLOAT,grid.depth_width,grid.depth_height);
                make(pend_depth,VK_FORMAT_R32_SFLOAT,grid.depth_width,grid.depth_height);
                make(exposure,VK_FORMAT_R32_SFLOAT,1,1);make(anchor_exposure[1],VK_FORMAT_R32_SFLOAT,1,1);
                make_guides();make_scheduler();return;
            }
            make(current,fmt);make(rendered,fmt);make(shown,fmt==VK_FORMAT_R16G16B16A16_UNORM?fmt:VK_FORMAT_R16G16B16A16_SFLOAT);
            for(auto& x:anchor) make(x,fmt);
            for(auto& x:edit) make(x,VK_FORMAT_R16G16B16A16_SFLOAT);
            const uint32_t mw=grid.motion_width,mh=grid.motion_height,dw=grid.depth_width,dh=grid.depth_height;
            for(auto& x:motion) make(x,VK_FORMAT_R32G32_SFLOAT,mw,mh);
            make(composed,VK_FORMAT_R32G32_SFLOAT,mw,mh);make(depth,VK_FORMAT_R32_SFLOAT,dw,dh);
            for(auto& x:anchor_depth) make(x,VK_FORMAT_R32_SFLOAT,dw,dh);
            make(exposure,VK_FORMAT_R32_SFLOAT,1,1);
            for(auto& x:anchor_exposure) make(x,VK_FORMAT_R32_SFLOAT,1,1);
            if(c.scrgb) {
                if(fmt!=VK_FORMAT_R16G16B16A16_SFLOAT || !c.bidi || !c.delay)
                    throw std::invalid_argument("scRGB frame reuse requires a two-sided FP16 stage");
                make(native_input,fmt);make(native_answer,fmt);
                Image dst=storage(current);
                encode.create(ctx,shaders+"/scrgb_encode.spv",{},0,{&native_input,&dst});
                decode.create(ctx,shaders+"/scrgb_decode.spv",{},0,{&current,&dst});
            }
            make_guides();
            Image out=storage(shown),e0=storage(edit[0]),e1=storage(edit[1]),mv=storage(composed);
            {
                VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;
                si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
                si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                check(vkCreateSampler(ctx.device,&si,nullptr,&linear));
            }
            auto filtered=[&](Image& x){Image r=x;r.sampler=linear;return r;};
            Image a0=filtered(anchor[0]),a1=filtered(anchor[1]),ed0=filtered(edit[0]),ed1=filtered(edit[1]),
                  ad0=filtered(anchor_depth[0]),ad1=filtered(anchor_depth[1]);
            {   // the sides (full size only for the two-sided rebuild)
                const uint32_t sw=c.bidi?c.width:1,sh=c.bidi?c.height:1;
                make(side,VK_FORMAT_R16G16B16A16_SFLOAT,sw,sh);make(side_match,VK_FORMAT_R16_SFLOAT,sw,sh);
                make(depth_full,VK_FORMAT_R32_SFLOAT,sw,sh);make(pick,VK_FORMAT_R8_UNORM,sw,sh);
                if(c.bidi){make(pend_depth,VK_FORMAT_R32_SFLOAT,dw,dh);make(rebuilt8,fmt);make(bidi_held,direct_shown?shown_fmt:fmt);}
                const bool cx=c.bidi&&c.context;
                for(auto& x:context) make(x,VK_FORMAT_R8_UNORM,cx?dw:1,cx?dh:1);
                make(context_tile,VK_FORMAT_R32_SFLOAT,cx?(dw+15)/16:1,cx?(dh+15)/16:1);
            }
            // (bindings 17, 19, 21, 23 - the second side's own images once - are read no more: the side's bound there)
            Image ss=storage(side),ms=storage(side_match),pks=storage(pick),dfs=storage(depth_full);
            const auto composition=shaders+(fmt==VK_FORMAT_R16G16B16A16_UNORM?"/compose16u.spv":"/compose.spv");
            compose.create(ctx,composition,{},sizeof(ComposePush),
                {&current,&a0,&a1,&ed0,&ed1,&motion[0],&motion[1],&motion[2],&rendered,&out,&e0,&e1,&mv,&depth,&ad0,&ad1,
                 &ss,&ss,&side,&side,&ms,&ms,&side_match,&side_match,
                 &context[0],&context[1],&context[2],&pks,&pick,&dfs,&depth_full});
            if(c.bidi) {
                if(c.compute!=1||c.period!=2||c.fast_speed>0)throw std::invalid_argument("frame reuse two-sided rebuild: 1 of 2, CPU cadence");
                if(c.context&&c.merge_k>0)throw std::invalid_argument("frame reuse two-sided rebuild: the outline context and merge_k share the merge's match terms");
                compose_b.create(ctx,composition,{},sizeof(ComposePush),
                    {&current,&a0,&a1,&ed0,&ed1,&motion[0],&motion[1],&motion[2],&rendered,&out,&e0,&e1,&mv,&pend_depth,&ad0,&ad1,
                     &ss,&ss,&side,&side,&ms,&ms,&side_match,&side_match,
                     &context[0],&context[1],&context[2],&pks,&pick,&dfs,&depth_full});
                if(c.context) {
                    Image cns=storage(context[0]),c0s=storage(context[1]),c1s=storage(context[2]),cts=storage(context_tile);
                    context_kernel.create(ctx,shaders+"/context.spv",{},sizeof(ContextPush),
                        {&pend_depth,&anchor_depth[0],&anchor_depth[1],&cns,&c0s,&c1s,&cts,&context_tile});
                }
            }
            if(c.display_filter) {
                for(auto& x:display_history) make(x,VK_FORMAT_R16G16B16A16_SFLOAT);
                if(!direct_shown)make(display_out,shown_fmt);   // (a stage of its own writes held / bidi_held instead)
                // the images are bound per frame (filter); these give the set's layout
                Image hc=storage(display_history[0]),o=storage(direct_shown?bidi_held:display_out);
                display.create(ctx,shaders+"/display"+kernel_suffix(fmt),{},sizeof(DisplayPush),
                    {&anchor[0],&rendered,&motion[0],&depth,&display_history[1],&current,&hc,&o});
            }
            if(!c.pair_kernel) make_scheduler();
        } catch(...) {destroy();throw;}
    }
    std::vector<Image*> images() {
        return {&native_input,&native_answer,&current,&anchor[0],&anchor[1],&edit[0],&edit[1],&rendered,&shown,&motion[0],&motion[1],&motion[2],
                &composed,&depth,&anchor_depth[0],&anchor_depth[1],&exposure,&anchor_exposure[0],&anchor_exposure[1],
                &display_history[0],&display_history[1],&display_out,&output_copy,&held,&game_motion,
                &pend_depth,&side,&side_match,&rebuilt8,&bidi_held,&context[0],&context[1],&context[2],&context_tile,&depth_full,&pick};
    }
    void destroy(Borrow& b) {
        if(b.pool) vkDestroyDescriptorPool(ctx.device,b.pool,nullptr);
        for(auto v:b.views) vkDestroyImageView(ctx.device,v,nullptr);
    }
    void destroy() {
        if(encode.device)encode.destroy();
        if(decode.device)decode.destroy();
        if(guides.device)guides.destroy();
        if(compose.device)compose.destroy();
        if(compose_b.device)compose_b.destroy();
        if(context_kernel.device)context_kernel.destroy();
        if(linear)vkDestroySampler(ctx.device,linear,nullptr);
        if(display.device)display.destroy();
        for(auto& b:borrowed) destroy(b);
        if(tail_pool)vkDestroyCommandPool(ctx.device,tail_pool,nullptr);   // (its recordings with it)
        if(compute_tail_pool)vkDestroyCommandPool(ctx.device,compute_tail_pool,nullptr);
        if(balance_pool)vkDestroyQueryPool(ctx.device,balance_pool,nullptr);
        for(auto im:images()) if(im->handle) ctx.destroy(*im);
    }
    ~Impl(){cancel();destroy();}
    void collect() {
        for(size_t i=borrowed.size();i-->0;) if(borrowed[i].uses.ready()) {
            destroy(borrowed[i]);borrowed.erase(borrowed.begin()+i);
        }
    }
    // ---- Config::delay ----------------------------------------------------------------------------------------------
    // A second part's recording: one whose last run finished, else a new one.
    static void attach(const nr::gpu::Ticket& parent,const nr::gpu::Ticket& child) {
        if(!parent||parent==child)return;
        std::scoped_lock lock(parent->mutex,child->mutex);
        // (re-armed: an old fallback time must not retire it while the new parent records)
        child->touched=std::chrono::steady_clock::now();child->touched_at=nr::gpu::submissions.load();
        if(std::find(parent->children.begin(),parent->children.end(),child)==parent->children.end()) {
            parent->children.push_back(child);child->parents.push_back(parent);
        }
    }
    size_t begin_tail() {
        const uint8_t pool=recording_pool();
        if(pool==1 && !compute_tail_pool) {
            VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pi.queueFamilyIndex=compute_family;
            check(vkCreateCommandPool(ctx.device,&pi,nullptr,&compute_tail_pool));
        }
        if(pool==1 && !family_said) {
            family_said=true;
            nr::logf("[nr] frame reuse: a compute-only command list - its second parts recorded for the compute family %u",compute_family);
        }
        size_t i=0;
        for(;i<tails.size();++i) if(int(i)!=waiting && tails[i].ran && tails[i].pool==pool && nr::gpu::ready(tails[i].recording)) break;
        if(i==tails.size()) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool?compute_tail_pool:tail_pool;
            ai.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;ai.commandBufferCount=1;
            Tail t;t.pool=pool;check(vkAllocateCommandBuffers(ctx.device,&ai,&t.cb));tails.push_back(std::move(t));
        } else check(vkResetCommandBuffer(tails[i].cb,0));
        VkCommandBufferInheritanceInfo inh{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        bi.pInheritanceInfo=&inh;
        check(vkBeginCommandBuffer(tails[i].cb,&bi));
        tails[i].ran=false;tails[i].picture=true;tails[i].from_bidi=false;tails[i].recording=std::make_shared<nr::gpu::Recording>();
        attach(nr::gpu::current(),tails[i].recording);
        memory(tails[i].cb);if(cfg.mark)cfg.mark(tails[i].cb,100);   // (measurement: the second part's start / end)
        return i;
    }
    // The waiting second part, run in `cmd` (attached to the recording made now: what it uses lives until that one is
    // done); keep: its answer is this recording's output, else it goes nowhere (flush).
    void run_waiting(VkCommandBuffer cmd,bool keep) {
        if(waiting<0)return;
        Tail& t=tails[size_t(waiting)];
        if(t.pool!=recording_pool()) {   // recorded for another queue family than this list's: it cannot run here - the
            // stream starts afresh (its recording sealed unrun, so what it used is let go; nothing of it is shown)
            t.recording->seal();t.ran=true;t.picture=false;waiting=-1;held_valid=false;
            valid=previous=false;display_valid=false;bidi_pending=bidi_held_valid=false;reset_requested=true;
            nr::logf("[nr] frame reuse: its second part was recorded for another queue family than this command list's "
                     "(NR moved between a graphics and a compute list): started afresh");
            return;
        }
        attach(nr::gpu::current(),t.recording);t.recording->seal();
        memory(cmd);vkCmdExecuteCommands(cmd,1,&t.cb);memory(cmd);
        t.ran=true;waiting=-1;held_valid=keep&&t.picture;held_bidi=t.from_bidi;uses.touch();
    }
    void cancel() {abandoned=true;valid=previous=false;age=guide_count=phase=0;}
    void advance(VkCommandBuffer cmd) {
        if (advanced || waiting<0) return;
        const uint32_t q=uint32_t(serial%kTimed)*4;
        vkCmdResetQueryPool(cmd,balance_pool,q,4);
        stamp(cmd,q);
        run_waiting(cmd,true);
        stamp(cmd,q+1);
        advanced=true;
    }
    void reset() {reset_requested=true;}
    // `src` (GENERAL) into `dst`, which goes from `from` to `to`: a copy in the same format, else a nearest blit
    void store(VkCommandBuffer cmd,const Image& src,VkImage dst,VkFormat dst_format,VkImageLayout from,VkImageLayout to) {
        // A blit to an sRGB destination would encode the values a second time. Convert
        // channel order/precision in a private UNORM target first, then copy its bytes.
        const VkFormat fmt = code_format(dst_format);
        if (fmt != dst_format && src.format != fmt) {
            if (!output_copy.handle) {
                output_copy=ctx.image(cfg.width,cfg.height,fmt,true,false);
                output_copy.layout=VK_IMAGE_LAYOUT_GENERAL;
                barrier(cmd,output_copy.handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
            }
            store(cmd,src,output_copy.handle,fmt,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
            store(cmd,output_copy,dst,dst_format,from,to);
            return;
        }
        barrier(cmd,src.handle,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(cmd,dst,from,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if(src.format==fmt) {
            VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={cfg.width,cfg.height,1};
            vkCmdCopyImage(cmd,src.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
        } else {
            VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            blit.srcOffsets[1]=blit.dstOffsets[1]={int32_t(cfg.width),int32_t(cfg.height),1};
            nrvk::blit(ctx.physical,ctx.device,cmd,src.handle,src.format,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,dst_format,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
        }
        barrier(cmd,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,to);
        barrier(cmd,src.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    }
    void init(VkCommandBuffer cmd) {
        if(initialized)return;
        for(auto im:images()) {
            if(!im->handle)continue;
            barrier(cmd,im->handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
            VkClearColorValue zero{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(cmd,im->handle,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&range);
        }
        memory(cmd);initialized=true;
    }
    // Transient descriptor sets for copies and filters.
    Borrow pool(uint32_t images,uint32_t buffers) {
        Borrow b;b.uses.touch();
        std::vector<VkDescriptorPoolSize> sizes;
        if(images)sizes.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,images}),sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,images});
        if(buffers)sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,buffers*2});
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=images+buffers;
        pi.poolSizeCount=uint32_t(sizes.size());pi.pPoolSizes=sizes.data();
        check(vkCreateDescriptorPool(ctx.device,&pi,nullptr,&b.pool));return b;
    }
    VkDescriptorSet set_of(Borrow& b,const nrvk::Kernel& k) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=b.pool;
        ai.descriptorSetCount=1;ai.pSetLayouts=&k.dsl;VkDescriptorSet set{};check(vkAllocateDescriptorSets(ctx.device,&ai,&set));return set;
    }
    // src (sampled) -> dst (storage), the frame's extent of dst
    void copy_dispatch(VkCommandBuffer cmd,Borrow& b,const nrvk::Kernel& k,const ColourFrame& src,const ColourFrame& dst) {
        const VkDescriptorSet set=set_of(b,k);
        VkDescriptorImageInfo ii[2]={{motion[0].sampler,view(src,b),VK_IMAGE_LAYOUT_GENERAL},{{},view(dst,b),VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2]{};
        for(uint32_t i=0;i<2;++i){w[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w[i].dstSet=set;w[i].dstBinding=i;w[i].descriptorCount=1;
            w[i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[i].pImageInfo=&ii[i];}
        vkUpdateDescriptorSets(ctx.device,2,w,0,nullptr);
        dispatch(cmd,k,nullptr,0,dst.width,dst.height,set);
    }
    VkImageView view(const ColourFrame& f,Borrow& b) {
        if(f.usage && !(f.usage&VK_IMAGE_USAGE_SAMPLED_BIT))
            throw std::invalid_argument("frame reuse guide image cannot be sampled");
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=f.image;
        ci.viewType=VK_IMAGE_VIEW_TYPE_2D;ci.format=f.format;
        ci.subresourceRange={aspects(f.format)&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView v{};check(vkCreateImageView(ctx.device,&ci,nullptr,&v));b.views.push_back(v);return v;
    }
    void transform(VkCommandBuffer cmd,const nrvk::Kernel& kernel,Image& src,Image& dst) {
        Borrow b=pool(2,0);
        b.uses.touch();
        copy_dispatch(cmd,b,kernel,frame(src),frame(dst));
        borrowed.push_back(std::move(b));
    }
    void copy_input(VkCommandBuffer cmd,const ColourFrame& src,Image& working) {
        if(!cfg.scrgb){copy(cmd,src,working);return;}
        copy(cmd,src,native_input);
        transform(cmd,encode,native_input,working);
    }
    void record_guides(VkCommandBuffer cmd,const renodx::StageFrame& fr) {
        Borrow b;b.uses.touch();
        try {
            VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=2;pi.pPoolSizes=sizes;
            check(vkCreateDescriptorPool(ctx.device,&pi,nullptr,&b.pool));
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=b.pool;
            ai.descriptorSetCount=1;ai.pSetLayouts=&guides.dsl;VkDescriptorSet set{};check(vkAllocateDescriptorSets(ctx.device,&ai,&set));
            std::array<VkDescriptorImageInfo,6> ii{};
            ii[0]={motion[0].sampler,view(fr.motion,b),VK_IMAGE_LAYOUT_GENERAL};
            ii[1]={depth.sampler,fr.depth.image?view(fr.depth,b):depth.view,VK_IMAGE_LAYOUT_GENERAL};
            ii[2]={exposure.sampler,fr.exposure.image?view(fr.exposure,b):exposure.view,VK_IMAGE_LAYOUT_GENERAL};
            ii[3]={{},motion[serial%3].view,VK_IMAGE_LAYOUT_GENERAL};ii[4]={{},depth.view,VK_IMAGE_LAYOUT_GENERAL};ii[5]={{},exposure.view,VK_IMAGE_LAYOUT_GENERAL};
            std::array<VkWriteDescriptorSet,6> writes{};
            for(uint32_t i=0;i<6;++i) {
                writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;
                writes[i].descriptorCount=1;writes[i].descriptorType=i<3?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[i].pImageInfo=&ii[i];
            }
            vkUpdateDescriptorSets(ctx.device,6,writes.data(),0,nullptr);
            GuidePush p{{motion[0].w,motion[0].h},{depth.w,depth.h},{fr.motion_x,fr.motion_y},{fr.motion.width,fr.motion.height},
                        {fr.depth_x,fr.depth_y},{fr.depth.width,fr.depth.height},{fr.motion_scale_x,fr.motion_scale_y},
                        (fr.depth.image?1u:0u)|(fr.exposure.image?2u:0u)};
            barrier(cmd,fr.motion.image,fr.motion.before,VK_IMAGE_LAYOUT_GENERAL);
            if(fr.depth.image)barrier(cmd,fr.depth.image,fr.depth.before,VK_IMAGE_LAYOUT_GENERAL,aspects(fr.depth.format));
            if(fr.exposure.image)barrier(cmd,fr.exposure.image,fr.exposure.before,VK_IMAGE_LAYOUT_GENERAL);
            dispatch(cmd,guides,&p,sizeof p,std::max(motion[0].w,depth.w),std::max(motion[0].h,depth.h),set);
            barrier(cmd,fr.motion.image,VK_IMAGE_LAYOUT_GENERAL,fr.motion.after);
            if(fr.depth.image)barrier(cmd,fr.depth.image,VK_IMAGE_LAYOUT_GENERAL,fr.depth.after,aspects(fr.depth.format));
            if(fr.exposure.image)barrier(cmd,fr.exposure.image,VK_IMAGE_LAYOUT_GENERAL,fr.exposure.after);
            borrowed.push_back(std::move(b));
        } catch(...) {destroy(b);throw;}
    }
    // display.comp into `out` (display_out, or a stage's own shown picture) (with the answer / vectors / depth of a
    // frame recorded earlier)
    void filter_image(VkCommandBuffer cmd,uint32_t input,const Image& nr,const Image& mot,const Image& dep,const Image& out) {
        Image* inputs[3]={&anchor[0],&anchor[1],&current};
        const Image* bind[8]={inputs[input],&nr,&mot,&dep,&display_history[display_bank^1u],inputs[display_prev],
                              &display_history[display_bank],&out};
        Borrow b=pool(8,0);const VkDescriptorSet set=set_of(b,display);
        VkDescriptorImageInfo ii[8];VkWriteDescriptorSet w[8]{};
        for(uint32_t i=0;i<8;++i) {
            const bool written=i>=6;ii[i]={written?VkSampler{}:bind[i]->sampler,bind[i]->view,VK_IMAGE_LAYOUT_GENERAL};
            w[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w[i].dstSet=set;w[i].dstBinding=i;w[i].descriptorCount=1;
            w[i].descriptorType=written?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[i].pImageInfo=&ii[i];
        }
        vkUpdateDescriptorSets(ctx.device,8,w,0,nullptr);
        DisplayPush p{{cfg.width,cfg.height},{0,0},{motion[0].w,motion[0].h},{1.f,1.f},{0,0},{depth.w,depth.h},
                      (display_valid?1u:0u)|(inverted?4u:0u),{0,0,0}};
        dispatch(cmd,display,&p,sizeof p,cfg.width,cfg.height,set);
        borrowed.push_back(std::move(b));
        display_valid=true;display_bank^=1u;display_prev=input;
    }
    // ---- the two-sided rebuild ---------------------------------------------------------------------------------------
    // the kept rebuilt frame (current, pend_depth, vectors in motion[pend_slot]) from the answers in banks `past` and
    // `next` (this recording's): the forward field (mode 4) into the free vector slot, its depth at every pixel (mode 7) and
    // the nearer surfaces of its pixels (mode 6), mode 2 from each side (the second merges), mode 3 -> `shown`
    void compose_two_sided(VkCommandBuffer cmd,uint32_t past,uint32_t next) {
        const uint32_t now=uint32_t(serial%3),free=(now+1u)%3u;
        if(pend_slot!=(now+2u)%3u)throw std::logic_error("frame reuse: the kept frame is not the one before");
        auto f=push(4,next,1);f.current_motion=now;f.previous=pend_slot;
        dispatch(cmd,compose_b,&f,sizeof f,composed.w,composed.h);
        copy(cmd,frame(composed),motion[free]);
        auto z=push(7,next,0);dispatch(cmd,compose_b,&z,sizeof z,cfg.width,cfg.height);
        auto n=push(6,next,0);dispatch(cmd,compose_b,&n,sizeof n,cfg.width,cfg.height);
        auto a=push(2,past,pend_age);a.current_motion=pend_slot;a.previous=0;a.blend=1;a.side=1;
        dispatch(cmd,compose_b,&a,sizeof a,cfg.width,cfg.height);
        auto b=push(2,next,1);b.current_motion=free;b.previous=now;b.blend=1;b.side=2;   // (previous: the next frame's own vectors)
        dispatch(cmd,compose_b,&b,sizeof b,cfg.width,cfg.height);
        auto fl=push(3,next,0);fl.side=3;dispatch(cmd,compose_b,&fl,sizeof fl,cfg.width,cfg.height);
    }
    // context.comp: the outline context classes of the kept depth (which 0) or an anchor's depth (1 + its bank), at the
    // depth's own size, once each - read by compose's mode 2 of both sides (Config::context)
    void context_map(VkCommandBuffer cmd,uint32_t which) {
        if(!cfg.bidi||!cfg.context)return;
        ContextPush c{{cfg.width,cfg.height},0,which,inverted};
        dispatch(cmd,context_kernel,&c,sizeof c,context[which].w,context[which].h);
        c.mode=1;dispatch(cmd,context_kernel,&c,sizeof c,context[which].w,context[which].h);
    }
    void bidi_show(VkCommandBuffer cmd,const Image& src) {
        store(cmd,src,held.handle,held.format,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    }
    // ... no new picture: the one held from the computed frame before (none yet: the game's frame / a second part without one);
    // in_place: nothing after this in the part writes bidi_held - the output is stored from it (no copy into `held`)
    void bidi_show_held(VkCommandBuffer cmd,size_t tail_index,bool in_place=false) {
        if(bidi_held_valid && in_place)tails[tail_index].from_bidi=true;
        else if(bidi_held_valid)store(cmd,bidi_held,held.handle,held.format,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        else tails[tail_index].picture=false;
    }
    void stamp(VkCommandBuffer cmd,uint32_t q) {memory(cmd);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,balance_pool,q);}
    // The recordings 3 and 4 frames back (done by now, else skipped): a computed frame after a rebuilt one and before a
    // rebuilt one -> its slot (the rebuilt frame's second part + its own first part) against the next one (its second
    // part + the rebuilt frame's first part); the median of 9 such pairs moves the share.
    void balance() {
        if(serial<6||!cfg.balance_split)return;
        const auto slot_ms=[&](uint64_t f,double* out)->bool {
            const Timed& t=timed[f%kTimed];if(t.serial!=f)return false;
            uint64_t q[8]{};
            if(vkGetQueryPoolResults(ctx.device,balance_pool,uint32_t(f%kTimed)*4,4,sizeof q,q,16,
                                     VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)!=VK_SUCCESS)return false;
            if(!q[1]||!q[3]||!q[5]||!q[7])return false;
            *out=double((q[2]-q[0])+(q[6]-q[4]))*tick_ns/1e6;return true;
        };
        const auto rebuilt=[&](uint64_t f){return timed[f%kTimed].kind=='R';};
        const uint64_t a=serial-4,b=serial-3;
        if(timed[a%kTimed].serial!=a||timed[a%kTimed].kind!='C'||!rebuilt(a-1)||!rebuilt(b))return;
        double sa=0,sb=0;
        if(!slot_ms(a,&sa)||!slot_ms(b,&sb)||sa+sb<=0)return;
        static const bool trace_balance=std::getenv("NR_DEBUG_REUSE_TIMING")!=nullptr;
        if(trace_balance)nr::logf("[nr] frame reuse GPU pair %llu NR %.5f ms rebuilt %.5f ms share %.4f",(unsigned long long)a,sa,sb,share);
        balance_errors.push_back(float((sa-sb)/(sa+sb)));
        if(balance_errors.size()<9)return;
        std::sort(balance_errors.begin(),balance_errors.end());
        const float e=balance_errors[balance_errors.size()/2];balance_errors.clear();
        share=std::clamp(share-0.5f*e,0.05f,0.95f);++balance_moves;
    }
    // A delayed stage at the end of a recording: the second part ends and waits; this frame's output is the frame
    // before's answer (`held`, from the second part run at this recording's start), or - none yet - the game's frame.
    // (Every way through the stage copied the frame's colour first: it is in its `after` layout.)
    void finish(VkCommandBuffer head,VkCommandBuffer tail,const renodx::StageFrame& fr,size_t index,bool native=false) {
        if(!cfg.delay)return;
        pair_sources.show(held_valid,cfg.bidi);
        memory(tail);if(cfg.mark)cfg.mark(tail,101);check(vkEndCommandBuffer(tail));waiting=int(index);
        const bool in_place=fr.output.image==fr.colour.image;
        const auto from=in_place?fr.colour.after:fr.output.before;
        struct End {Impl* s;VkCommandBuffer c;~End(){s->stamp(c,uint32_t(s->serial%kTimed)*4+3);}} end{this,head};
        if(held_valid) {
            Image& picture=held_bidi?bidi_held:held;
            if(cfg.scrgb && !native)transform(head,decode,picture,shown);
            store(head,cfg.scrgb && !native?shown:picture,fr.output.image,fr.output.format,from,fr.output.after);return;}
        if(in_place) {if(from!=fr.output.after)barrier(head,fr.output.image,from,fr.output.after);return;}
        barrier(head,fr.colour.image,fr.colour.after,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(head,fr.output.image,from,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={cfg.width,cfg.height,1};
        vkCmdCopyImage(head,fr.colour.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,fr.output.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
        barrier(head,fr.colour.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,fr.colour.after);
        barrier(head,fr.output.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,fr.output.after);
    }
    ComposePush push(uint32_t mode,uint32_t bank,uint32_t a) {
        return {{cfg.width,cfg.height},{motion[0].w,motion[0].h},mode,uint32_t(serial%3),bank,a,valid?1u:0u,previous?1u:0u,.5f,1.f/255.f,protection,inverted,0u,cfg.merge_k,cfg.stay_fixed/255.f,
                (cfg.fwd_check?1u:0u)|(cfg.bidi&&cfg.context?2u:0u)|((cfg.scrgb||cfg.fwd_check)?4u:0u)};
    }
    void write_native(VkCommandBuffer cmd,Image& src,Image& dst) {
        if(cfg.scrgb) transform(cmd,decode,src,dst);
        else store(cmd,src,dst.handle,dst.format,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    }
    uint32_t render_pair(VkCommandBuffer& cmd,renodx::StageFrame* odd,renodx::StageFrame& even,const PairNetwork& network) {
        uses.touch();init(cmd);
        if(even.reset || reset_requested || !odd) { valid=false;display_valid=false;reset_requested=false; }
        protection=cfg.protection;inverted=even.depth_inverted?1u:0u;
        if(odd) {
            record_guides(cmd,*odd);
            copy_input(cmd,odd->colour,current);
            copy(cmd,frame(depth),pend_depth);
            context_map(cmd,0);
            pend_slot=uint32_t(serial++%3);pend_age=1;
        }
        const uint32_t past=latest,bank=valid?(past^1u):0u;
        record_guides(cmd,even);
        copy_input(cmd,even.colour,anchor[bank]);copy(cmd,frame(depth),anchor_depth[bank]);context_map(cmd,1u+bank);
        auto f=even;
        f.colour=frame(cfg.scrgb?native_input:anchor[bank]);f.output=frame(cfg.scrgb?native_answer:rendered);
        f.motion=frame(motion[serial%3]);
        if(odd && valid) {auto pc=push(0,bank,2);dispatch(cmd,compose,&pc,sizeof pc,composed.w,composed.h);f.motion=frame(composed);}
        f.motion_x=f.motion_y=0; f.motion_scale_x=f.motion_scale_y=1.f;
        if(even.depth.image){f.depth=frame(anchor_depth[bank]); f.depth_x=f.depth_y=0;}
        const uint32_t calls=network(cmd,f);
        if(calls==frame_pair::failed_calls){reset_requested=true;return calls;}
        memory(cmd);
        if(cfg.scrgb)transform(cmd,encode,native_answer,rendered);
        auto ep=push(1,bank,0);dispatch(cmd,compose,&ep,sizeof ep,cfg.width,cfg.height);
        const bool previous_anchor=valid;
        previous=valid; valid=true; latest=bank;
        if(odd && previous_anchor) {
            compose_two_sided(cmd,past,bank);
            store(cmd,shown,rebuilt8.handle,rebuilt8.format,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
            filter_image(cmd,2,rebuilt8,motion[pend_slot],pend_depth,display_out);
            write_native(cmd,display_out,rebuilt8);
            odd->colour=odd->output=frame(rebuilt8);
        }
        filter_image(cmd,bank,rendered,motion[serial%3],depth,display_out);
        write_native(cmd,display_out,bidi_held);
        even.colour=even.output=frame(bidi_held);
        ++serial;return calls;
    }

    uint32_t batch_step(VkCommandBuffer head,VkCommandBuffer tail,const renodx::StageFrame& fr,size_t tail_index,
                        const Batch& batch,bool evaluate) {
        auto f=fr;
        f.motion=frame(motion[serial%3]);f.motion_x=f.motion_y=0;f.motion_scale_x=f.motion_scale_y=1.f;
        f.depth_x=f.depth_y=0;
        if(evaluate) {
            copy(head,fr.colour,anchor[0]);
            f.colour=f.output=frame(anchor[0]);
            if(fr.depth.image)f.depth=frame(depth);
            if(fr.exposure.image)f.exposure=frame(exposure);
            f.reset=!valid || fr.reset;
            auto* odd=bidi_pending && valid?&pending_input:nullptr;
            VkCommandBuffer cur=head;
            const uint32_t calls=batch(cur,odd,f,tail,share);
            if(calls==frame_pair::failed_calls)return calls;
            memory(cur);
            if(odd)store(tail,borrow_image(odd->colour),held.handle,held.format,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
            else tails[tail_index].picture=false;
            store(tail,borrow_image(f.colour),bidi_held.handle,bidi_held.format,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
            bidi_held_valid=valid=true;bidi_pending=false;
            return calls;
        }
        copy(head,fr.colour,current);f.colour=f.output=frame(current);
        if(fr.depth.image){copy(head,frame(depth),pend_depth);f.depth=frame(pend_depth);}
        if(fr.exposure.image){copy(head,frame(exposure),anchor_exposure[1]);f.exposure=frame(anchor_exposure[1]);}
        pending_input=f;bidi_pending=true;
        bidi_show_held(tail,tail_index,true);return 0;
    }
    static Image borrow_image(const ColourFrame& f) {
        Image im{};im.handle=f.image;im.format=f.format;im.w=f.width;im.h=f.height;
        im.layout=VK_IMAGE_LAYOUT_GENERAL;return im;
    }
    Result record(VkCommandBuffer head,const renodx::StageFrame& fr,const Complete& complete,const HistoryPairs& pairs,const Batch& batch={}) {
        if(abandoned)throw std::logic_error("retired frame reuse instance");
        VkCommandBuffer cmd=head;   // a delayed stage goes on in its second part's recording (`tail`) after the split
        if(cfg.pair_kernel || cfg.batch_only!=bool(batch) || !cmd || (!complete && !batch) || fr.colour.width!=cfg.width || fr.colour.height!=cfg.height || fr.colour.format!=cfg.frame_format || fr.output.width!=cfg.width || fr.output.height!=cfg.height || !supports(cfg.frame_format,fr.output.format))
            throw std::invalid_argument("frame reuse extent/format/callback mismatch");
        if(!fr.motion.image || !fr.fast_motion_available || fr.motion.width!=cfg.guides.motion_width || fr.motion.height!=cfg.guides.motion_height)
            throw std::invalid_argument("frame reuse requires the game's motion vectors at the size it was made for");
        if(fr.depth.image && (fr.depth.width!=cfg.guides.depth_width || fr.depth.height!=cfg.guides.depth_height))
            throw std::invalid_argument("frame reuse requires the game's depth at the size it was made for");
        result={};collect();
        uses.touch();
        const auto mark=[&](uint32_t step){if(cfg.mark)cfg.mark(cmd,step);};
        mark(0);
        const bool reset=fr.reset||reset_requested;
        if(reset){valid=previous=false;phase=age=guide_count=0;reset_requested=false;display_valid=false;
            bidi_pending=bidi_held_valid=false;}   // (nothing of the scene before is shown after a reset)
        init(cmd);
        if (!advanced || reset) held_valid=false;
        VkCommandBuffer tail{};size_t tail_index=0;
        if(cfg.delay) {
            balance();
            const uint32_t q=uint32_t(serial%kTimed)*4;timed[serial%kTimed]={serial,0};
            if (!advanced) {
                vkCmdResetQueryPool(head,balance_pool,q,4);stamp(head,q);
                run_waiting(head,!reset);
                stamp(head,q+1);
            }
            advanced=false;
            result.shown_late=held_valid?2u:0u;   // the frame before's second part: its answer is this frame's output
            stamp(head,q+2);
            tail_index=begin_tail();tail=tails[tail_index].cb;
        }
        // (a delayed stage: what this recording touches from here on is kept until its second part has run)
        nr::gpu::Scope tail_scope(cfg.delay?tails[tail_index].recording:nr::gpu::current());
        if(cfg.delay)uses.touch();
        protection=cfg.protection;inverted=fr.depth_inverted?1u:0u;
        record_guides(cmd,fr);guide_count=std::min(guide_count+1u,3u);
        mark(1);
        // the guides pass left the game's motion / depth / exposure in their `after` layouts
        renodx::StageFrame g=fr;g.motion.before=fr.motion.after;g.depth.before=fr.depth.after;g.exposure.before=fr.exposure.after;
        if(cfg.delay && !batch) {   // the callback reads the stage's own copy of the game's vectors (same texels)
            if(game_motion.handle && (game_motion.format!=fr.motion.format || game_motion.w!=fr.motion.width || game_motion.h!=fr.motion.height))
                throw std::logic_error("frame reuse: the game's motion vectors changed format");
            if(!game_motion.handle) {
                // sampled and copied only (a vector format need not take storage)
                game_motion=ctx.image(fr.motion.width,fr.motion.height,fr.motion.format,true,false,false);game_motion.layout=VK_IMAGE_LAYOUT_GENERAL;
                barrier(cmd,game_motion.handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
            }
            // (the region the game rendered into, from its base: the copy's own base is 0)
            copy(cmd,g.motion,game_motion,fr.motion_x,fr.motion_y);
            g.motion=frame(game_motion);g.motion_x=g.motion_y=0;
            g.motion.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        }
        const bool evaluate=phase==0 || !valid;
        timed[serial%kTimed].kind=evaluate?'C':'R';
        if(batch) {
            result.network_calls=batch_step(head,tail,fr,tail_index,batch,evaluate);
            result.completed=evaluate;
        } else if(evaluate){
            const uint32_t bank=valid?(latest^1u):0u,past=latest;const bool had_valid=valid;   // (the past answer's bank)
            copy_input(cmd,fr.colour,anchor[bank]);copy(cmd,frame(depth),anchor_depth[bank]);
            context_map(cmd,1u+bank);
            if(fr.exposure.image)copy(cmd,frame(exposure),anchor_exposure[bank]);
            mark(2);
            // the network's histories go on from the frames before (NR ran on them whether or not this stage did: switched
            // on, back from a pause); only the game's own reset restarts them
            auto sf=g;sf.colour=frame(cfg.scrgb?native_input:anchor[bank]);sf.output=frame(cfg.scrgb?native_answer:rendered);sf.reset=fr.reset;
            const uint32_t gap=std::min(guide_count,2u);current_gap=std::max(gap,1u);
            if(gap==1u){sf.motion=g.motion;}
            else {auto cp=push(0,bank,gap);dispatch(cmd,compose,&cp,sizeof cp,composed.w,composed.h);sf.motion=frame(composed);sf.motion_x=sf.motion_y=0;sf.motion_scale_x=sf.motion_scale_y=1;
            }
            if(fr.depth.image){sf.depth=frame(anchor_depth[bank]);sf.depth_x=sf.depth_y=0;}
            if(fr.exposure.image)sf.exposure=frame(anchor_exposure[bank]);
            splitting=tail;result.network_calls=complete(cmd,sf);splitting={};
            if(tail){memory(cmd);cmd=tail;}
            if(result.network_calls!=frame_pair::failed_calls) {
            memory(cmd);mark(3);mark(7);
            if(cfg.scrgb)transform(cmd,encode,native_answer,rendered);
            if(phase+1u==cfg.compute){auto ep=push(1,bank,0);dispatch(cmd,compose,&ep,sizeof ep,cfg.width,cfg.height);}
            mark(4);mark(5);
            previous=valid;valid=true;latest=bank;age=0;guide_count=0;result.completed=true;
            if(fr.reset)display_valid=false;
            if(bidi_pending && had_valid) {
                compose_two_sided(cmd,past,bank);
                if(cfg.display_filter) {
                    store(cmd,shown,rebuilt8.handle,rebuilt8.format,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
                    filter_image(cmd,2,rebuilt8,motion[pend_slot],pend_depth,held);   // (the next recording's output)
                } else bidi_show(cmd,shown);
            } else bidi_show_held(cmd,tail_index);
            if(cfg.display_filter) {
                filter_image(cmd,bank,rendered,motion[serial%3],depth,bidi_held);
            } else store(cmd,rendered,bidi_held.handle,bidi_held.format,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
            bidi_held_valid=true;bidi_pending=false;
            mark(6);
            }
        } else {   // kept for the next recording (composed there from both answers); the held picture out
            if(!valid)throw std::logic_error("reuse without a completed frame");
            copy_input(cmd,fr.colour,current);copy(cmd,frame(depth),pend_depth);context_map(cmd,0);age=phase+1u-cfg.compute;
            pend_slot=uint32_t(serial%3);pend_age=age;bidi_pending=true;mark(2);mark(3);mark(7);mark(4);mark(5);
            if(tail)cmd=tail;   // (delayed: the computed frame before's picture into `held` when this second part runs)
            bidi_show_held(cmd,tail_index,true);mark(6);
        }
        if(result.network_calls==frame_pair::failed_calls) {
            result.ok=false;result.completed=false;result.network_calls=0;result.shown_late=0;
            tails[tail_index].picture=false;held_valid=false;valid=false;display_valid=false;reset_requested=true;
        }
        result.next_late=tails[tail_index].picture?2u:0u;
        finish(head,tail,fr,tail_index,bool(batch));
        ++cpu_frames;if(evaluate)++cpu_computed;
        result.correction_age=age;phase=(phase+1)%cfg.period;++serial;return result;
    }
};
Stage::Stage(const HostDevice& h,const Config& c,const std::string& s):impl_(std::make_unique<Impl>(h,c,s)){}
Stage::~Stage()=default;
Stage::Result Stage::record(VkCommandBuffer cmd,const renodx::StageFrame& fr,const Complete& complete,const HistoryPairs& pairs){
    return impl_->record(cmd,fr,complete,pairs);}
void Stage::pass_input(VkCommandBuffer cmd,const renodx::StageFrame& fr) {
        if(fr.colour.image==fr.output.image) {
            if(fr.colour.before!=fr.output.after)barrier(cmd,fr.colour.image,fr.colour.before,fr.output.after);
            return;
        }
        barrier(cmd,fr.colour.image,fr.colour.before,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(cmd,fr.output.image,fr.output.before,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={fr.colour.width,fr.colour.height,1};
        vkCmdCopyImage(cmd,fr.colour.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,fr.output.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
        barrier(cmd,fr.colour.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,fr.colour.after);
        barrier(cmd,fr.output.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,fr.output.after);
    }
uint32_t Stage::render_pair(VkCommandBuffer& c,renodx::StageFrame* odd,renodx::StageFrame& even,const PairNetwork& net) {
    return impl_->render_pair(c,odd,even,net);
}
Stage::Result Stage::record_batch(VkCommandBuffer c,const renodx::StageFrame& fr,const Batch& batch) {
    return impl_->record(c,fr,{},{},batch);
}
void Stage::reset(){impl_->reset();}
void Stage::cancel(){impl_->cancel();}
bool Stage::recordings_done()const{
    for(const auto& t:impl_->tails) if(t.ran && !nr::gpu::ready(t.recording)) return false;
    return impl_->uses.ready();}
uint64_t Stage::image_bytes()const{return impl_->ctx.image_bytes;}
const Stage::Config& Stage::config()const{return impl_->cfg;}
uint32_t Stage::gap()const{return impl_->current_gap;}
Stage::Split Stage::split()const{return {impl_->splitting,impl_->splitting?impl_->share:0.f};}
float Stage::balance(uint32_t* moves)const{if(moves)*moves=impl_->balance_moves;return impl_->share;}
bool Stage::pending()const{return impl_->waiting>=0;}
bool Stage::can_flush_on(VkDevice device, uint32_t family) const {
    return impl_->ctx.device == device && impl_->ctx.family == family;
}
void Stage::advance(VkCommandBuffer cmd){impl_->advance(cmd);}
void Stage::flush(VkCommandBuffer cmd){impl_->advanced=false;impl_->uses.touch();impl_->run_waiting(cmd,false);
    impl_->bidi_pending=impl_->bidi_held_valid=false;}   // (nothing from before the gap is shown after it)
// Kept for the older offline reader. The product cadence has no motion-speed decision.
float Stage::speed()const{return -1.f;}
Stage::Stats Stage::stats()const{return {impl_->cpu_frames,impl_->cpu_computed,impl_->cpu_frames-impl_->cpu_computed,0.f};}
void Stage::history_weight(VkCommandBuffer,VkBuffer,uint32_t,float){}
}
