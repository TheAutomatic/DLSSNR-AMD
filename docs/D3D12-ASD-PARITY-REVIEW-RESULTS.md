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
