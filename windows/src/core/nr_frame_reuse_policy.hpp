#pragma once
#ifndef NR_N
#define NR_N(text) text
#endif
namespace nr::frame_reuse {
// Each string is also a language-file key. The caller publishes the same reason to the panel and log.
inline constexpr char direct_wait[] = NR_N("Waiting for D3D12 Direct: %s");
inline constexpr char d11_keyed[] = NR_N("Cannot run: D3D11: keyed-mutex vkQueueSubmit returned %d");
inline constexpr char d11_error[] = NR_N("Cannot run: D3D11: %s");
inline constexpr char direct_error[] = NR_N("Cannot run: D3D12 Direct: %s");
inline constexpr char detail_error[] = NR_N("Cannot run: %s");
inline constexpr char anchor_error[] = NR_N("Cannot run: a layer could not record its computed anchor");
inline constexpr char fast_programs[] = NR_N("Cannot run: the selected fast method has no compute programs");
inline constexpr char packed_motion[] = NR_N("Cannot run: feedback needs the packed motion of the computed anchor");
inline constexpr char packed_model[] = NR_N("Cannot run: the packed NR model could not be made");
inline constexpr char packed_eval[] = NR_N("Cannot run: the packed NR model could not execute");
inline constexpr char placement[] = NR_N("Cannot run: this placement needs the current frame before Present");
inline constexpr char interleaved[] = NR_N("Cannot run: frame reuse and Interleaved cannot be used together");
inline constexpr char guides_off[] = NR_N("Cannot run: Present guides are set to Never");
inline constexpr char motion_missing[] = NR_N("Cannot run: the game did not supply motion vectors");
inline constexpr char unsupported_format[] = NR_N("Cannot run: unsupported input or output texture format");
inline constexpr char chain_busy[] = NR_N("Cannot run: another layer uses the network scratch between the two halves");
inline constexpr char fg_unsynced[] = NR_N("Cannot run: frame generation has no synchronized inputs for the shown frame");
inline constexpr char ffx_chain[] = NR_N("Cannot run: the FSR camera extension cannot be paired with the shown frame");
inline constexpr char distortion_chain[] = NR_N("Cannot run: the FSR distortion input is in an unrecognized extension chain");
inline constexpr char dispatch_colour[] = NR_N("Cannot run: the FSR dispatch colour has not been paired with the shown frame");
inline constexpr char xe_list[] = NR_N("Cannot run: XeFG supplied no command list for this Present input");
inline constexpr char ngx_queue[] = NR_N("Cannot run: DLSS-G supplied no usable input queue");
inline constexpr char ngx_submit[] = NR_N("Cannot run: the DLSS-G input recording could not be submitted");
inline constexpr char depth_format[] = NR_N("Cannot run: unsupported depth texture format");
inline constexpr char guide_format[] = NR_N("Cannot run: unsupported guide texture format");
inline constexpr char copy_memory[] = NR_N("Cannot run: the motion-vector copy could not be allocated");
inline constexpr char hud_unsynced[] = NR_N("Cannot run: HUD composition has no inputs for the shown frame");
}
