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
| 参考(Vulkan, ACO Mode) vs D3D12-ASD(驱动 DXIL) | 222 / 16.06 | 144 / 11.63 | 247 / 43.99 |

读数：**ACO + D3D12 路线与参考路线的误差比 DXIL 路线低一个数量级**，说明 stage 的 host 逻辑与网络接线一致；
残余 ~1/1023 归因未做（需要逐层对照）。对照仅用于**正确性**：参考宿主是纯 Vulkan，**不是**
D3D12→Vulkan 共享资源/fence 的 bridge，不能拿它的时间当桥接或 ACO 的性能。

## 真实桥接 A/B（D3D12 ↔ Vulkan 共享纹理/fence，完整 stage）

`windows/tests/d3d12_asd/nr_bridge_host.cpp`：D3D12 设备 → `bridge::Device::get(LUID)` → `create_d3d12` 的
colour/output/motion/depth 共享图像 → 参考 `nr::renodx::Stage` 在共享 job 上跑完整流程（预处理/编码/网络/resolve）
→ `run_between` → D3D12 队列回读。ACO 在桥接设备上生效（`ACO network: pipeline binaries ... (LLPC)`）。

| 对比（R10G10B10A2，通道绝对值） | R max/mean | G max/mean | B max/mean |
|---|---|---|---|
| 桥接输出 vs 纯 Vulkan 参考宿主 | 0 / 0.000 | 0 / 0.000 | 0 / 0.000 |
| 桥接输出 vs D3D12-ASD(ACO) | 30 / 1.12 | 36 / 1.00 | 38 / 2.30 |
| 桥接输出 vs D3D12-ASD(驱动 DXIL) | 221 / 16.24 | 144 / 11.85 | 242 / 43.97 |

读数：桥接交接**逐位不改变**参考路线的结果（与参考宿主完全一致），说明共享纹理/fence 与 job 顺序正确；
桥接(ACO) 与 D3D12-ASD(ACO) 的差仍在 ~1/1023 量级，DXIL 路线差一个数量级。**这不是性能测量**：
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
最终帧的 30–38/1023。下一步是匹配网络侧的配置（`noise_every_frame`、控件、temporal 参数），再决定这是配置差
还是 ACO/DXIL 之外的实现差。

抓取不影响输出：开关两次的 frame0/1/2 哈希完全相同（`8DCA…/9E91…/5ED5…`）。
`hand_over_mark()` 必须在 `run_between` 之前取（已在头注释更正）。
