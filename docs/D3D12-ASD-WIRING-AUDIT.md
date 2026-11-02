# D3D12 ASD 接线独立审查

审查日期：2026-11-02。范围：产品 ASI → model → backend → RenoDX stage → ACO network；参考旧 Windows bridge 的生产调用。仅代码审查，未新增 GPU 测试。本文不将测试宿主缺陷当作产品缺陷，也不将以下问题直接归为 PBZ 崩溃根因。

## 已核实的正确接线和有意适配

| 接线 | 核实结果与依据 |
|---|---|
| 产品注册 planner / 控制补丁 / 模型选择 | `nr_asi_main.cpp:7820` 同时传 planner、`d3d12_net_controls`、`frame_network`，并安装 `evaluate_backend`。此前 file_planner 的 nullptr 控制补丁是离线宿主问题，已由 cfe514e 修复；产品未漏接。 |
| 游戏设备、命令列表、资源 | `nr_dlssnr_model.cpp::evaluate_d3d12` 从调用者 command list 获取 device，并将现有资源传 backend；未在该执行链增加产品 Vulkan 设备。canary 单独队列是诊断设施，与网络执行路径区分。 |
| 控制参数与每 pass 设置 | backend 填充 base_controls，stage 按 RenoDX settings 覆盖每 pass controls；`history_strength=1`、pass_follow、skin_independent 对照 `nr_pe_session.cpp::record_renodx` 有相同规则，是有意行为。 |
| temporal 输入 | backend/stage 传 motion、depth、reset、feature 和 normalized scale；网络按 feature 保存 latch/seed/history。当前样本同输入重放 neural 完整哈希相同，支持该 case 的接线正确，不能扩大为所有模式已验证。 |
| alpha 与 round_u8 | D3D12 控制 patch 不设 post 内 alpha/round 标志，由独立 alpha shader 执行；参考 post 的 bit31 差异是有意适配，不应为了 push 完全一致而删除参考功能。 |
| 图像与缓冲 | ACO 图像描述符使用线性 float4 缓冲，stage 负责 texture↔buffer；这属于 D3D12 资源适配，不是另造网络。 |
| barrier / Clear / 调用者绑定 | 显式状态转换和 shader-visible Clear GPU 描述符是 D3D12 必需项；backend snapshot/restore 用来恢复调用者绑定，不能照抄 Vulkan layout。 |
| 代码来源 | `PRE-CODE-PROVENANCE-AUDIT.json` 实证 PSDB 包含完整0081 pre代码，载体tag97/root98对应同记录；不同源码目录或SPIR-V文件标签不能推出执行了不同机器码。 |

## 真实缺陷：不是有意 D3D12 适配

### 1. 描述符循环复用没有完成保护

- stage：`nr_renodx_d3d12.cpp:1027,1270`，4096 dispatch 槽按 `% kSlots` 直接回绕。
- 普通 backend：`nr_d3d12_asd_backend.cpp:586,1347`，2048描述符、每evaluate4槽，512次直接回绕。
- CPU 重写 shader-visible heap 时，先前录制/提交的 GPU 命令仍可能引用旧槽；现有 token 完成水位只保护资源退休，没有保护这些描述符写入。
- 数量扩大只能推迟复现，不构成完成证据。应按实际完成保护分配段，或增长/退休 heap；槽耗尽时不能覆盖在飞段。无需改模型算术。

### 2. 阶段执行状态被折叠成成功

- `Stage::Impl::record` 既用0表示 NR关闭并成功copy-through，也用0表示 `work_b` / `decoded_alt` / preprocess / motion / exposure 分配失败。
- `nr_d3d12_asd_backend.cpp:1085-1091` 只把返回值写日志，随后总是 `note_backend(..., true); return true`。
- 对分离输出，分配失败可发生在任何输出复制之前；上层 `evaluate_d3d12` 因backend true直接返回NGX成功，不能执行既有fallback/等待复制，输出可能未写入。
- 不能简单将所有 `ran==0` 改false：NR关闭的copy-through是合法结果，网络失败时也可能已录部分内部命令。应区分 processed / valid passthrough / failure 并保留调用者状态恢复、已录命令资源寿命和准确状态。

### 3. device更换不能被当作旧device已空闲

- `wilsjo2_ready` / `evaluate_backend` 在传入device指针改变时直接调用 `drop_device()`。
- `drop_device` 清掉token/done/退休资源及stage；注释称调用时旧device无在飞工作，但调用点没有 fence/水位或device removed证据支持。
- 另外stage销毁时 `drop()` 会调用捕获全局 `retire(r)` 的回调；`drop_device` 先清退休表/重置水位、后清活跃stage，可能将旧device资源重新加入新一代的全局退休表。
- 应将owner/device、stage及退休队列按代际封装，旧代按自己的真实完成证据释放；不要用新device水位释放旧代对象。若仅支持单device，切换也需有明确的生命周期处理，不能凭指针变化立即释放。

## 执行顺序

继续当前 HDR codec 定位；以上三项由执行agent按独立修复提交收敛，先资源/描述符寿命，再失败状态。仅做覆盖具体缺陷的定向验证，复用现有离线宿主。正式性能测试前采用修正后的版本，并保留基线与差异证据。

## 补充：成熟机制复用核查（用户要求，2026-11-02）

### A. 完成与退休机制重复，且遗漏录制生命周期

当前 net.cpp 自建 copied/executed 两类 readback token、水位、上传块、网络/历史退休列表；backend.cpp 又建一套 frame token、水位与 stage/资源退休；stage 再维护 descriptor frame/completed 回调。必要的是分别知道上传何时可复用、GPU资源何时可释放，不必要的是三层分别重新定义录制/提交寿命。

成熟参考：`nr_pe_d3d12split.cpp::completion_fence`、ExecuteCommandLists/Reset 管理，`nr_pe_bridge.hpp::Job` 的 generation/busy/on_drop/cancel，以及 `nr_gpu_completion.hpp::Recording/Uses` 的录制保留、重复提交和父子录制结构。

**不能直接替换的具体缺口**：split 当前无 cut/held 的普通提交会直接转发，不为 ASD 暴露完成 ticket；GPU completion 现有64次提交加2秒的 fallback 是旧策略，不能当新D3D12真实完成证据。应扩展现有提交追踪层，提供按 command-list recording/Reset generation 的 ticket，记录真实 queue fence；未提交、重复提交和多队列都保留引用。backend/net/stage 使用同一凭据。不能为了原生路径主动增加跨API切分。

### B. Feature 销毁没有通知 ASD，改用数量/活跃窗口猜测

`nr_dlssnr_model.cpp` 的 feature release 当前通知 `g_d3d12_session->release_feature`；`D3d12AsdNetwork` 没有对应 release 接口。ASD 自行以64/128个feature、最近128次evaluate等规则淘汰历史。这不是 API 必需差异。

复用原有 owner 生命周期：将实际 feature release 传给 ASD backend/stage/network；按原始feature→stack_feature映射释放全部pass历史，并用录制完成凭据延迟销毁。LRU可以辅助预算，不能取代明确的销毁通知。此项是接线缺口，不要求推翻原有模型历史规则。

### C. 诊断逻辑散入生产路径

stage/net/runtime 各自添加多个环境变量、dump buffer、重放与跳过dispatch逻辑。它们对本轮定位有效，但不是产品功能，不应继续在每层无限增长。复用 capture 入口与离线宿主，统一诊断配置/输出元数据；测试重放、阶段截断需显式诊断模式。先保留可复现实验，再收敛，不删除审计证据，不以清理诊断为由中断当前修复。

### D. 不属于重复造轮子的部分

官方ASD注册与PSDB载体匹配、D3D12 descriptor heap/resource state、texture与线性缓冲转换、调用者绑定恢复是API适配。HDR编码HLSL与SPIR-V的运算结果差异应沿同一参考公式修正；不能因编译器不同就允许改模型精度，也不能直接把Vulkan设备/fence对象搬进纯D3D12路径。

优先顺序：当前安全修复 → 实际feature释放接线 → 统一现有录制/提交完成凭据 → 诊断收敛。每次可审查增量修改；不同时另造一套完整框架。


## 强制对照规则（2026-11-02，用户明确指令）

严格对照 Mochi 0.0.8.1：Linux 原生 Vulkan 直通实现作为算法与运行语义参考，Windows Vulkan 桥接作为产品接入、所有权与生命周期参考。除 D3D12 特有 API/资源契约必需差异外，不自行更改语义或建立并列机制；类型、字段和变量名尽量与参考对应。

此前审查只定位了成熟机制，没有把新类型与状态逐项映射，放任新增 Booking 框架，属于漏审。不能据已有局部审查声称其他部分没有遗漏。

剩余必须核对的范围：Recording/Uses 与提交/Reset/Release；owner/device代及资源退休；descriptor/upload/timestamp分配回收；feature及stack历史销毁；模型选择与降级；controls/seed/reset/history；motion/depth/exposure的尺度和状态；codec/look/preprocess与多pass；失败/等待/passthrough；产品配置优先级与诊断开关。每项记录原版函数/类型、当前对应、实际差异、D3D12必要性及处理结果。

不得用“API不同”解释非API行为改动，也不得以单一case通过推出全路径已审完。先复用原类型和结构，再补D3D12薄适配；不扩大为通用框架，不更改Linux参考本身，不扩DX9/11任务。保留已修安全性与全部证据。


## 剩余范围逐项源码核对（2026-11-02，本轮补齐）

参考固定为 main `1336d50ace388d5819f372640a00a6cb9be75ea2` 的 Linux 原生语义和 Windows bridge 接入。下表完成的是源码对照；修复和运行验证分别写明，不能把“已查清”冒充“已修完”。路径均相对本仓库。

| 范围 | 原版对应 | D3D12 实际对应与结论 | 本轮处理 / 执行动作 |
|---|---|---|---|
| 录制、提交、Reset、Release | `windows/src/core/nr_gpu_completion.hpp::Recording/Uses`；`nr_pe_bridge.hpp::Job`；split 的 Execute/Reset | 已把并列 Booking 结构改回原 `Recording`，严格查询排除64提交/2秒 fallback。提交需逐次保留 queue fence，成功 Reset 才 seal；Release 不代表已提交工作完成。现有 backend 仍有按帧水位消费者，net 仍有另一套 token。 | 原类型复用已落代码；剩余消费者必须接同一凭据，不能仅新增接口便宣布完成。保留重复提交、未提交后Reset、两队列的 CPU 用例。 |
| owner/device 代际 | 原 Session 的 runtime/stage 退休与 Job busy/recordings_done | `nr_d3d12_asd_backend.cpp::drop_device` 仍以设备指针变化直接销毁 stage/network/token；原“无在飞工作”注释无调用方保证。不是 D3D12 必须差异。 | 执行 agent 正在接录制 owner；旧代保留自己的设备、网络、stage和完成凭据，不能由新代水位释放。此项尚未修完。 |
| descriptor/upload 回收 | 原 Runtime 的 GraphSlot/Recording；Vulkan descriptor 更新随录制保留 | backend segment、stage heap 已避免简单回绕；net upload 仍以 copied 水位决定复用，不能覆盖重复提交录制仍引用的参数。 | 保留已修 heap 增长；把 descriptor/upload 生命周期并入同一 Recording，不扩大槽数冒充修复。 |
| timestamp | 原 Session::take_stage_time、Runtime::Impl::take_time 先取 query 可用性，未完成不计样本 | net 原先16次CPU调用后直接读 mapped readback，再重用 query。调用次数不是 GPU 完成证据。 | 本轮直接改成每 query 对持有原 `Recording`，完成后读回/复用；槽全忙只跳过计时，模型继续；DispatchLimit 不产生完整帧计时。丢弃录制清零结果，避免重复计算上一样本。已通过相关两源文件的 MinGW 语法检查；尚未做正式性能运行。 |
| feature及stack历史 | Session::release_feature / record_renodx 中 `entry.stack` 与 `release_everywhere(stack_feature(...))`；Runtime::release_feature | 实际 feature 销毁已接入 ASD；但 network 自建64/128 feature上限、128次活跃窗口；stack缩短未调用原版释放多余pass历史的逻辑。原版不会仅凭“较久没调用”重置仍存活feature。 | stack缩减照抄原 `entry.stack`/命名映射，只释放消失的pass；用实际owner销毁替代自行规定活跃窗口。不能清掉整个feature的全部pass代替。此项由执行 agent 接生命周期消费者时完成。 |
| 模型选择/降级 | Runtime构造、Session::wanted_network/check_availability；共享make_native_plan/NrSession::build | 产品 `frame_network()` 传现有 Session 选择；planner复用原 GraphCore lowering、权重包、noise jobs，不自造模型。缺PSDB记录时实验模型退small并记录note属当前交付范围；INT4明示未支持，不能称0081所有模型已支持。 | 保持当前目标模型，不扩INT4任务；请求key/实际模型key共享Built是缓存适配，但extent容量应按唯一Built而非别名数计算，避免无谓重建（效率问题，不解释崩溃）。 |
| controls/seed/reset/history | Linux与Windows `Runtime::record_all` 的InputDomain、latch、counter；Session::record_renodx::pass_controls | 输入域当前只含reversible，和原版一致；history_strength=1、独立skin、pass_follow规则一致。发现 stage 丢失 `Controls::seed_fixed`：有motion时关闭每帧噪声仍递增实际seed。 | 本轮已补同名 `seed_fixed`，stage沿原版赋 `!settings.noise_every_frame`，net在有/无motion分支均消费。原版recording丢弃会清latch/恢复历史；ASD尚缺对应取消通知，需在现有Recording生命周期接入，不另造时间窗口。 |
| motion/depth/exposure | model::normalized_mv_scale；Session::record_renodx；StageFrame与PlanFrameScale | 复用原尺度规范化、depth反转/override、pre_exposure/exposure_scale、AutoExposure标记；D3D12将纹理重采样成线性float4供ACO读取，属API资源适配。stage即使无游戏MV也提供中性guide，和原stage给network的engine帧用途对应，不能随意改为无temporal。 | 不改尺度/精度。直接纹理与线性buffer需各自保持真实state并恢复游戏资源；保留已有Clear与状态修复。对应模式运行覆盖仍属原计划，不能靠当前单case声称全部通过。 |
| codec/look/preprocess、多pass | `linux/src/core/nr_renodx.cpp::Stage::record/set`，Windows同名实现；RenoDX HLSL/Slang | history_settings_differ/recreate_settings_differ逐字段对应原版；stack pass controls和特征名对应。HLSL、texture/buffer和state不同是D3D12适配；不允许借此改变codec公式。当前实测HDR10proxy仍554个half值不同（548一ULP、6二ULP），同输入network输出已全字节一致。 | 不再重查已一致的ACO/权重/噪声；修真实v6绝对编码路径的末位差。按既有完整stage做对照，保留原始dump/哈希。此项仍是计划1的数值未完成项。 |
| 失败/等待/passthrough | 原model evaluate与Session::carry/record_renodx | 新record_ok已区分分配失败与合法0pass；network失败时stage复制原帧，保证输出。但backend只以record_ok判成功，网络失败后的合法passthrough仍被报“ASD network ran”；状态与真实执行不一致。 | 保留copy-through，不将所有0pass当错误；将ran结果传到已有路线/状态报告，区分NR关闭、等待、网络失败。已录命令的资源也必须持有到完成。 |
| 配置优先级/诊断 | 原Config/controls_from、Network选择、原NR_DIAG_*诊断 | 正常模型和菜单控制从现有Session/controls进入。ASD额外NetTemporal/NetNoiseEveryFrame、DispatchLimit、NR_TEST_PROXY_REPLAY等能改变实际结果，是定位设施，并非模型功能。原INI默认1不改变正常规则，但旧诊断配置会改变帧语义。 | 比较证据必须记全有效诊断值；正式对照不启用proxy重放/截断。沿现有capture与离线宿主收敛，不新增一套产品配置优先级；不能删除留存实验。 |
| 官方ASD交付入口 | 本项目ASD注册/PSDB载体、公共APIcanary | 真实ASD_HIT的诊断override已有证据；正常程序化注册E_HANDLE还未解决，不能用override代替产品注册完成。与Mochi的模型/codec实现无关，不能修改模型绕过它。 | 生命周期/codec工作继续推进；恢复正常程序化注册后跑完整生产入口，再进入正式性能比较。 |

### 本轮直接修复与交接边界

直接修改仅三文件：`nr_d3d12_net_plan.hpp`、`nr_renodx_d3d12.cpp`、`nr_d3d12_asd_net.cpp`，内容为固定seed和timestamp完成检查。与执行agent明确协调了修改段；没有启动GPU、部署游戏或修改游戏配置。语法检查命令：

```bash
x86_64-w64-mingw32-g++ -std=c++17 -O0 -fsyntax-only -Iwindows/src -Iwindows/src/core -Iwindows/src/pe -Itoolchain/Vulkan-Headers/include windows/src/pe/nr_d3d12_asd_net.cpp windows/src/pe/nr_renodx_d3d12.cpp
```

退出0。共享controls头已改变，执行agent的后续链接必须重编所有消费者，不能拿旧对象混链。

执行顺序保持不变：完成原Recording消费者和旧device保留 → stack缩减/取消录制语义与准确状态 → 当前HDR10 codec和正常ASD注册 → 同目标case完整离线集成 → 计划2真实桥接/直连分段比较。以上修复纳入现有计划，不新增框架任务；每一修复写真实提交和验证结果，再继续下一项。
