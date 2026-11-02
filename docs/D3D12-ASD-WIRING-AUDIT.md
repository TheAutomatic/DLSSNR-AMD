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

