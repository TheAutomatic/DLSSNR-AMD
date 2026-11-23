# ReShade 路线（`dlssnr_amd.addon64`）接入 D3D12 ACO 直通与 D3D11→D3D12 桥接规划

> **状态**：`009-dev` 研究与架构规划文档（暂不执行，待后续排期实施）。  
> **基准代码库**：`009-dev` 分支（基于 `v0.0.9m7` 及 `0.0.9m8` D3D12 显存与拷贝优化）。

---

## 一、文档定位与适用范围

OptiScaler / Standalone 路线（`dlssnr-amd.asi`）在 64 位 Windows 上已默认启用 D3D12 ACO 直通（Zero-Heap D3D12 ACO ELF）与 D3D11→D3D12 同步桥接，并在 `0.0.9m8` 完成了权重分块流式上传、Tier 2 Heap Aliasing、`lin_mv` 延迟分配、SDR 多 Pass `fast_lin` 扩展以及 D3D11 共享后备缓冲区原地处理等显存与带宽优化。

相比之下，ReShade 路线（`dlssnr_amd.addon64` / `dlssnr_amd.addon32`）目前在 Windows 上仍然对所有 D3D10 / D3D11 / D3D12 游戏统一走 **Vulkan `Session` 跨 API 桥接**。本文档旨在彻底拆解将 ReShade 路线迁入 D3D12 直通后端所需的接口解耦、边界处理与分阶段迁移方案。

### 1.1 目标适用范围（走 D3D12 直通 / D3D11→D3D12 桥接）
- 仅限 **64 位插件 `dlssnr_amd.addon64`**（需 64 位 `amdxc64.dll` 与 48-bit GPU VA）；
- 原生 Windows **D3D12** 游戏（RDNA4 / RDNA3，且 `[D3D12Passthrough] Enabled = 1`、canary `kHit`）；
- 原生 Windows **D3D11** 游戏（RDNA4 / RDNA3，支持 D3D11.4 共享围栏且 `[D3D12Passthrough] Enabled = 1`）。

### 1.2 明确保留在 Vulkan 路线的场景（不改动、自动保留/回退）
- **32 位 `dlssnr_amd.addon32`**（`__i386__`，`windows/build/x86/build32.sh` 保持零改动；插件日志已明确 `Native D3D12 is not supported by the 32-bit add-on`）；
- **原生 D3D10**（`nr_reshade_addon.cpp:3190-3217`：无法 `QueryInterface(ID3D11Device)` 的原生 D3D10 设备仍走 keyed-mutex `run_d3d10`）；
- **DX9（经 DXVK）与 DXVK 上的 D3D10/D3D11**；
- **原生 Vulkan 游戏**（含 RTX Remix 等直接 Vulkan 路径）；
- **非 RDNA3 / RDNA4 GPU**、`[D3D12Passthrough] Enabled = 0`、或 D3D12 canary 未通过时的自动回退。

---

## 二、现状架构盘点与可复用资产

### 2.1 `nr_reshade_addon.cpp` 当前帧链路盘点
- **主入口**：`on_render_technique()`（监听 `DLSS5_Feed`，`nr_reshade_addon.cpp:4248-4297`）→ `feed()` → `run_layers()` / `run_one()` / `run_pq()`。
- **D3D10 / D3D11 分支**（`nr_reshade_addon.cpp:3201-3218`）：
  - 若 `api == device_api::d3d10` 先探测 `QueryInterface(__uuidof(ID3D11Device))`，命中则转归 DXVK D3D11 路径；
  - 构造 `Session::D3D11Frame`，将 `tgt.colour`、`motion_image`、`depth` 交给 `s.run_d3d10()` / `s.run_d3d11()`。
- **D3D12 分支**（`nr_reshade_addon.cpp:3219-3246`）：
  - Windows 原生 D3D12 命中 `s.bridged(device)`，采用**队列级交接**：先调用 `queue->flush_immediate_command_list()`，再将 `ID3D12Device*`、`ID3D12CommandQueue*`、`tgt.colour`（`RENDER_TARGET`）、`motion`/`depth`（`PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE`）封装进 `Session::D3D12QueueFrame` 交由 `s.run_d3d12_queue()`。
- **三个强绑 Vulkan `session->record_raw*` 的外围辅助 Pass**：
  1. **HUD FIX**：`hudfix_end()`（`nr_reshade_addon.cpp:1706-1765`）→ `hudfix_record()`，通过 `H.stage`（`nr::wilsjo2::Stage`）与 `record_raw` / `record_raw_d3d11` 在 Vulkan 上跑 `hudfix.comp`。
  2. **Inspect 对比缩放视图**：`inspect_fit_show()`（`nr_reshade_addon.cpp:1882-1930`）通过 `raw_pass()` 与 `F.stage` 在 Vulkan 上跑 `wilsjo2_inspect.comp`。
  3. **HDR10 (PQ BT.2020) ↔ 线性 scRGB 转换**：`pq_begin()` / `pq_end()` / `pq_pass()`（`nr_reshade_addon.cpp:2004-2056`）通过 `raw_pass()` 与 `Q.stage` 在 Vulkan 上跑 `wilsjo2_finished.comp`（`kFinishedToLinear` / `kFinishedToPq`）。

### 2.2 仓库现有可复用 D3D12 资产（零着色器改动）
| 模块 | 复用能力 |
| :--- | :--- |
| `nr_d3d12_passthrough.{hpp,cpp}` | `[D3D12Passthrough] Enabled` 配置读写、RDNA3/RDNA4 架构判定、canary 校验、`status()`。 |
| `nr_d3d12_passthrough_net.cpp` | ACO ELF 网络执行、权重分块流式上传（`upload_weights_chunked()`）、无光流零描述符退避（`FLOW -> stand_va`）。 |
| `nr_d3d12_passthrough_backend.{hpp,cpp}` | `evaluate_backend()`、`copy_depth_r32()`、`backend_net_status()`、`abandon_at_exit()`。 |
| `nr_renodx_d3d12.{hpp,cpp}` | `renodx12::Stage`（含 `lin_mv` 按需延迟扩容、Tier 2 Heap Aliasing、多 Pass SDR `fast_lin`）。 |
| `nr_d3d12_direct_state.{hpp,cpp}` | `install_hooks()`、`ready()`、`Scope`、`book()` / `completed()`（已被 `renodx12::Stage` 和 `asi_wilsjo2` 内部引用）。 |
| `nr_asi_wilsjo2.{hpp,cpp}` | 基于预编译 `d3d12_outer.nra` 实现 `wilsjo2_run()`、`wilsjo2_convert()`、`hudfix_d3d12()`、`hudfix_select_d3d12()`、`wilsjo2_inspect()`。 |
| `nr_d3d11_d3d12_bridge.{hpp,cpp}` | `Bridge::decide()` / `execute()` 按 `Role`（`kBack`/`kHudless`/`kDepth`/`kMotion`）同步单次往返交接、D3D11 计算着色器深度转 `R32_FLOAT`（`DepthCopyD3D11`）、`kBack` `writable = true` 原地复用。 |
| `nr_asi_main.cpp`（参考模式） | `present_layers()` 中的私有命令分配器/命令列表环形池、单 `target` `read_back` 拷贝缩减与 `d11_in_place` 模式。 |

### 2.3 ReShade 接入相对 OptiScaler 的核心优势
- OptiScaler 的难题在于必须将计算插入**游戏自己的 `ID3D12GraphicsCommandList`**，因而需要处理复杂的命令列表状态劫持、Descriptor Heap 还原和第三方 Wrapper 兼容。
- ReShade 在 Windows 原生 D3D12 上本就是**队列级交接**（先 `flush_immediate_command_list()` 再提交）：插件可以使用**自己私有的 D3D12 命令分配器 / 命令列表**向 `queue` 提交全部多层计算与辅助 Pass，彻底不触碰 ReShade immediate list 的内部渲染状态与描述符堆。
- **需注意**：即便采用私有命令列表，`renodx12::Stage`、`nr_d3d12_passthrough_backend` 与 `nr_asi_wilsjo2` 内部仍依赖 `nr::pe::d3d12_direct::book(cmd)` / `completed(ticket)` 与 `ready()` 对命令提交完成度的追踪，因此在启用 D3D12 直通时仍需在设备初始化/首次使用阶段执行一次 `d3d12_direct::install_hooks(device)` 并在队列同步后维持生命周期票据推进（私有命令列表亦可用固定直接路径或经 hook 追踪）。

---

## 三、核心难点与关键耦合点拆解（5 大问题）

### 问题 1：ReShade 拦截 `D3D12CreateDevice` 导致的私有设备误识别与包装冲突
- **现状问题**：`nr_reshade_addon.cpp:2636-2640` 的 `own_device(device* dev)` 目前只过滤了 `device_api::vulkan` 下由 `nr::pe::bridge` 创建的私有 Vulkan 设备。一旦接入 `nr::pe::d11_bridge::Bridge`，`ensure_d12_locked()` 创建私有 `ID3D12Device` 与 `ID3D12CommandQueue` 时同样会穿过 ReShade 的 D3D12 钩子；若不屏蔽，`on_init_device` / `on_destroy_device` 与 `cap_*` 捕获钩子会误将私有 D3D12 设备识别为游戏设备，甚至干扰 `d3d12_direct::install_hooks()`。
- **解决对策**：
  - 在 `nr::pe::d11_bridge` 暴露 `creating_device()` / `is_own_device(ID3D12Device*)`（与 `nr::pe::bridge` 对齐）；
  - `nr_reshade_addon.cpp` 的 `own_device(dev)` 同时识别并放过 `device_api::d3d12` 的桥接私有设备；
  - 对私有 D3D12 设备与桥接自建命令列表做好 `d3d12_direct` 钩子兼容性验证，防止递归追踪或对桥接设备误装 `hook_d3dcompile()`。

### 问题 2：编译边界与 `nr::dlssnr` / `nr::pe::renodx_route` 符号耦合
- **现状问题**：
  - `windows/build/build_package.sh:77-105` 构建 `dlssnr_amd.addon64`（带 `-DNR_RESHADE_ROUTE=1`）时，当前**并未编译** `d3d12_passthrough*`、`renodx_d3d12`、`d3d11_d3d12_bridge`、`asi_wilsjo2`、`model.o`、`renodx_route.o`。
  - `nr_d3d12_passthrough_backend.cpp` 与 `nr_asi_wilsjo2.cpp` 当前存在多层跨模块反向调用：调用 `nr::dlssnr::with_d3d12_session` / `evaluate_d3d12` / `d3d12_no_vk_fallback` / `note_native_ms`，且强依赖 `nr::pe::renodx_route` 的线程局部作用域（`CallStyle` / `CallLayer` / `CallNetwork` / `CallWilsjo2`）。直接把 `nr_dlssnr_model.cpp` 整体拉进 `.addon64` 不仅体积膨胀，还会引入 NGX / Streamline / OptiScaler 逻辑以及重复配置读取风险。
- **方案对比与推荐**：
  - *方案 A（不推荐）*：全量编入 `nr_dlssnr_model.cpp` 并大面积 `#ifdef NR_RESHADE_ROUTE` 打补丁——维护负担重、易误触发 OptiScaler 状态。
  - *方案 B（推荐）*：抽离轻量共享胶水 / 接口层：
    - 将 `renodx_route` 纯线程上下文与直通状态结构从 OptiScaler hook 逻辑中独立（`nr_pe_renodx_route.cpp` 本身不含 OptiScaler 二进制 patch，patch 在 `nr_pe_optifix.cpp`，本身就可轻量复用）；
    - 为 `nr_asi_wilsjo2.cpp` 与 `nr_d3d12_passthrough_backend.cpp` 提供面向 ReShade 64 位侧的瘦后端驱动函数（或最小 `nr::dlssnr` 桩适配器，仅对接 `d3d12_passthrough` 而不包含 NGX/OptiScaler 分支），避免 ReShade 被迫链接进整个 `nr_dlssnr_model.cpp`。
- **编译单元边界清单（仅 x64 `build_package.sh`，32 位 `build32.sh` 不动）**：
  - 新增编入 `addon64` 的单元（或解耦重构后对应单元）：`nr_d3d12_direct_state.cpp`、`nr_d3d12_passthrough.cpp`、`nr_d3d12_passthrough_backend.cpp`、`nr_d3d12_passthrough_net.cpp`、`nr_renodx_d3d12.cpp`、`nr_d3d11_d3d12_bridge.cpp`、`nr_asi_wilsjo2.cpp`、`nr_pe_renodx_route.cpp`（及按方案 B 抽出的 D3D12 调度适配器）。

### 问题 3：零 Vulkan 启动（`session == nullptr`）下的 UI、OSD、VRAM 与生命周期合流
- **现状问题**：当前 `nr_reshade_addon.cpp` 假设只要启用插件就有全局 Vulkan `session`：
  - `fill_view()`（L3626-3668）：网络状态、不可用原因、构建阶段与百分比（`build_fraction()`）、`aco_state` / `aco_rdna3`、`gpu_ms` / `network_ms` 与显存信息（`session->video_memory()`）全部读自 `session`；
  - `on_reshade_overlay()`（L4218）：`if (session && config.network.osd != 0)` 直接以 `session != nullptr` 作为渲染 OSD 的前提；
  - `on_render_technique` / `feed()` 的 240 帧汇总日志与热切模型通知均读写 `session`。
- **解决对策**：
  - **惰性创建 `session`**：当 64 位 D3D11/D3D12 路径经判定可走 D3D12 直通时，延迟且彻底跳过创建 Vulkan `Session` 与调用 `load_vulkan()`（实现真正的零 Vulkan 运行时 / 零 Vulkan 显存开销）；仅在需要回退（32 位 / Vulkan / DX9 / D3D10 / 非 RDNA3-4 / `Enabled=0` / canary 失败）时才创建 `session`。
  - **双后端视图合流**：抽取 `fill_view_from_d3d12()`，读取 `d3d12_passthrough::status()`、`backend_net_status()`、`nr::gpu::stage_ms` 与当前后端记录的网络架构/构建进度，优先填充 `view`；无 D3D12 激活时走原 `session->state()`。
  - **VRAM 统计不依赖 Vulkan**：当 `session == nullptr` 时，直接通过与当前游戏 D3D12 / 桥接 D3D12 设备关联的 `IDXGIAdapter3::QueryVideoMemoryInfo` 查询进程 `CurrentUsage` / `Budget`，与 `nr::vram::totals()` 合流填入 `view.vram_*_kib`。
  - **退出防崩与 ABI 约束**：
    - `on_destroy_device()` 需按 API 分别清理 ReShade D3D12 私有命令列表、`PresentTarget`、桥接状态；
    - `DllMain(DLL_PROCESS_DETACH)` 与 `AddonUninit` 中若激活了 D3D12 直通，必须按 `nr_d3d12_passthrough_backend.hpp` 契约调用 `abandon_at_exit()`，防止 `DLL_PROCESS_DETACH` 阶段访问已卸载的 `amdxc64.dll`；
    - 面板新增任何 Diagnostics / `[D3D12Passthrough]` UI 控件时，必须严格遵守 `windows/build/check_imgui.sh`（不得在 mingw 代码中直接调用返回 `ImVec2`/`ImVec4` 的 MSVC ImGui 函数）。

### 问题 4：纹理格式、资源状态流与多 Pass / 辅助 Pass 调度对齐
- **D3D12 路径交换链特性适配**：
  - ReShade 交到 `on_finish_effects` / `feed` 的 `tgt.colour` 在大多数游戏中作为交换链或效果渲染目标传入，处于 `D3D12_RESOURCE_STATE_RENDER_TARGET` 状态，**常常没有 `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`，或为 `*_SRGB` 格式**；
  - 因此必须借鉴 `nr_asi_main.cpp::present_layers()` 的策略：维护带环形围栏（ring）的插件私有直接命令分配器与命令列表（提交到 `rt->get_command_queue()`），并按条件管理一张持久中间 `target`（具备 `UNORDERED_ACCESS | NON_PIXEL_SHADER_RESOURCE | COPY_SRC | COPY_DST`、非 sRGB 类型化格式）：
    - 仅当 `tgt.colour` 本身支持 UAV、格式匹配且无需保留原图（无 HUD FIX、Inspect、F5 截图）时原地直跑；
    - 否则采用单 `target` 优化路径：**首层直接以 `NON_PIXEL_SHADER_RESOURCE` 读取 `tgt.colour` 写入 `target`**，中间所有多层完全在 `target` 上串联，末层结束后仅做**一次 `CopyResource` 写回 `tgt.colour`** 并还原为 `RENDER_TARGET`。
- **D3D11→D3D12 路径单次往返收敛**：
  - 利用 `Bridge::execute()` 已具备的按 `Role` 打包能力，每次 Present 将 `kBack`（`writable = true`）、可选 `kHudless`（HUD FIX 捕获帧）、可选 `kMotion`、`kDepth`（`depth = true`，经内置 `DepthCopyD3D11` 处理 `D24S8`/`D32S8`/`R32_TYPELESS` 等 Generic Depth）一次性跨入 D3D12；
  - 结合 `0.0.9m8` 已落地的 `kBack` 原地计算（`d11_in_place`），多层推理及阶段 2 迁入的 HUD FIX / PQ / Inspect 全在同一次 `Bridge::execute()` 回调函数中的 D3D12 命令列表内执行完毕，避免单帧多次往返同步。
- **指导纹理与参数对齐**：
  - Generic Depth：D3D12 侧多种深度格式经过 `copy_depth_r32()` 转成 `R32_FLOAT`；
  - Motion Vectors（`vort_Motion.fx` / `Lumenite` / `DLSS5_Feed.fx`）：按全图宽高及 `sx`/`sy` 传给 `renodx12::Frame` / `Wilsjo2Frame`，并同步更新 `nr::pe::g_motion_without_depth_now`。

### 问题 5：冷启动竞态、Inspect/Hold 与第三方 `renodx-*.addon` 共存
- **冷启动不重拉 Vulkan**：网络异步编译阶段（`waiting == 1`、`canary == kNotRun/kRunning`、`Bridge::Route::kUndecided`）或临时构建缺失时，帧直接按 **passthrough 透传**，绝不可在这 1–2 秒窗口期回头创建 Vulkan `Session`，否则会同时驻留两套权重/激活堆（~1.5+ GB 峰值尖峰）。
- **与 `renodx-*.addon*` 共存**：保留 `renodx_mod_loaded()` 与 `present_white()` 读取（`nr_reshade_addon.cpp:542-581`）；使用队列级同步 + 私有命令列表并在提交后还原 ReShade 纹理状态，不篡改宿主效果链 SwapChain 与 RenderTarget 追踪表。
- **Inspect 滑条 / HoldFrame 状态**：Phase 1 未完成 Inspect D3D12 化前，若检测到 `inspect_fit` 需要触发且未迁移，明确门控策略（或延后到 Phase 2 一并开放全流程无缝支持），避免同一帧内跨两套后端争夺后备缓冲区。

---

## 四、分阶段实施计划（M1 / M2 / M3）

### Phase 1（M1）：64 位主推理路径打通（D3D12 直通 + D3D11→D3D12 桥接）
- **改造重点**：
  1. `nr_d3d11_d3d12_bridge.{hpp,cpp}`：新增 `creating_device()` / `is_own_device(ID3D12Device*)`，保护 ReShade 不对桥接私有设备安装捕获/控制钩子。
  2. 抽出面向 ReShade 的轻量 D3D12 后端驱动胶水（衔接 `nr_d3d12_passthrough*`、`nr_renodx_d3d12`、`nr_asi_wilsjo2` 与 `nr_pe_renodx_route`，避免整体拖入 `nr_dlssnr_model.cpp`）。
  3. `nr_reshade_addon.cpp`：
     - `DllMain` 调用 `d3d12_passthrough::start()`，并在退出阶段补全 `abandon_at_exit()`；
     - `own_device()` 增加 D3D12 桥接私有设备过滤；
     - `feed()` / `run_layers()` 内对 64 位 D3D12 与 D3D11 优先路由至 D3D12 直通队列环 / `d11_bridge`，仅当判定不适用（非 RDNA3/4、canary 失败、`Enabled=0`、原生 D3D10/DX9/Vulkan）才延迟初始化并回退 Vulkan `session`；
     - 在冷启动构建期保持透传而不拉起 Vulkan。
  4. `windows/build/build_package.sh`：为 `dlssnr_amd.addon64` 增量接入 D3D12 所需编译单元；保持 `x86/build32.sh` 不变，且检查 `.addon64` 导出/导入表仍无静态 `vulkan-1.dll` / `d3dcompiler_47.dll`。
- **M1 交付里程碑**：64 位 D3D12 与 D3D11 游戏在默认场景（RenoDX / wilsjo2 单层与多层、无 HUD FIX/Inspect/PQ）下实现**零 Vulkan 启动**推理，直接继承 `0.0.9m8` 显存/拷贝优化。

### Phase 2（M2）：三大外围辅助 Pass 迁移至 D3D12 Outer-ELF
- **改造重点**：
  1. **HUD FIX 迁移**：在 D3D12 / D3D11→D3D12 路径下，将 `hudfix_end()` 改由 `nr::asi::hudfix_d3d12()` + `hudfix_select_d3d12()` 执行（复用 `d3d12_outer.nra` 已内置的 `hudfix` 计算内核），D3D11 端通过 `Bridge::execute` 的 `Role::kHudless` 同轮交接 `H.hudless` 并在 D3D12 端合成回 `kBack`。
  2. **Inspect 对比与 Hold 迁移**：将 `inspect_fit_show()` 与对比拷贝切到 `nr::asi::wilsjo2_inspect()` 与队列内资源复制；HoldFrame 直接保留/复用 D3D12/桥接纹理。
  3. **HDR10 (PQ BT.2020) wilsjo2 转换迁移**：将 `pq_begin()` / `pq_end()` 改为调用 `nr::asi::wilsjo2_convert()`（`kFinishedToLinear` / `kFinishedToPq`），无需创建 Vulkan `Q.stage`。
  4. **F5 截图配对迁移**：D3D12 / 桥接帧前后对比截图复用 `nr_screenshot` 异步 Readback + D3D12 围栏完成票据，不再调用 `retire_effect_resource` 绑定的 Vulkan 路径。
- **M2 交付里程碑**：开启 HUD FIX、Inspect 对比或跑在 HDR10 PQ 游戏下时，全程不再唤醒任何 Vulkan 运行时。

### Phase 3（M3）：Overlay UI、OSD 与生命周期状态合流
- **改造重点**：
  1. **UI & OSD 合流**：打通 `fill_view()`、`on_reshade_overlay()` 与 240 帧汇总日志对 D3D12 后端实时状态、模型热切构建进度、GPU/网络耗时及 DXGI 进程显存占用的展示；所有新增 UI 必须经 `check_imgui.sh` 静态门禁。
  2. **Diagnostics 与开关打通**：在 ReShade Add-on 页面的网络/诊断页统一暴露 `[D3D12Passthrough] Enabled` 开关状态、Canary 结果（`kHit`/`kFail`）与当前运行路径标识（`D3D12 ACO` / `D3D11->D3D12 Bridge` / `Vulkan Bridge`）。
  3. **设备重置/销毁与多 SwapChain 健壮性**：覆盖游戏 Alt+Tab 全屏切分辨率、`DXGI_ERROR_DEVICE_REMOVED`、多个 EffectRuntime 共存与游戏退出卸载清理验证。
- **M3 交付里程碑**：ReShade 64 位侧用户体验、日志诊断与显存表现全面对齐 `dlssnr-amd.asi`。

---

## 五、验证门禁与实机测试矩阵

### 5.1 构建与静态检查门禁
| 检查项 | 通过标准 |
| :--- | :--- |
| `bash windows/build/check_paths.sh` | `OK`（UTF-8 路径规范无违规） |
| `bash windows/build/check_imgui.sh` | `OK`（无 mingw 跨 ABI 直接调用 MSVC `ImVec2`/`ImVec4` 返回值函数） |
| `python3 windows/build/check_outer_aco.py` | `ok (rdna4 + rdna3 83 kernels in sync with shader sources)`（外围 ACO 不受影响） |
| `dlssnr_amd.addon64` + `objdump -p` | 编链成功，PE 导入表不含静态 `vulkan-1.dll` 与 `d3dcompiler_47.dll` |
| `dlssnr_amd.addon32`（`x86/build32.sh`） | 32 位构链完全绿灯，未受 64 位 D3D12 改动波及 |
| `dlssnr-amd.asi` + `test_d3d12_wrappers.exe` + `test_d3d12_direct_r33.exe` | 已有 `asi`、wrapper 回归与 RDNA4/RDNA3 `r33` 用例无新增回归 |

### 5.2 实机与端到端回归测试矩阵（实施各阶段时执行）
| 维度 | 验证场景 | 预期要点 |
| :--- | :--- | :--- |
| **原生 64 位 D3D12** | SDR（R8G8B8A8 / R10G10B10A2 / sRGB）、HDR10 PQ、HDR scRGB (FP16) | 日志无 Vulkan loader 初始化与 `vkCreateDevice`；队列提交无 D3D12 Debug Layer / TDR 报错 |
| **原生 64 位 D3D11** | 非 sRGB 与 sRGB 后备缓冲 + ReShade Generic Depth + `vort_Motion.fx` | 命中 `d11->d3d12 bridge active`；冷启动零 Vulkan 回退；显存稳态与峰值提交对齐 `0.0.9m8` |
| **功能覆盖** | RenoDX 单/多 Pass（1–3 pass + Interleaved）、wilsjo2、HUD FIX、Inspect Split/Hold、热切模型、F5 截图 | 切换热更新进度条在 Overlay 正常显示；开启 HUD FIX / Inspect / PQ 不拉起 Vulkan |
| **共存性** | 同时加载 `renodx-*.addon64`（如常见 RenoDX 模组） | `ToneMapGameNits` 读取正常；不发生设备/队列状态踩踏 |
| **回退边界** | `[D3D12Passthrough] Enabled = 0`、32 位 D3D9/D3D11 (`addon32`)、DXVK、原生 D3D10、原生 Vulkan 游戏 | 平滑回退至原有 Vulkan 路线，功能与视觉无回退 |
