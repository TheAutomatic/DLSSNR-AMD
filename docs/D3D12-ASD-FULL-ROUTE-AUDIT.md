# D3D12 全路线复用、完成度与遗漏总审查

2026-11-02。此表覆盖整个路线的已做与未做内容，不限HDR10/F32。原有WIRING-AUDIT保留排错历史，本文件作为统一分类入口。

## 固定范围与权威来源

- 工作区：`C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring`，分支`feat/d3d12-asd-wiring`。
- 原版基线：`1336d50ace388d5819f372640a00a6cb9be75ea2`。Linux原生Vulkan负责计算/帧语义参考，Windows bridge负责入口/owner/队列/资源交接参考；源码目录`C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki`。
- 0081发行资产：`C:\Users\Kenny\Downloads\DLSSNR-AMD-Vulkan-Windows-0.0.8.1-win11-lang-preview-x86_64`。
- 官方本机接口：`C:\Users\Kenny\Downloads\amd-d3d12-aco-lab\exports\inputs\asd-public\microsoft.direct3d.d3d12.1.619.5\build\native\include` 的d3d12.h、d3dshadercacheregistration.h、d3d12compiler.h。
- 本轮73个改动文件全部归类，包含生产、测试、打包和旁支。固定HEAD、工作区差异、每文件绝对路径/大小/SHA在`C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\artifacts\d3d12-parity-review\full-route-audit\source-inventory.json`。本报告与本轮后续修复另计。
- 源码核查、已实现、编译验证、实际运行分别记录；不以一条路线通过代表其他分支通过。

## 一、全部模块分类

| 类别 | 现成原版成果与复用方式 | D3D12必要适配 | 已做/未做及处理 |
|---|---|---|---|
| 1 产品入口/游戏对象 | 原evaluate_d3d12、NGX feature、OptiScaler/Present入口 | 传调用者device/list/resources，保存恢复绑定 | ASI已接planner/controls/model-query/backend，正常离线生产入口已跑；copy-through状态仍需收敛R05 |
| 2 模型选择/权重/图lowering | Runtime构造、make_native_plan、NrSession::build、GraphCore与原model pack直接复用 | weights_out/plan_only导出dispatch/push/权重tail，D3D12自己上传 | 已实现，不新建产品Vulkan设备；权重/noise已对齐。experimental缺记录退small有note；INT4未接，不当作0081所有模型已覆盖 |
| 3 ACO对象与公开交付 | 0081 ACO records、微软ASD/Compiler Plugin、AMD公开family/ABI/profile | carrier/PSDB/SODB、root参数、代码对象与图像/缓冲描述符 | 真ASD命中及同输入neural逐字节一致已有证据；正常注册E_HANDLE仍未解。驱动匹配走公开编译器/ABI/profile重生成，不回私有函数偏移hook |
| 4 注册与重profile | SDK Installer/Application/Component、CompilerFactory/CacheSession | 等价MinGW声明、exe/profile与安装路径、compiler DLL工具调用 | 已写注册/reprofile工具，已读SDK方法匹配；正常程序化注册及驱动更新实际重生成未通过，override不替代产品流程 |
| 5 canary/启动 | 原lab公共API canary | 在调用者device验证载体执行 | 已有ASD_HIT；force_canary不算命中证明。默认FreshCanary额外设备诊断R06本轮已改默认0，显式诊断保留 |
| 6 普通D3D12资源包装 | 原model资源边界、Runtime输入域与alpha规则 | 格式/SRV/UAV、纹理与float4缓冲、state/barrier、回写 | 已修SRV映射/Clear/同状态转换等；正常3帧过。backend旧对象退休R01、提交顺序契约R09尚未全闭环 |
| 7 RenoDX host/SDR/HDR/scRGB | 原nr_renodx.cpp、codec_gain.hpp、Settings、record_renodx | VkFormat映射DXGI，资源录制API变化 | 原数学/帧语义复用，seed_fixed/stack缩减已修；HDR10目标proxy仍554值不同。SDR/scRGB各分支存在，未据此宣称全模式跑过 |
| 8 codec/look/Preprocess/fast | v6/legacy/look同源HLSL；原prep与renodx-fast GLSL | HLSL可直编；原GLSL需等价D3D12表达 | v6主要文件两树相同。prep/fast手写HLSL需按对应原实现定向核对，不能凭same math注释认定逐位等价；不改公式和精度 |
| 9 temporal/controls/feature历史 | Runtime InputDomain/latch/reset/seed、原stack_feature/release_feature | guides转线性缓冲，历史按原Recording/Uses持有 | 固定seed、实际feature销毁、stack缩减、初始化丢弃恢复已落代码；正常case与18项CPU证据已有。device/discard边界实际验证仍需完成 |
| 10 wilsjo2原生style | 原nr_wilsjo2_flow.hpp和Stage::Backend直接复用；原SPV经SPIRV-Cross | Stage12 PSO/t/u/t6、push常量改只读buffer、DX12纹理API | Flow/Constants确实复用，独立宿主已有；W2Native默认1，生产可达。描述符/上传/计时/设备代R02–R04此前漏审，不能算已完成 |
| 11 提交/Reset/Release/退休 | 原Recording/Uses、split per-queue fence、bridge::Job | 普通Execute也记fence，成功Reset结束录制，strict完成不取时间退让 | Booking已移除，net/Built/FeatureState/Upload/时间戳已接原对象；backend和W2剩余消费者仍要接全。多fence释放证明不等于共享工作区执行顺序已正确 |
| 12 Present/HUD/帧生成 | 原present_layers、HUD FIX、原wilsjo2 Flow与NR Control颜色判断 | 在原入口使用真实DX12输出和命令列表 | 未另造游戏扫描/驱动hook。附带跨swapchain/display猜色彩及D3D11行为变化需独立注明动机/验证R10，不自动当bug，也不混为必需DX12适配 |
| 13 配置/菜单/OSD/状态 | 原Config/controls_from、Session模型选择和NR Control诊断页 | ASD特有启用/注册/路径状态 | 已复用菜单通道；多个诊断INI/env与状态字符串前缀识别需收敛。原样复制仍可报NR运行R05；不自造新的优先级 |
| 14 捕获/F32/定位 | 原Session::shot_prepare/copy/recorded/poll按格式容量/行距/完成读回；bytes_per_pixel已有RGBA32F | DX12 footprint/rowpitch/fence；真正half-store前32位观测 | 既有half/raw捕获可用；raw输入只是half展开。F32参考侧改造进行中，双侧尚未闭环R08；不新造capture框架，不改产品proxy精度 |
| 15 构建/依赖/生成/安装 | 原build_asi/package/assemble/install、固定Slang/glslang及lazy加载 | 编入ASD对象、包HLSL/PSDB/SODB/W2 DXBC/reprofile工具 | 已接构建与可选ASD目录，真实split正式宿主脚本已补；成套资产manifest/最小安装树验证待收尾R11/R13。旧OptiScaler-NR下载移除当前无生产引用，不误报为当前nightly依赖缺失 |
| 16 离线宿主/比较/证据 | 现有ASD、纯VK、真实D3D12桥接host与固定输入 | 创建等价caller对象，真实等待，按格式解析/统一元数据 | 有新exe正常3帧完整SHA和raw日志；旧exe伪验证已撤回。compare_dumps当前仅R10/RGBA16F；F32解析与未知kind处理待补R08 |
| 17 性能与优化 | 原Runtime/Session query、真实bridge和ASD，原性能计划 | DX12时间戳/频率、跨API交接与共同输出边界 | net计时已修，真实桥接host已有。Q0–Q6未完成，无正式性能差值结论；W2计时先修，留raw ticks/身份/结果 |
| 18 Xbox/注册表旁支 | 独立asd-xbox-test及旧lab实验 | 当前目标无必需适配 | 源文件已盘点，本轮未执行；不放正常安装/注册路径，不扩调查，不删除证据，不要求用户参与 |

## 二、集中问题清单

| ID | 已核对的差异/遗漏与来源 | 处理与优先级 |
|---|---|---|
| R01 backend退休遗漏 | retire旧资源保存当前g.booking；renodx_retired/token_retired仍按frame最大水位。当前录制完成不能替代旧资源其他使用完成 | 复用原Uses，快照实际使用集合；和已修net做同一轮消费者闭环，正确性必要项 |
| R02 新生W2槽覆盖 | Stage12::record与record_fast都4096槽取模；heap和upload无实际Recording owner | 按原Recording/Uses保留不可变描述符/常量块，两入口一起修；不另造ring或仅加槽数 |
| R03 新生W2计时 | native::Timing按16次CPU调用读回并复用query，无完成查询；设备变化也未更换owner | 复用真实Recording与设备代；槽忙只不计该样本，不跳模型；正式计时前修 |
| R04 W2新代/旧策略 | programs切device用stage.release永久遗留；表面退休沿用原64调用+2秒与Uses::ready退让 | 继承策略不是新发明，但原生分支需接strict真实owner；复用现有设备代退休，不永久泄漏 |
| R05 状态仍混淆 | 6951761加net_ran/日志后仍note_backend(true)；model见backend true就note_route(kRouteAsd)和report_control(...true,true) | 按原RecordResult/ran/waiting传真实结果；合法copy-through继续成功，不靠失败门控拒绝 |
| R06 诊断默认额外设备 | canary线程FreshCanary默认1会fresh_canaries/D3D12CreateDevice及其他adapter诊断 | 本轮改未配置默认0；显式实验保留，安装配置也需核实不默认打开。模型执行继续用caller设备 |
| R07 后台读取与销毁 | 新heartbeat线程device_removed_reason读g.device，status读g.network；evaluate/drop_device修改释放它们，backend无共同同步/稳定引用 | 复用owner锁/状态发布，读者持稳定引用；同代状态、canary与设备身份对应，不让日志线程影响销毁 |
| R08 诊断成套格式缺口 | half固定footprint/W*H*8、raw half展开、旧副本和比较脚本无F32已造成无效对照 | 按原格式驱动capture一次接齐源格式→绑定→容量→行距→等待→dtype，仍用现有宿主 |
| R09 顺序契约尚缺证明 | 同Built act/table/history被多录制共享，Release完成证明不提供原Job/Session串行执行关系 | 核对并复用原提交顺序依赖，定向验证必要场景；此为架构契约缺口，不称已复现崩溃，不因不确定新增用户准入限制 |
| R10 附带跨路线行为 | colour_space_guess让W2借用NR Control猜测，nr_asi_main也改D3D11分支 | 是有动机的语义对齐，非DX12 API必要项；单独标来源/验证，不盲目撤也不当本目标已验证，不扩DX11测试 |
| R11 交付与证据链 | 可选ASD目录复制、W2 DXBC和工具生成分散；曾漏plan/旧副本/旧exe | 复用assemble/install统一成套身份，最小离线安装树验证；run记录源码/载荷/plan/宏/格式/真实exit，保留前后证据 |
| R12 W2失败清理小缺口 | Stage12::make本地made存raw PSO，required程序中途失败可在释放made前return，初始引用遗留 | 随W2资源修正用既有COM清理/RAII处理，不为小问题扩GPU矩阵 |
| R13 缓存/生成身份收尾 | fetch_deps只看单个d3d9.dll就跳过整组提取；新DXBC流程外部传入cross工具和SPV | 核成套文件与工具/输入身份，沿原固定版本和清单；交付收尾，不打断codec定位 |

R01/R07/R09表明此前局部完成不能概括为整个生命周期闭环；R02–R04是此前整条W2分支漏出范围。其余代码不能因属于其他文件就从总清单消失。

## 三、已完成实质成果和可复用证据

- SRV映射、Clear可见heap、同状态transition、状态登记、file_planner controls等已修，不重复同一正常case验证。
- 同输入neural完整SHA2853eb01…一致；权重/noise/实际PSDB的0081代码来源已有原始证据，不重查71层。
- Booking已移除；net使用原Recording/Uses；上传凭据按实际buffer槽保存；Built/FeatureState和退休创建点已实际接通。固定seed、stack缩减与初始化丢弃恢复已实现。
- 74fc4ed永久提交事实：18项真实CPU回归0失败；新exe正常4K HDR10三帧exit0、无误重建。完整SHA `AD0B0B8A70A27E4A81F1184E69B5B2AE76947737D8311C17763E69D02A076DC2`；原日志和身份在`artifacts/d3d12-parity-review/build-logs/run-ad0b0b8a.*`。正常case不代替设备/discard边界。
- precise肩部候选554→869个不同值，b5a56a9已回退；失败dump保留。CPU逆变换归因已撤回，只有真正F32观测能定位首个不同边界。

## 四、纳入原计划的执行顺序

1. 当前codec继续完成最小双侧F32观测，定位554值首个不同处；R01/R07/R09与W2 R02–R04作为同一轮成熟生命周期消费者修正，不能再建并列框架。
2. 正常程序化注册按首次失败调用/本机SDK推进；独立CPU核查不被单点阻塞拖住，GPU仍统一串行。
3. 用现有生产入口完成目标模型完整离线集成及实际修改需要的最小边界；R05真实状态与R08/R11安装树/证据一起收尾。不安排用户或游戏部署。
4. 自动执行原性能计划Q0–Q6，真实bridge/ASD同条件共同边界，先粗分段后修主差异，原始证据充分保留。
5. R10–R13按原有模块收尾；INT4、DX9/11、Windows Vulkan native明确未接，不混进本轮结果，不因全量盘点而扩平台/模式矩阵。

执行者按本表更新实际修复提交、编译结果和运行结果；不再每次宣称“最后一个消费者”，也不在实现接口后停等用户。

## 五、73文件分类索引

固定文件身份见JSON清单。下列每类已核原版对应、生产可达性、适配必要性及现有证据；未实际运行的类别不会填成通过。

### D3D12诊断

- `windows/src/pe/nr_d3d12_capture.cpp`
- `windows/src/pe/nr_d3d12_capture.hpp`

### D3D12资源适配

- `windows/src/pe/nr_d3d12_asd_backend.cpp`
- `windows/src/pe/nr_d3d12_asd_backend.hpp`

### RenoDX/HDR/预处理

- `windows/shaders/renodx-prep/prep_encode.hlsl`
- `windows/shaders/renodx/v6_encode.cs_5_1.hlsl`
- `windows/src/core/nr_renodx.cpp`
- `windows/src/pe/nr_renodx_d3d12.cpp`
- `windows/src/pe/nr_renodx_d3d12.hpp`

### wilsjo2/fast与转换工具

- `windows/build/build_wilsjo2_d3d12.sh`
- `windows/build/build_wilsjo2_d3d12_test.sh`
- `windows/build/w2_spv_d3d12.py`
- `windows/src/core/nr_wilsjo2.cpp`
- `windows/src/core/nr_wilsjo2.hpp`
- `windows/src/pe/nr_asi_wilsjo2.cpp`
- `windows/src/pe/nr_asi_wilsjo2_d3d12.hpp`
- `windows/src/pe/nr_fast_shaders.hpp`
- `windows/tests/wilsjo2_d3d12/nr_wilsjo2_d3d12_test.cpp`
- `windows/tests/wilsjo2_d3d12/vk_reference.hpp`
- `windows/tools/w2_hlsl_dxbc.cpp`

### 产品入口与路由

- `windows/src/pe/nr_asi_main.cpp`
- `windows/src/pe/nr_dlssnr_model.cpp`
- `windows/src/pe/nr_dlssnr_model.hpp`
- `windows/src/pe/nr_pe_session.cpp`
- `windows/src/pe/nr_pe_session.hpp`

### 完成与提交生命周期

- `windows/src/core/nr_gpu_completion.hpp`
- `windows/src/pe/nr_pe_bridge.hpp`
- `windows/src/pe/nr_pe_d3d12split.cpp`
- `windows/src/pe/nr_pe_d3d12split.hpp`

### 官方ASD交付与注册

- `windows/src/pe/nr_d3d12_asd.cpp`
- `windows/src/pe/nr_d3d12_asd.hpp`
- `windows/src/pe/nr_d3d12_asd_api.h`
- `windows/src/pe/nr_d3d12_asd_canary_blob.h`
- `windows/src/pe/nr_d3d12_asd_compiler_api.h`
- `windows/src/pe/nr_d3d12_asd_reprofile.cpp`
- `windows/src/pe/nr_d3d12_asd_reprofile.hpp`

### 文档与计划

- `docs/D3D12-ASD-EXECUTION-FOCUS.md`
- `docs/D3D12-ASD-PARITY-REVIEW-PLAN.md`
- `docs/D3D12-ASD-PARITY-REVIEW-RESULTS.md`
- `docs/D3D12-ASD-PERFORMANCE-REVIEW-PLAN.md`
- `docs/D3D12-ASD-WIRING-AUDIT.md`
- `docs/M5-D3D12-ASD-SHELL.md`

### 旁支Xbox实验

- `windows/tools/asd-xbox-test/disable-xbox-admin.bat`
- `windows/tools/asd-xbox-test/restore-xbox-admin.bat`
- `windows/tools/asd-xbox-test/verify.ps1`
- `windows/tools/asd-xbox-test/说明.txt`

### 构建安装与依赖

- `.gitattributes`
- `fetch_deps.sh`
- `windows/build/assemble_product_data.sh`
- `windows/build/build_asi.sh`
- `windows/build/build_d3d12_asd_test.sh`
- `windows/build/build_package.sh`
- `windows/build/build_parity_host.sh`
- `windows/package/install.ps1`

### 模型规划与ACO执行

- `windows/src/core/nr_d3d12_net_plan.hpp`
- `windows/src/core/nr_graph.cpp`
- `windows/src/core/nr_runtime.cpp`
- `windows/src/core/nr_runtime.hpp`
- `windows/src/pe/nr_d3d12_asd_net.cpp`
- `windows/src/pe/nr_d3d12_asd_net.hpp`

### 离线宿主与比较证据

- `windows/tests/d3d12_asd/compare_dumps.py`
- `windows/tests/d3d12_asd/dump_manifest.py`
- `windows/tests/d3d12_asd/nr_bridge_host.cpp`
- `windows/tests/d3d12_asd/nr_d3d12_asd_test.cpp`
- `windows/tests/d3d12_asd/nr_parity_host.cpp`
- `windows/tests/d3d12_asd/ticket_log_stub.cpp`
- `windows/tests/d3d12_asd/ticket_state_test.cpp`

### 配置UI与状态

- `windows/src/pe/nr_d3d12_asd_panel.cpp`
- `windows/src/pe/nr_pe_config.cpp`
- `windows/src/pe/nr_pe_menu.cpp`
- `windows/src/pe/nr_pe_menu.hpp`
- `windows/src/pe/reshade_compat/Windows.h`
- `windows/src/ui/nr_control_pages.hpp`
