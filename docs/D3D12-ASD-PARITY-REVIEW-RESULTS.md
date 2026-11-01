# D3D12 ASD 后端与 Mochi 两条既有路线：对照审查结果

对应计划：`docs/D3D12-ASD-PARITY-REVIEW-PLAN.md`。本文件只记**有实质差异**的项目；等价段落合并说明。
证据目录：`artifacts/d3d12-parity-review/`。

## 结论摘要

- PBZ 首帧的 `DXGI_ERROR_INVALID_CALL`（`0x887A0001`）根因已离线定位并修复：**buffer SRV 的
  `Shader4ComponentMapping` 为 0**（无效映射）。该帧第一条把 buffer 当 SRV 读的 dispatch 是 `kBufToTex`，
  与设备丢失点完全一致。
- 修复后：单条 `kBufToTex`、完整 stage、以及**真实路径**（backend + RenoDX stage + 游戏导出的 ACO 计划，
  71 dispatches，1 pass）全部 `Close 0x00000000`、fence 正常、`GetDeviceRemovedReason() = S_OK`；
  连续 3 帧均如此。
- 输出可重复性已确认：修正宿主的 fence 水位后，同一输入两次进程运行的 1 帧与 3 帧哈希完全一致
  （`B46B428D44C7D765` / `421BFA5E857523E9` / `27B1B77BDFB3AA51`）。此前"首帧不同"是宿主
  `SetEventOnCompletion` 用了已被超越的旧值造成的读回竞争，与网络无关；那些旧哈希作废。

## 差异表

| 类别 | Linux/旧桥接来源 | native 对应 | 契约差异 | 状态 | 证据 |
|---|---|---|---|---|---|
| buffer SRV 描述符 | `nrvk.hpp` 的 descriptor 写入（Vulkan 无该字段） | `nr_renodx_d3d12.cpp` `dispatch()` / `fill()` 的 buffer SRV 分支 | `D3D12_SHADER_RESOURCE_VIEW_DESC` 全零构造，`Shader4ComponentMapping = 0`；纹理分支已设 `D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING`。D3D12 要求合法映射，否则读该 SRV 的 dispatch 在执行期被判非法调用 | **已修** `3de941e` | 二分：dispatch 1–4 健康、第 5 条（`kBufToTex`）移除设备；单独 `kBufToTex` 亦失败；修复后三档全部 S_OK。`P4-bisect-limit6-before-fix.txt`、`P4-stage-p30-after-fix.txt`、`P4-realpath-after-fix.txt` |
| Clear 的 GPU 句柄 | 无对应（Vulkan 用 clear 命令） | `nr_renodx_d3d12.cpp` 两处 `ClearUnorderedAccessView*` | GPU 句柄取自 `FLAG_NONE` 的 `clear_heap`；D3D12 要求 `ViewGPUHandleInCurrentHeap` 落在当前绑定的 shader-visible 堆 | **已修** `d0fdaa3` | 每处保留独立主堆槽位（`kClearSlot`、`+1`），Clear 前绑堆、状态先转 UAV、整流清零 `0, nullptr` |
| 同状态 transition | 无对应 | `transition()` / 收尾写回 | `output_state == UAV` 时就地路径会发出 `UAV → UAV`；D3D12 拒绝 `StateBefore == StateAfter` | **已修** `7cecebb` | `transition()` 统一跳过同状态；写回按 `output_is_uav` 判断 |
| 纹理从 COMMON 提升 | Vulkan 的 `UNDEFINED` 仅表示丢弃旧内容 | `ensure()` | 曾把 COMMON 当作"可隐式进入任何状态"；D3D12 可从 COMMON 提升到 SRV/COPY_SOURCE/COPY_DEST，**不能提升到 UAV**（非 simultaneous-access 纹理） | **已修** `824ffc6` | 首次以 UAV 使用前发出真实转换 |
| 外部资源状态登记 | 旧桥接 `lookup12`/`begin_recording` 的外部契约 | `record()` 入口 | 外部资源曾以"第一次见到即 COMMON"登记；就地路径 `colour == output` 且真实状态为 UAV，导致名不副实的 `COMMON → UAV` | **已修** `117e53d` | 每帧按调用者声明重新登记；`transition()` 与复制 helper 同步状态表 |
| buffer 状态与依赖 | 旧桥接经 Vulkan 隐式排序 | `record()` around `net` / `fill()` buffer 分支 | 线性缓冲曾完全绕过状态表；网络按下标地址写 `lin_neural`、读 `lin_proxy`/guides，缺少真实转换与 UAV 依赖 | **已修** `694de0f` | 每帧入口 `ensure(...UAV)` + `uav_barrier`；网络返回后用 `ensure(SRV)` 而非赋值 |
| 渲染目标 UAV 能力 | 旧桥接 `lookup12` 的格式/尺寸限制 | `nr_d3d12_asd_backend.cpp` 入口校验 | 复用调用者纹理时需确认 `ALLOW_UNORDERED_ACCESS` 与实际用途一致 | 待执行（P2） | — |
| 输出可重复性（宿主） | — | `windows/tests/d3d12_asd/nr_d3d12_asd_test.cpp` 的提交/等待 | 上传段 `Signal(100)` 与帧循环 `Signal(1)` 混用，`SetEventOnCompletion(1)` 立即满足；读回与 `Reset` 与在飞的 GPU 工作竞争 | **已修** `74db8b9` | 统一单调计数 + 每次核对 `GetCompletedValue() >= 期望`；1 帧与 3 帧两次运行哈希一致。`P4-determinism.md` |

## 离线宿主（P4/P5 用）

`windows/tests/d3d12_asd/nr_d3d12_asd_test.cpp`，构建 `windows/build/build_d3d12_asd_test.sh`。模式：

- `stage [W H] [game folder]`：只驱动 RenoDX D3D12 stage（无 ACO），可用调试层与 DRED。
  调试环境变量：`NR_STAGE_DISPATCH_LIMIT=<n>`（只录前 n 条）、`NR_STAGE_ONLY=<program>`（只录指定程序）。
- `renodx [W H] [game folder] [plan.bin] [frames]`：完整生产入口（backend + stage + 真实 ACO 计划）。
  ACO 需要正常注册或 `NR_TEST_FORCE_CANARY=1`（测试专用，产品不调用）。
- `NR_CAPTURE_GPU_VALIDATION=1` 启用 GPU 验证；`NR_CAPTURE_LAYER=0` / `NR_CAPTURE_DRED=0` 关闭对应采集。

运行方式：批处理避免引号被 `cmd` 吞掉，例如 `artifacts/windows/asd-harness/run-real7.bat`。

## 未运行 / 待办

- 分离输出（`colour != output`）：已运行，2 帧 `S_OK`（`P4-separate-output.txt`）。
- guides（motion + depth）：已运行，历史门 0→1→1，3 帧 `S_OK`（`P4-guides-history-3frames.txt`）。
- 非 UAV 入口状态：未运行。
- resize / 多 pass / 特征回收边界的定向案例：未运行。
- 输出与旧桥接数值对齐：未运行（桥接对照宿主待搭）。
- **ACO 真实执行已单独证实（诊断装载）**：关闭注册、不用 `force_canary`，仅用 Microsoft 文档的本地 D3DConfig
  PSDB override 指向离线测试 exe，canary **ASD_HIT**（0/128 与 ACO 一致），71 dispatches，3 帧 `S_OK`，
  退出后 override 已恢复原状。证据 `P5-real-aco-execution.md`。这与产品程序化注册（下表）分开。
- ACO 输出与驱动编译的 HLSL 载体输出**不一致**（逐像素：B 通道 mean 44–72/1023、max 246–338），
  这是两份不同的机器码（ACO ELF over Linux SPIR-V vs 驱动 DXIL），不是可比的数值等价；不用载体时间当 ACO 性能。
- 正常注册路径：**本机此刻对所有 exe 都不可用**——实验室旧 `asd_probe.exe` 用它自己的路径（历史上成功过）
  现在同样返回 `E_HANDLE`；换成 1.619.5 Agility Core、清空条目、换目录、换目标 exe 均无差别。
  因此 `E_HANDLE` 不是宿主 exe 的属性，而是这台机器上 D3DSCR 存储的瞬时状态（实验室 `docs/ASD-PUBLIC-IMPORT.md`
  §E_HANDLE 记过同样现象）。证据：`P5-registration.md`、`P5-registration-agility619.txt`。
- 由此：**ACO 真实执行未验证**。`force_canary` 只改门控，不让驱动执行 ACO 二进制；71 次 dispatch 的 `S_OK`
  与全部数值/历史/分离输出结论只证明**生产录制路径**（命令、资源、表、push、内存序）正确，不证明机器码结果。
  机器码结果需要注册可用后重跑同一案例（计划：`PASS_ASD_ACO` 等价判据）。

## 结论强度（读本文时先看）

| 已证明 | 未证明 |
|---|---|
| 生产 D3D12 录制路径在真实设备上提交执行（`S_OK`，3 帧，ACO/HLSL 两条产出都存在） | 驱动实际执行 ACO 机器码（注册 `E_HANDLE`，canary `ASD_MISS`） |
| 资源/状态/表/push/内存序与计划一致 | 与旧桥接的数值一致（桥接对照宿主未搭） |
| 完成信号与退休生命周期（等 GPU 水位） | 多 pass / resize / 特征回收边界 |

## P2 资源状态/描述符（逐条核对结果）

| 项 | 结果 | 依据 |
|---|---|---|
| 入表规则：只有 `make_tex`/`make_buffer` 以 COMMON 入表 | 符合 | `make_tex()` 同时插表；外部资源每帧重登记 |
| 外部资源首帧按调用者状态 | 符合 | `f.colour`/`f.output` 用调用者声明的状态登记 |
| 同状态不转换 | 符合 | `transition()` 比较即返回 |
| 手写 barrier 与状态表一致 | 符合 | `copy_frame()`、`ensure()` 每个改动都写表 |
| 每条路径都恢复调用者状态 | 符合 | record 尾部恢复并 `restore()` 调用者的 split 状态 |
| 每帧重新登记指针与状态 | 符合 | `f` 每帧 `ensure()` 后登记 |
| 复用前清理/改写描述符 | 符合 | 每个 dispatch 都重建 SRV/UAV 描述符 |
| 堆/表/根签名在网络上运行后有效 | 符合 | 网络只设自己的根签名（`nr_d3d12_asd_net.cpp` 无 `SetDescriptorHeaps`），stage 每个 dispatch 重新设 RS+PSO+两张表 |
| 失败返回导致半帧已录制 | 已记录，影响仅内部资源 | 5 处 `return 0`（`prep_keep`/`prep_state`/`motion`/`exposure_1x1` 等）都在写回帧之前；调用者按“未运行”原样放过 |

## P3 完成信号/退休（逐条核对结果）

| 项 | 结果 | 依据 |
|---|---|---|
| 网络退休：copy 与 exec 两段水位 | 已修（前次提交） | `refresh_copied`/`refresh_executed`，torn read 过滤 |
| 后端退休：stage/资源 | 本轮修复 | `mark_frame` + `refresh_executed` + `token_slots` 增长；证据 `P3-completion-watermark-3frames.txt`（3 帧 `S_OK`） |
| 后端水位与 device 代际 | 本轮修复 | `drop_device()` 释放 token/done/两张退休表并清水位 |
| stage 内部纹理 | 随 stage 一起退休 | stage 由后端水位决定释放时刻 |
| 后端 broken 路径 | 不走 ASD，重试有界 | `broken` + `broken_why`，`evaluate` 早退 |

## P1 资源契约（当前帧实际用到的资源）

| 资源 | owner | 进入状态 | 本帧用途 | 所需状态/依赖 | 退出状态 | 状态表更新者 |
|---|---|---|---|---|---|---|
| `f.colour` | 调用者 | 调用者声明的 `colour_state`（就地时 UAV） | 复制进 original；`kCopyOut` 时写入（就地） | COPY_SOURCE / UAV；复制前后各一次转换 | 恢复为调用者状态 | `record()` 每帧登记；`transition()`/`copy_frame()` 写入 |
| `f.output` | 调用者 | 调用者声明的 `output_state` | UAV 写回 | UAV（非 simultaneous-access 纹理不可从 COMMON 提升） | 恢复为调用者状态 | 同上（就地时与 colour 同一份） |
| `f.motion` / `f.depth` | 调用者 | NON_PIXEL_SHADER_RESOURCE | 复制子矩形 / 作为 SRV | COPY_SOURCE → 复制 → SRV；子矩形夹紧到来源范围 | 恢复调用者状态 | `ensure()`/`transition()` |
| `original`、`work*`、`proxy`、`neural`、`decoded*`、`block_mean`、`scale`、`commit` | stage（`make_tex`） | COMMON（创建态） | UAV 写 / SRV 读 | 首次按用途转换；之后按表转换 | 内部保留 | `ensure()`（唯一入口） |
| `prep_state`、`lin_proxy`、`lin_neural`、`lin_mv`、`lin_depth` | stage / net | COMMON（`make_buffer`） | 网络按 raw address 读写；stage 也读 | 每帧入口 `ensure(UAV)` + UAV barrier；网络返回后 `ensure(SRV)` | 内部保留 | `ensure()` + 显式登记 |
| `f.exposure` | 调用者 | 调用者状态 | SRV（若有） | SRV | 不变 | 每帧登记 |

规则：只有 `make_tex`/`make_buffer` 造出来的资源以 COMMON 入表；外部资源每帧按调用者声明重新登记；
所有转换（`transition()`、`ensure()`、`copy_frame()` 的手写 barrier）都写同一张表；同状态不转换。

## 参考宿主（Vulkan,0.0.8.1 路线）与 D3D12 路线的对照

`windows/tests/d3d12_asd/nr_parity_host.cpp`（`windows/build/build_parity_host.sh`）在**生产设备创建**
（`nrvk::Context::create`，与 D3D12 设备 LUID 对应的适配器，`buffer_device_address` 取自设备本身）上构建
`nr::Runtime`（temporal 开启，root = 组装的安装树，`configure_network` 打开 ACO Mode），并用参考
`nr::renodx::Stage` 跑同一帧（同 HDR10/present 2/203 nits、同 guide 字节、同输入图案）：

| 对比（R10G10B10A2，通道绝对值，3840x2160） | R max/mean | G max/mean | B max/mean |
|---|---|---|---|
| 参考(Vulkan, ACO Mode) vs D3D12-ASD(ACO) | 32 / 1.14 | 34 / 1.05 | 41 / 2.47 |
| 参考(Vulkan, ACO Mode) vs D3D12-ASD(无 PSDB：载体) | 222 / 16.06 | 144 / 11.63 | 247 / 43.99 |

读数：**ACO + D3D12 路线与参考路线接近**，而"无 PSDB"的路线差得多。后者的读数**不是编译器差异**：那一路
跑的是 ASD 的 carrier，按设计不写任何输出（实验室 `docs/ACO-D3D12-ASD-PLAN.md` §2"载体本身不写任何输出"），
所以它只说明"没有网络"时输出是什么样子。残余差异（ACO vs ACO）归因未做。对照仅用于**正确性**：参考宿主是纯 Vulkan，**不是**
D3D12→Vulkan 共享资源/fence 的 bridge，不能拿它的时间当桥接或 ACO 的性能。

## 真实桥接 A/B（D3D12 ↔ Vulkan 共享纹理/fence，完整 stage）

`windows/tests/d3d12_asd/nr_bridge_host.cpp`：D3D12 设备 → `bridge::Device::get(LUID)` → `create_d3d12` 的
colour/output/motion/depth 共享图像 → 参考 `nr::renodx::Stage` 在共享 job 上跑完整流程（预处理/编码/网络/resolve）
→ `run_between` → D3D12 队列回读。ACO 在桥接设备上生效（`ACO network: pipeline binaries ... (LLPC)`）。

| 对比（R10G10B10A2，通道绝对值） | R max/mean | G max/mean | B max/mean |
|---|---|---|---|
| 桥接输出 vs 纯 Vulkan 参考宿主 | 0 / 0.000 | 0 / 0.000 | 0 / 0.000 |
| 桥接输出 vs D3D12-ASD(ACO) | 30 / 1.12 | 36 / 1.00 | 38 / 2.30 |
| 桥接输出 vs D3D12-ASD（无 PSDB：载体，不写输出） | 221 / 16.24 | 144 / 11.85 | 242 / 43.97 |

读数：桥接交接**逐位不改变**参考路线的结果（与参考宿主完全一致），说明共享纹理/fence 与 job 顺序正确；
桥接(ACO) 与 D3D12-ASD(ACO) 的差 mean 1.0–2.3、max 30–38/1023（≈3.7% 满量程），**不能**笼统称为"一致"。**这不是性能测量**：
还没有多帧计时、没有同 HDR preprocess/resolve 的 A/B 计时，也没有帧时间数据；`P4-bridge-host-run.txt` 为完整日志。

## 首个差异定位（网络入口/出口，同帧同配置）

抓取：`NR_TEST_STAGE_DUMP`（ASD 测试，阶段纹理 + 原始线性缓冲）与 `NR_STAGE_DUMP`（参考/桥接宿主），两侧都是
RGBA16F、NW x NH。严格比较脚本 `windows/tests/d3d12_asd/compare_dumps.py`（两份文件必须精确等于 W*H*4 或
W*H*8 字节，不截断；逐通道 max/mean/RMSE/p50/p90/p99/不同像素，float 检查非有限值）。

| 边界 | 结果 |
|---|---|
| 网络入口 proxy（encode 之后） | **逐位相同**（0 在所有通道，0.00% 像素不同） |
| 网络出口 neural | 差 mean 0.0045–0.0064、max 0.064–0.109、p99 0.022–0.025（单位：纹理值 0..1）；alpha 完全相同 |

结论：**差异起点在网络内部**（proxy 完全一致，说明 encode/guide/输入链一致）；把网络输出喂给编解码后才放大为
最终帧的 30–38/1023。

已排除的配置差：把参考侧 `Controls` 对齐到 ASD 的配置（`noise_every_frame=true`、`automatic_mask=false`）后，
proxy 仍逐位一致、neural 的差几乎不变（mean 0.0054–0.0072、max 0.074–0.102、p99 0.026–0.029）。两条路线的
网络规模也一致（71 dispatches、23 kernels、working extent 3840x2176、同一 small model）。

下一步已跑：两侧都记录**第二帧**（有历史）。结果：

| 帧 | proxy | neural 差（mean / max / p99） |
|---|---|---|
| 0（无历史） | 逐位相同 | 0.0054–0.0072 / 0.074–0.102 / 0.026–0.029 |
| 1（有历史） | 差 max 0.44（proxy 本身来自上一帧的答案） | 0.027–0.074 / 0.42–0.51 / 0.11–0.19 |

所以首帧差不是噪声场初始化：**第一帧网络答案就已不同**（proxy 相同、控件相同、网络规模相同），随后在 temporal
块里被放大。剩余候选：网络内部各段的实现差（pre/attn/post 的调度与精度）、以及 ASD 首帧多出的 1 个 noise job。
下一实验：抓 pre/attn 之后各一段（同 proxy 输入），确定差异出现在哪一段；不要在没有该证据时归因到精度。

抓取不影响输出：开关两次的 frame0/1/2 哈希完全相同（`8DCA…/9E91…/5ED5…`）。
`hand_over_mark()` 必须在 `run_between` 之前取（已在头注释更正）。

## 校正：ASD 路径上"无 PSDB"不是第二条网络实现

ASD 的 PSDB 记录缺失时，驱动执行的是 **carrier**（读全部 root constants、**不写任何输出**，实验室
`docs/ACO-D3D12-ASD-PLAN.md` §2）。用 `NR_TEST_FORCE_CANARY=1`（无 PSDB override）抓到的 `neural` 是
RGB=0、alpha=1，正好是这个行为，而不是"DXIL 编译的网络"。因此：

- 之前的"ACO vs DXIL/HLSL 差一个数量级"**不是编译器或精度差异**，只是"有网络 vs 无网络"；
- ASD 路线上网络只有 ACO 一种；与 Vulkan 参考的对比正是 ACO vs ACO；
- 本文件里所有"DXIL"字样已改为"载体/无 PSDB"。

## 噪声与 guide 的实际输入（逐项核实，不靠假设）

- **噪声规则**：把 ASD 侧 `noise_every_frame` 设为 0、参考侧 `Controls::noise_every_frame=false` 后重跑，
  ASD 的帧哈希不变（`8DCA…/9E91…/5ED5…`），与参考的网络答案差也不变（mean 0.0072、max 0.1016）。
  第 0 帧的 seed 本来就是 0（两条规则的差别只在后续帧），所以**帧 0 的差与 seed 无关**；
  ASD 的 `+1 noise` 是首帧的场生成（不是每帧 seed），它的**内容**是否与参考构建时预计算的场一致**尚未验证**。
- **guide**：两侧的 motion/depth 都是**全零**、同 extent（ASD 侧 `NR_TEST_ZERO_GUIDES=1`；参考宿主填零）。
  但 ASD 阶段会把 guide 重采样到 NW×NH（`to_motion`/`to_depth`），参考的运行时在自己的 pre 块里采样，
  两者的**重采样结果未比对**——已加 `Stage::Frame::capture_mv/capture_depth` 钩子（D3D12 侧可抓），
  参考侧需要等价的内部钩子，这是下一步。
- 因此"proxy 一致"不等于"guide 一致"；在当前全零 guide 下，剩余差异不能归因于 guide 输入，只能继续往网络内部分段。

## 调度口径与真实映射

- 两条路线**同一份 lowered plan**：ASD 侧日志 `71 dispatches, 23 kernels + temporal pre/post`；参考侧构建
  `25 pipelines` = 23 kernels + 2 temporal，"25"是流水线数、"71"是每帧 dispatch 数，不是两套不同的网络。
- ASD 侧的逐 dispatch 映射（内核名、grid、tag、table base）已在
  `artifacts/d3d12-parity-review/P4-asd-dispatch-map.txt`（3 帧 214 行，约 71/帧）：
  `vitattn / gemmvqkvnorm / gemmvact / gemmprojw / gemmprojc / gemmproj / ffwd3w / attn / noisefield / temporal`。
- 参考侧 runtime 的 plan 没有内核名字段，逐 dispatch 名字无法直接对齐；下一步要在两个引擎里各加一个
  **按索引 dump 该 dispatch 的 UAV** 的钩子（同一索引、同一帧），按"首个不一致"停止，而不是盲扫 71 层。

## 语义边界抓取的落点（已定位，尚未产出 dump）

四处边界在两侧都按**名字/语义**可取，不需要全局计数：

| 边界 | D3D12 侧 | 参考侧 |
|---|---|---|
| noise 场 | 调度日志 `kern noisefield`（`nr-diag/run.txt` 第 0 条） | 计划里的 `t.noise_field_at`（`nr_runtime.cpp:3055`） |
| temporal_pre 输出 | `kern temporal`（第 1 条，pre） | `t.pre` 索引与 `fswinimagepreds32nh`（`nr_runtime.cpp:3041`） |
| 首个 fswin 输出 | `kern fswin32nh` / `fswindsp32nh`（第 3、4 条） | 同名 `Disp::kern`（`nr_graph.cpp:2523`） |
| temporal_post 前 surf1 | `kern temporal`（post） | `t.post` 与 `fswinimagepost32`（`nr_runtime.cpp:3042`） |

两条录制入口：ASD `nr_d3d12_asd_net.cpp` 的 `for (i = lo; i < hi; ++i)` 循环（已有 `kern` 名字）；
参考 `Runtime::record_all`（`record_temporal` 只是转发）。四处 dump 需要在两侧各加"名字命中→copy 该 dispatch
的输出区"的代码，并知道每个 dispatch 的输出 offset/字节数（计划 push 里的 `o_off`/`x_off`）。

本轮**没有**产出这四处 dump，也没有据此的修复；不把已定位当已完成。

## 首个不一致边界（实测，逐字节）

| 边界 | ASD vs 参考 | 证据 |
|---|---|---|
| 噪声场 `wgt[off*4, +3840*2176*8)` | **逐字节相同**（66,846,720 B，sha256 `07e23073c3690247…`） | `netdump/asd_noise.bin` / `vulkan_noise.bin` |
| `act[0, 64 MiB)`（pre 之后） | 两侧相同但**全为 0**——pre 不写这里，此行不构成"pre 一致"的证据 | `asd_pre.bin` / `vulkan_pre.bin` |
| `act[534,773,760, +64 MiB)`（pre 的 push 第 2 个字面值） | 仍**全为 0**，pre 的真实输出区**尚未定位** | 同上重抓 |
| 权重块 `wgt[0, weights_end)` | **逐字节相同**（115,736,784 B，sha256 `da0f18be1ae5fcbb…`） | `asd_wgt.bin` / `vulkan_wgt.bin` |
| 首个 `fswin32nh` 之后 `act[0, 64 MiB)` | **不同**：49,371,484 / 67,108,864 B 不同（73.57%），首字节即不同 | `asd_kern.bin` / `ref/vulkan_kern.bin` |

两条路线的该 dispatch：同一索引（1）、同 grid `240x136x1`、**同 push 84 字节**（`00 00 d0 2f 00 00 00 00 00 92 …`）。

注意：pre 的落点未定（两处候选区都为零），所以"输入相同"目前只对**权重/噪声/push/grid**成立；
首个 fswin 之后 `act[0, +64 MiB)` 非零且两侧 73.57% 字节不同（`asd_kern.bin` vs `vulkan_kern.bin`，
sha256 `cd173835…` / `edcbed17…`）。fswin 的 push 第二个字为 0（输出在 act 偏移 0）与其输入偏移 800,063,488 待核。

因此——剩余变量是各自使用的
ACO 机器码（ASD 的 `public-copy.psdb` vs 参考安装树的 `aco/records`）。下一步比较两者为 `fswin32nh` 装载的
ELF（记录文件/PSDB 内容），而不是再比下游。

## pre 的落点：act 里没有（实测扫描）

对 `act` 做 64 MiB 窗口扫描（pre dispatch 之后立即抓取）：

| 区域 | ASD | 参考 |
|---|---|---|
| 0–512 MiB | 全零 | 非零窗口在 448 MiB |
| 512–1024 MiB | 全零 | 非零窗口在 960 MiB |
| 1024–1408 MiB | 全零 | 未抓 |
| surf1[0,64 MiB) | 全零 | n/a |

结论：**temporal pre 在 ASD 路线上没有向 act 写任何东西**（至少不在其 1.5 GB 的已扫描区间），而参考路线在 act
的 448/960 MiB 有非零写入。结合参考侧 pre 的绑定表（`nr_runtime.cpp:1452`，pre 读 `act, act, wgt, wgt, wgt, act`），
pre 的一部分输出本来就在 **wgt**，下一步应抓 pre 之后的 wgt 区域而不是继续扫 act。

### 修正索引/偏移后的重建结果（两侧都修好）

- D3D12：表索引 `i` 与 plan 索引 `tp.pre` 混淆，pre 抓取实际发生在 noise 之后、pre 之前 → 旧"全零"作废。
- 参考：pre 抓取读了 `roff` 却把 `bb.offset`/`bc.srcOffset` 写成 0 → 旧偏移扫描结论作废。
- 修正后同偏移 `act[534,773,760, +64 MiB)`、pre 之后：两侧都非零，但**内容不同**：
  ASD sha256 `136de105…`、参考 `dfc867f8…`，49,602,675 / 67,108,864 字节不同（73.91%），f16 上 max|d| = 1.509。
- 因此**首个不一致点在 temporal pre 的输出**（或它读取的输入）。已逐字节一致：noise、weights。

## pre 的内核身份（第 1 项证据）

记录文件按 `fnv64(SPIR-V)` 命名（`nr_pipeline_binary.hpp`，`pal::hash`）：

| 来源 | SPIR-V sha256 前缀 | fnv64（= 记录名） | 记录是否存在 |
|---|---|---|---|
| `parity-root/dlssnr-amd/aco/shaders/temporal/temporal_pre_fp32nh.spv`（参考侧 ACO 用） | `63396fd0…` | `3ccbc35ad9ececb3` | **存在**（`windows/data/aco/records/3ccbc35ad9ececb3.nrp`） |
| `parity-root/dlssnr-amd/shaders/temporal/temporal_pre_fp32nh.spv`（原生/Windows 网络） | `8c2c57bc…` | `58161c1ca21f22e5` | **不存在** |

即：ACO 包里的 pre 与 Windows 原生网络里的 pre 是**两份不同的 SPIR-V**；只有前者有 ACO 记录。ASD 侧的
PSDB 由实验室流程生成，尚未从运行时侧证明它装载的是哪一个 ELF（第 1 项证据只完成了一半：记录身份已明确，
两侧实际装载的 ELF 还需运行时取证）。
