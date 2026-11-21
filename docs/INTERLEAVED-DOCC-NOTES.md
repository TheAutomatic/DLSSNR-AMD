# Interleaved（交错模式）默认关闭深度去遮挡（DOCC / bit `512`）修改说明与复审依据

本文档记录在 `v0.0.9m3`（`009-dev`）中将 Interleaved 模式的默认深度策略从 `1`（DILATE + DOCC）调整为 `3`（仅保留 DILATE，关闭 DOCC）的原因、改动位置、权衡与回退方式，以及对 `b24` 外部补丁中其余改动未合入的审阅结论，便于后续复审。

---

## 一、背景与适用范围

- **问题来源**：外部基于上游 `v0.0.9`（`9503f6b`）提供的 `b24` 修复报告显示，在 RR（OptiScaler Ray Reconstruction）+ 高倍上采样 + Interleaved 同开时（例如《影之刃零》1281×721 → 1920×1080，RX 9070 XT），画面会出现规则的浅色方格阵列；单变量排查确认只与 Interleaved 的深度去遮挡判据（DOCC，`flags & 512u`）有关。
- **与 `v0.0.9m3` 的差异**：
  1. `b24` 同时修的另一个 D3D12 直通“叠层失效”问题（`pass_net` 未读槽位输入 `il_base` / `il_base2`），本分支已在 commit `d11fef8` 提前彻底修复（见 `windows/src/pe/nr_renodx_d3d12.cpp` 中 `268435456u`、`536870912u`、`1073741824u` 与 `slot_in` 传参），且本分支自 `73297d8` 起 D3D12 直通直接运行预编译的 `il.comp` / `il0.comp` ACO outer kernel（`d3d12_outer.nra`），不存在 `b24` 所担心的 HLSL 分叉。
  2. `b24` 只改了 RenoDX Vulkan（`nr_renodx.cpp`）和 RenoDX D3D12（`nr_renodx_d3d12.cpp`），**漏掉了 `wilsjo2` 路径**（`nr_wilsjo2_flow.hpp`）。本次将 **3 条 Interleaved 路径（Windows + Linux 共 5 个源码文件）全部统一**。

---

## 二、根因分析（对照 `windows/shaders/renodx-fast/il.comp` 与 `il0.comp`）

在 `il.comp` / `il0.comp` 中，上帧搬移过来的旧槽位增量是否可信，由 `STEP == 0` 计算的 `trust = min(t_v, t_b, t_d)` 决定（`VTRUST` bit `8` + `BOX` bit `128` + `DOCC` bit `512`）。其中 `DOCC (512u)` 在上采样 + 抖动链路下会成块误判：

1. **低分辨率深度最近邻写入制造阶梯**：`STEP == 5` 通过 `near_at(mt)` 以最近邻方式将渲染分辨率的深度写入工作分辨率的 `DepthCur`，天然带有上采样倍率（如 1.5×）的网格阶梯边界。
2. **2×2 最大值 + 反向 Z 相对比值在亚像素抖动下放大误报**：`STEP == 0` 在 `DepthPrev` 的搬移源位置先取 2×2 最大值 `np`（进一步加粗块状边界），再计算：
   $$r = \frac{n_p - n_c}{\max(n_c, 10^{-7})}$$
   在反向 Z 下，天空、远景或斜视墙面/地面的 `nc` 极小；RR/TAA 每帧存在亚像素相机抖动，两帧间的微小深度偏移除以极小的 `nc` 后极易超过 `smoothstep(0.05, 0.2, r)` 阈值，成块将 `t_d` 乃至 `trust` 压低至 0。
3. **金字塔补洞 + 上采样锐化形成方格阵列**：`trust` 跌低后触发 `STEP == 4`（`FILL`，bit `64`）的 push-pull 金字塔模糊填充，在平滑区域产生网格状色差块，最终被上采样器放大为规则的浅色方格阵列。

`b24` 实测尝试了 8 种改写 DOCC 的方式（双线性采样、斜率自适应容差、绝对步长、原地比对等）仍全部出现方格，证明在此链路下不宜默认开启 `512u`。

---

## 三、本次修改清单（纯主机端改动，不改 shader / `.nra`）

3 条路径统一通过环境变量 `NR_IL_DEPTH` 控制深度模式（`0` 关闭深度、`1` DILATE `256` + DOCC `512`、`2` 仅 DOCC `512`、`3` 仅 DILATE `256`），**默认值统一改为 `3`**（若在 `nr_renodx.cpp` 中显式设置 `NR_IL_ANTIGHOST=0`，默认仍随 `il_ag` 落回 `0`）：

| 路径 | 修改文件 | 改动内容 |
|---|---|---|
| D3D12 直通（RenoDX） | `windows/src/pe/nr_renodx_d3d12.cpp` | 新增 `#include <cstdlib>` 与 `NR_IL_DEPTH` 解析（默认 `3u`）；`il_depth_mode == 0` 时置空 `depth`；`ip.flags` 将硬编码 `768u` 拆分为 `256u`/`512u` 条件位；首次读取深度日志追加 `, depth mode %u` |
| Vulkan 桥接 / 原生（RenoDX） | `windows/src/core/nr_renodx.cpp`、`linux/src/core/nr_renodx.cpp` | `il_depth_mode` 默认值由 `1u` 改为 `3u`；首次读取深度日志追加 `, depth mode %u` |
| wilsjo2 样式流（D3D12 + Vulkan） | `windows/src/core/nr_wilsjo2_flow.hpp`、`linux/src/core/nr_wilsjo2_flow.hpp` | `il_run1()` 新增 `NR_IL_DEPTH` 解析（默认 `3u`）；`depth` 开关受 `il_depth_mode != 0u` 控制；`ip.flags` 将硬编码 `768u` 拆分为 `256u`/`512u` 条件位 |

> 注：未改动任何 `.comp` 着色器源码或 push constant（保持 96 字节），因此无需重编 RDNA4 / RDNA3 的 `d3d12_outer.nra`。

---

## 四、保留功能、权衡与回退方式

### 仍然完整保留的深度与防鬼影机制
1. **`DILATE`（bit `256`）**：3×3 邻域取最近深度的运动向量（向量膨胀），前景物体边缘仍跟随前景向量移动，不被背景拖坏。
2. **`DFILL`（bits `67108864` / `1073741824`）**：`STEP == 4` 的联合双边深度感知补洞仍读取 `DepthCur` / `DP1..6`，`VTRUST` 或 `BOX` 触发补洞时仍优先取同深度周围像素。
3. **Preset 2 `stable_reduce`（`STEP == 6`）**：其 8×8 块内 16 点显露检测直接读 `DepthCur` / `DepthPrev`，不依赖 `512u`，不受影响。
4. **`VTRUST`（bit `8`）+ `BOX`（bit `128`）+ `EDGE`（bit `33554432`）**：运动向量突变检测、FSR2 3×3 颜色盒检测与画面边缘填充全部保留。
5. 核对 `il.comp` 第 632 行唯一另一处 `(pc.flags & 512u) != 0u && raw_depth < .5`：它只把局部 `age` 置 0，经第 639 行和第 664 行流向 `display_age`，而当前 shader 中 `display_age` 并没有参与后续计算，无隐藏副作用。

### 引入的权衡（Trade-off）
- 原先三道防鬼影防线（`VTRUST` + `BOX` + `DOCC`）变为两道（`VTRUST` + `BOX`）。
- 在**极端场景**下（前景物体与背后低纹理背景颜色极其相近、躲过 `BOX` `0.02..0.06` 容差，同时两者相对运动速度很慢、躲过 `VTRUST`），被缓慢移开的前景刚露出的边缘可能比开 DOCC 时多残留 1 至数帧极轻微的旧增量，直到对应槽位轮转刷新。
- 在无强抖动且无高倍上采样的原生分辨率游戏中，DOCC 误报较少，若遇到此类慢速同色拖边仍可能有益。

### 回退与 A/B 对照方法
- 推荐通过系统或启动器环境变量设置 **`NR_IL_DEPTH=1`** 以恢复 `DILATE + DOCC`（`768u`）原行为做 A/B 对比；也可临时写入 `dlssnr-amd.ini` 的 `[Debug]` 段生效一次（注意：NR Control 保存设置时会重写 `dlssnr-amd.ini` 且不保留 `[Debug]` 段，需要持久化请务必使用系统/启动器环境变量）。
- `NR_IL_DEPTH` 严格只接受 `0` / `1` / `2` / `3` 四个值：`0`（完全禁用深度）、`1`（DILATE + DOCC）、`2`（仅 DOCC）、`3`（当前默认，仅 DILATE）。若传入越界或非法值（如 `true`、`5`），将回退默认模式（`3`，或 `NR_IL_ANTIGHOST=0` 时的 `0`），且 RenoDX 路径会在日志输出告警提示原值无效。
- 日志核验：RenoDX 路径（D3D12 与 Vulkan）在设置 `NR_IL_DEPTH=0` 时会明确输出 `depth disabled (NR_IL_DEPTH=0)`；取 `1`/`2`/`3` 且首次读取游戏深度时会输出 `depth mode <N>`，可直接核验当前生效值。

---

## 五、为何刻意未合入 `b24` 的其余改动（供后续复审）

1. **`01-d3d12-minimal.patch` 改动 B（`net_in_override` 让 `pass_net` 读 `il_base`）**：已在 commit `d11fef8` 通过 `slot_in` 参数更干净地实现；且原始 `0.0.9` 在没有 `268435456u`（proxy kept）时本就用 `Proxy` 传槽位输入，`b24` 的最小补丁丢掉了 `268435456u` 本身存在逻辑不一致。
2. **`03-full-working-tree-windows.patch` 中的 `268435456u` / `536870912u` / `1073741824u` 补齐**：已全部包含在 `d11fef8` 中。
3. **`NRIlAdvance` / `NR_IL_ADVANCE`（单帧推进多槽位）**：仅改了 Vulkan 且存在明显逻辑缺陷——`for (a = 0; a < adv; ++a)` 循环从 `a >= 1` 起会用同一帧 MV 重复搬移已搬移过的增量、`il(3u)` 翻转 `il_bcur` 破坏 `BasePrev`、且 `a == 0` 结束后深度图已退出 `GENERAL` layout。
4. **`il.comp` / `il0.comp` 扩展（104B push 块、`DOCC=2..8`、`DILATE=2`、`MOVEFILT`、`MVBLERP`、`NR_IL_WK`、`DEBUG=5`）**：相关研究方向多已被 `b24` 自身验证无效，且修改 push constant 会导致现有预编译 `d3d12_outer.nra` 失效。
5. **`NR_IL_HIST` + `il_final`（binding 48）、`NR_IL_INT`、`dlssnr-il.txt`**：`il_final` 在 Vulkan 桥接未绑定会崩溃（`03` 补丁中仍有 D3D12 死代码残留）；`NR_IL_INT` 会被 `if (raw)` 覆盖且与 `NRPass2..4Intensity` 重叠；`dlssnr-il.txt` 为临时排查配置，正式版不应额外维护平行配置文件。
