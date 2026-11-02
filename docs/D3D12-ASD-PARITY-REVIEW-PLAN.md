# D3D12 ASD 后端与 Mochi 两条既有路线的对照审查计划

日期：2026-11-02。用途：无人值守夜间任务。agent 自行搭建离线宿主、对照审查、运行、定位、修复与复测，以 PBZ 失败条件为案例来源。
本计划只新增文档；不要求重做公共入口研究、模型提取或整套性能测试。


## 执行方式：全程离线、无人值守

用户不参与本轮执行。P0→P5 完成后自动衔接性能计划 Q0→Q6。所有构建、输入准备、D3D12/Vulkan 宿主、运行、诊断、修改、复测、证据整理均由 agent 完成；不得把启动游戏、截图、操作菜单、复现现场或确认结果安排给用户，不等待用户回复才继续。

PBZ 目录只用于读取已有日志/配置并提取复现条件。此次不启动游戏、不向游戏目录部署、不修改游戏安装或配置。需要游戏提供的 device/list/资源时，由独立测试进程扮演调用者并创建等价对象，将它们传给真实后端入口；不把这个测试宿主设备混入产品路径。

已有 harness 不足就自行扩展或搭建合适宿主，包括真实桥接输入输出、资源初始/退出状态、原位关系、格式、motion/depth/exposure、历史、命令录制/提交与状态恢复。已有捕获输入可直接复用；缺少图片或历史序列时自行生成可重现的 HDR/SDR 输入和 guides，记录生成规则、seed 与哈希，不等用户补素材。合成案例明确标注，不称为捕获到的真实游戏帧。

离线失败由 agent 读取 Close/设备状态及可用诊断定位，修复后重跑必要案例；只做到源码检查或把失败报告交给用户继续测，不算完成。测试进程设置超时、退出码和独立输出目录；崩溃/设备移除后保留现场，在可运行条件下修复并从检查点继续。工具/头文件优先用本计划已列出的本机资产，自行解决可解决的构建问题。

若存在无法自行恢复的环境错误，保存精确错误和恢复所需条件，继续完成其他独立工作，最终报告剩余项，不把夜间流程改为等用户交互。遵守当前会话权限与已有 GPU 串行规则，不停止用户进程。
## 目标与事实边界

目标是让原生 Windows D3D12 路线复用 Mochi 的模型和帧语义，正确管理游戏已有 device、command list、资源与 GPU 生命周期，最终去掉这条路线额外 Vulkan 设备和桥接。

相对 main，新增加的 nr_renodx_d3d12.cpp、nr_d3d12_asd_backend.cpp、nr_d3d12_asd_net.cpp、nr_asi_wilsjo2_d3d12.hpp 是本次重点。已发现的非法 Clear GPU 描述符、同状态 transition、COMMON/UAV 处理、外部状态登记、按 evaluate 次数回收等问题位于新增实现；不能据此说 Mochi 原有 Vulkan 算法存在同样缺陷。

旧桥接没有暴露同样症状，是有价值的对照，但不是自动证明。桥接负责 D3D12 外部边界和跨 API 同步，Vulkan stage/runtime 负责内部图像、layout、descriptor 和执行依赖。新后端接管了这两侧职责，遗漏容易出现在交接处。

COMMON 不是任意状态：普通非 simultaneous-access 纹理可以从 COMMON 隐式提升到 SRV/复制状态，不能这样提升到 UAV；buffer 也不是任意状态可随意访问。Vulkan UNDEFINED 表达丢弃旧内容的 layout 起点，不是 D3D12 资源状态的替代物。具体规则参考微软官方 Resource Barriers 文档，不沿用此前错误注释。

## 工作区与版本

- 主要开发 worktree：C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring
- 分支：feat/d3d12-asd-wiring
- 文档建立时 HEAD：824ffc6b84cd176ee284b9cd99dbd2cc3d2faf21
- main 基线：1336d50ace388d5819f372640a00a6cb9be75ea2
- Mochi main checkout：C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki
- 0081 release：C:\Users\Kenny\Downloads\DLSSNR-AMD-Vulkan-Windows-0.0.8.1-win11-lang-preview-x86_64
- ACO/ASD 原始成功实验：C:\Users\Kenny\Downloads\amd-d3d12-aco-lab
- 游戏现场：C:\Program Files (x86)\Steam\steamapps\common\PBZ\platform\steam
- 本计划：C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\docs\D3D12-ASD-PARITY-REVIEW-PLAN.md

接手先读一次 git status/diff。当前 nr_renodx_d3d12.cpp、nr_d3d12_asd_net.cpp 有进行中的修改，另有 plantest.cpp；不得 reset/覆盖。最新未提交状态登记与元数据修复也要审查，不能只看 824ffc6。
main 固定为旧路线参照；新后端修复只在当前 worktree 落地。Linux 源码先作为只读语义参考，不因 Windows API 错误改动 Linux。

## 三路线的源文件对应关系

以下目录与文件均为绝对路径。

| 职责 | Linux/Vulkan 原生参考 | Windows Vulkan/桥接参考 | 新原生 D3D12 实现 |
|---|---|---|---|
| 图、权重、push、实际算子顺序 | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\linux\src\core\nr_graph.cpp | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\windows\src\core\nr_graph.cpp | C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\windows\src\core\nr_runtime.cpp 的 plan_d3d12_network；同目录 nr_d3d12_net_plan.hpp |
| 帧执行、历史、seed、控制参数 | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\linux\src\core\nr_runtime.cpp | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\windows\src\core\nr_runtime.cpp | C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\windows\src\pe\nr_d3d12_asd_net.cpp |
| RenoDX stage、HDR/SDR、前后处理、look | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\linux\src\core\nr_renodx.cpp | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\windows\src\core\nr_renodx.cpp | C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\windows\src\pe\nr_renodx_d3d12.cpp |
| 资源/描述符/dispatch 实现 | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\linux\src\core\nrvk.hpp | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\windows\src\core\nrvk.hpp | C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\windows\src\pe\nr_d3d12_asd_backend.cpp，以及 net/stage 实现 |
| ACO 装载与模板 | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\linux\src\core（按实际装载入口追踪） | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\windows\src\core\nr_pipeline_binary.hpp、nr_pal_binary.hpp | C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\windows\src\pe\nr_d3d12_asd.cpp、nr_d3d12_asd_reprofile.cpp |
| 游戏外部资源、前后状态、帧调用接缝 | 原生 Vulkan 的调用者资源契约，以实际代码为准 | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\windows\src\pe\nr_pe_session.cpp 的 run_renodx、lookup12、begin_recording；同目录 nr_pe_bridge.cpp | C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\windows\src\pe\nr_dlssnr_model.cpp、nr_d3d12_asd_backend.cpp、nr_renodx_d3d12.cpp |
| 命令列表状态保存/恢复与提交 | Vulkan 原调用路径 | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\windows\src\pe\nr_pe_d3d12split.cpp、nr_pe_bridge.cpp | C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\windows\src\pe\nr_pe_d3d12split.cpp、nr_asi_main.cpp |
| wilsjo2 流程 | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\linux\src\core | C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\windows\src\core\nr_wilsjo2_flow.hpp；windows\src\pe\nr_asi_wilsjo2.cpp | C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\windows\src\pe\nr_asi_wilsjo2.cpp、nr_asi_wilsjo2_d3d12.hpp |

表内“按实际代码追踪”的入口不得凭文件名假定已等价；只追当前帧可达的调用。共同数学优先直接复用，D3D12 的 barrier、heap、root binding、完成信号必须按 D3D12 契约实现。

## 进度表与执行次序

| 步骤 | 当前状态 | 任务及完成证据 |
|---|---|---|
| P0 固定现场和边界 | 已完成（离线 case） | 记录源码/候选构建与已有安装 ASI 身份、runtime、模型/配置和资源契约；保存已有现场，生成可重复的离线 case |
| P1 外部/内部资源所有权和状态对照 | 进行中（4 项已修，见 RESULTS） | 一份精简资源契约表；修掉状态表、显式转换和外部边界不一致，覆盖同类调用点 |
| P2 描述符/复制/Clear/命令恢复对照 | 进行中（SRV 映射、Clear、同状态转换已修；UAV 能力校验待做） | 一份差异清单，含来源函数、实质差异、修复和证据；完成当前帧可达路径审查 |
| P3 模型及生命周期对照 | 剩余范围源码复核已补齐；seed/timestamp已修并编译；生命周期消费者在修，见 WIRING-AUDIT 最后对照表 | 图/push/帧状态语义沿用原实现；上传、token、历史、PSO/资源销毁有覆盖最后使用的完成条件 |
| P4 同场景离线定位与比较 | 首帧已定位并修复；3 帧离线通过；首帧可重复性未解决 | 真实 RenoDX+ACO 首帧与必要后续帧；Close/设备状态及可用验证输出；与旧路径同输入比较 |
| P5 离线集成复现、回归与收敛 | 进行中（宿主 + 真实入口可跑；分离输出/resize 案例待补） | 独立宿主调用真实集成入口，复现 PBZ 资源契约及必要后续帧；无设备错误、输出对齐、状态恢复正确，保存自动运行结果 |

每完成一项就在本表更新状态，附源码位置、运行产物路径和实际结论。不把“能编译”“日志录完”“查到一个确定缺陷”写成“闪退已修复”。当前不额外建立多 GPU、多驱动、多尺寸测试矩阵。

### P0：只固定当前案例，不重做资产调查

离线宿主基于 PBZ 已有日志/配置生成案例，固定 HDR10、4K/format 24、原位 colour==output、入口 UAV、RenoDX 前置、实际模型/参数。早前 1080p/format 26 是另一次现场，不能混成同一复现。
保存 nr-diag/run.txt、dlssnr-amd.log、相关 ini 与构建身份。命令流水证明 CPU 录了什么，不证明 GPU 已执行到最后一行；若只有 0x887A0001，根因仍按具体错误继续定位。

### P1：先画资源契约，随后统一实现

对当前帧实际用到的资源填一行：资源名/owner → 创建初态或调用者传入状态 → 每步 read/write/copy/clear → 所需状态与依赖 → 退出状态 → 状态表由谁更新 → 何时可销毁。按同一逻辑把旧 bridge 外部契约与 Vulkan 内部依赖拼起来，检查新后端是否完整承接。

重点对象：colour/output（同指针和不同指针）、motion/depth/exposure，以及 original/proxy/neural/decoded/work/scale/commit/prep_state、激活/权重/历史/参数表。

必须解决的具体问题：

- 新建自有资源从真实创建初态登记；外部资源每次进入按调用者当前状态登记，不按“第一次见到这个指针就是 COMMON”推断，也不永远沿用第一次帧状态。指针复用不能携带旧资源记录。
- colour==output 时只能有一份一致状态；若输入/输出契约矛盾，先定位调用者，不让两个名字维护冲突状态。
- 所有转换方式都更新同一状态事实，包括手写 barrier 数组、transition、ensure 和复制 helper；退出返回调用者承诺的状态。状态已相同不发 transition，但必要 UAV 依赖仍保留。
- 显式状态表、COMMON 提升/衰减和 ExecuteCommandLists 边界必须一致；不把两次提交之间已衰减的状态当成仍然保留的读状态。先用清楚可证明的方案，不在修闪退时顺便优化 barrier 数量。
- 以原 Vulkan 的 producer/access/stage → consumer/access/stage 为依赖意图，逐一翻译为 D3D12 资源状态、UAV、复制及跨队列依赖；不能只看 VkImageLayout 的名字。

### P2：按操作类别横向扫一遍，避免再逐处补丁

对新增文件中的以下类别一次性枚举，按同类逻辑批量修正；不为每个调用另写长篇报告。

- Clear：GPU UAV 在当前绑定 shader-visible heap 中；CPU handle 对应合法 CPU descriptor；两者指向同一视图；状态在 Clear 前正确，依赖在其前后正确；GPU 槽在执行前不被覆盖。
- dispatch：真实 PSO/root signature 对应，root constants/ByteAddressBuffer/structured stride/register 表完整；切堆会使哪些 descriptor tables 失效；网络 callback 返回后 stage 必须重绑它使用的堆与表。不能假定“stage 入口绑定过一次就始终有效”。
- copy：原位和分离路径、源/目标尺寸、格式、subresource、box 与 offset 的契约一致；不只看指针存在。按旧桥接 lookup12 的尺寸/格式限制检查 native 侧对应行为。
- 调用者状态：追 split snapshot/restore 实际保存的绑定，不只恢复 resource state；Close、提交顺序、command list 重置/丢弃时的行为要自洽。
- 失败返回：开始录制前完成能失败的准备；中途失败后不能留下半段转换、已推进的历史或重复初始化，却向上层宣称“什么都没录”。

### P3：复用图与帧语义，重新审计完成信号覆盖范围

- 对照 nr_graph/runtime，核对所选内核变体、推送大小/字段、buffer/image 绑定次序、dispatch 与实际权重/激活布局；禁止替换数学或精度来掩盖错误。
- 对照原 record_all/record_renodx：HDR 输入单位、曝光/白点、工作尺寸、图像转换、控制参数、alpha、历史 gate、seed、reset、每 pass 独立 feature。相同配置应保持同语义。
- 按资源类别明确最后使用点：参数 upload 只需复制完成；PSO/激活/历史需模型执行完成；stage 纹理和描述符可能还要等后处理/写回完成。不能共用一个位置过早的标记。
- 全部退休点都要审查，包含 net、backend、RenoDX stage、extent/feature 缓存、设备切换；不能只把某一处 tick+64 改掉。退休后的水位仍需更新，保存水位对象的生命周期也要完整。
- 记录/提交顺序和队列必须与水位推断一致；“最新编号已完成”只在相应顺序确实成立时才覆盖较早编号。被重置/未提交的录制不能被误认为执行过。
- 活跃 feature 不按很短的 evaluate 窗口反复淘汰；以原 feature/pass 生命周期为依据。当前单 pass 首帧只需最小运行，不因此新增大型多 pass GPU 矩阵；缓存策略可先用 CPU 逻辑验证。

### P4：已有 harness 服务定位，不另建庞大测试系统

入口：C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\windows\tests\d3d12_asd\nr_d3d12_asd_test.cpp
构建：C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\windows\build\build_d3d12_asd_test.sh
现场已有 harness：C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\artifacts\windows\asd-harness
已有 plan.bin 以该目录/现场实际定位结果为准，不因为旧文档大小或路径假定它仍匹配新帧。优先复用现有计划转储与输入。

由 agent 在离线宿主中按 PBZ 已知配置走完整 RenoDX+ACO 首帧，检查 Close 返回、提交后等待结果、GetDeviceRemovedReason，以及可用的验证/DRED 输出。完成一次必要验证后，若通过，只补本次改动涉及的后续帧/状态路径，不反复重跑所有旧案例。

若本机实际存在 ACO 与某种验证设置不兼容，记录启用步骤、HRESULT 和 runtime/DLL 身份；不要写成“ACO 普遍与 DRED/调试层互斥”。可以用两个宿主模式：stage 包装用合法网络替身定位 D3D12 调用；真实 ACO 模式核对实际执行和输出。前者通过不能代替后者。

直接数值对照优先用 Windows 旧 Vulkan 路线的同版本 ACO、同输入/权重/参数；Linux 先做源码语义参照，不要求搭新 Linux GPU 环境。已有可信产物可复用。只在实际偏差出现时读取对应边界，不额外采集所有张量。

DRED 官方定义已在本机，无需搜索新工具：
C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\d3d12.h
C:\Users\Kenny\Downloads\amd-d3d12-aco-lab\exports\inputs\asd-public\microsoft.direct3d.d3d12.1.619.5\build\native\include\d3d12.h
查 IID_ID3D12DeviceRemovedExtendedDataSettings，并通过 D3D12GetDebugInterface 获取，不是猜 CLSID。

### P5：离线集成复现、回归与收敛（agent 自行完成）

用独立宿主模拟游戏/OptiScaler 调用者，传入其自建设备、command list、colour/output/guides 和声明状态，执行真实生产路径；不可仅在绕开集成代码的算子 demo 上通过。桥接路线也必须跑实际交接和同步，而非用纯 Vulkan 小内核代替。

从已知失败的 4K/HDR10/format 24、就地/UAV 条件开始，自动验证完整 RenoDX+ACO 首帧、后续帧、输出和设备状态。针对本轮修复涉及的契约，以少量定向案例覆盖：原位/分离输出、实际支持的入口/退出状态、必要的跨帧 reset/历史，以及涉及的 resize/多 pass/回收边界。每个新增案例必须对应实际修改或已发现风险，不扩成任意大矩阵。

宿主必须实际 Close、提交并等待相应 GPU 完成，再读取输出/状态；在生产相关路径核对命令列表绑定和资源退出状态。失败自动留下版本、case、Close HRESULT、设备错误、流水及可用 DRED/验证信息，agent 根据首个真实异常修复、复测，不交回用户重现。

本阶段完成条件：保存可从命令行无人值守运行的入口、固定案例/输入生成方式、实际运行结果、输出对比与修复记录。完成的是“离线集成验证”；不写成“PBZ 实机已通过”，也不把未执行游戏验证列为夜间任务缺口或性能计划前置条件。

通过后自行进入下一个性能计划，保留正确性基线，用户无需启动游戏、确认阶段结果或再次授权普通后续步骤。
## 交付与记录

持久审查结果放：
C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\docs\D3D12-ASD-PARITY-REVIEW-RESULTS.md

用一张表记录：类别 | Linux/旧桥接来源函数 | native 对应函数 | 契约差异 | 已修复/待解决 | 验证证据。只记录有实质差异的项目，纯等价段落合并说明，不机械逐行打勾。

构建/运行证据统一放：
C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\artifacts\d3d12-parity-review

结束时交付修复代码、简短差异表、离线集成宿主/案例、正确性结果和审计证据；随后继续性能任务。未运行的验证明确记未运行。不要修改已有 Linux 参考、重写模型数学、另开注册/API 研究或在这次排错中扩成性能优化项目。

## 给执行 agent 的开工指令

读本文件和当前工作树差异。以 PBZ 首帧闪退为目标，先完成 P1 的外部/内部状态契约，然后按 P2/P3 横向检查同类移植问题并修复；参考 Linux Vulkan 的计算与依赖语义、旧 Windows bridge 的 D3D12 外部边界。由 agent 自行复用/扩展 harness 和同配置 Vulkan 参考，离线完成全部验证，不安排用户参与。不要只顺着日志最后一行逐个修补，也不要重做已通过的 ACO 公共装载调查。完成一项更新上方进度表，并留可复现证据。夜间执行由 agent 持续推进到离线验证和后续性能计划交付，不在阶段间等待用户。

## 正确性工作完成后的下一项

按用户要求，完成本文并记录通过证据后，执行独立性能对照、诊断与修复计划：
C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\docs\D3D12-ASD-PERFORMANCE-REVIEW-PLAN.md
性能计划要求保存 ACO bridge / 直接 D3D12 / 修复后的成套原始证据，供后期审查。当前不提前启动性能测量。

## 实现原则（用户明确要求，2026-11-02）

一律优先成熟思路，不自行重复造轮子。先查 Mochi Linux 原生 Vulkan、旧 Windows bridge，以及现有 D3D12 split/队列提交追踪的资源与完成管理；能复用就复用，需要适配则只替换 API 与真实契约差异。新机制必须基于现有实现的具体缺口，不能以 API 名称不同为由重新设计整套生命周期。

描述符不强制循环槽位表。优先按录制/提交生命周期分配保持不变的描述符空间，GPU 完成后统一回收；空间不足分配新块，不能覆盖在飞内容或跳过 dispatch。优先接入已有提交完成/fence 管理，不再并列新增多个自造计数环、水位和失败门控。此原则用于后续正确性修复和性能优化，采用可审查的增量修改，不以重构为由中断实际推进。

## 实现原则（2026-11-02，适用于本计划全程）

1. **优先成熟机制**：生命周期/完成判定先复用现有实现——`split` 的每队列 fence 与 continuation 复用、
   `nr_gpu_completion` 的 Recording/Uses 结构、旧 bridge 的会话追踪；能适配就不新造。
2. 不许先加"自造计数环/轮询水位/失败门控"，再层层补洞；描述符按录制生命周期分配不可变空间，GPU 完成统一回收，
   空间不足则新分配。
3. 若现有机制确实接不上，写明具体缺口，只做最小实现，并保留可审查增量。
4. 诊断开关收敛到统一入口/测试宿主，不并列扩散新环境变量。
5. 不同编译产物的末位差（如 HLSL/fxc 与 Slang）只记录为现象，未定位到具体运算前不作"优化器重排"之类的归因。
