# D3D12 全路线复用、完成度与遗漏总审查

> 最新执行范围（用户2026-11-02决定）：当前只完成阶段一正确性、正常注册和完整离线集成。阶段二性能比较暂不执行，预留Mochi；本文件下文所有“自动进入性能计划/Q0–Q6”的旧指令均撤销。保留原性能计划和已有证据。阶段一完成后整理构建、资产、输入、命令及结果交接，不等待用户操作、不启动正式性能测试。


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


## 16:00阶段一实际增量

双侧真实RGBA32F已跑通，文件均132710400B。审查者独立复核295429个f32数值/bit不同、全部有限，最大绝对差0.0005024075508117676。原554个half差异全部位于f32已不同的相同位置；34个肩部以下差异像素的34个对应通道也已不同。原始交叉统计为artifacts/d3d12-parity-review/build-logs/encode-f32-independent-crosscheck.json。

准确边界为v6_encode内部、SrgbEncode之前（probe位于toe/shoulder之后），不能写成v6_encode之前或排除肩部全部计算；不能仅由f32不同就证明SrgbEncode绝无附加差异。下一步优先取34个无肩部干扰的位置分离实际source/sum/weight/divisor。当前是定位进展，不是554值已经修复。阶段二仍延期。


## 16:45阶段一实际增量

stage2同文件独立复核：全图2501个bit差异；34个目标像素的原差异通道实际也是34个不同，并非代理此前报告的0。样本(293,95) R：ASD sum=0.0001765384222380817（bits960044332），VK=0.00017641922750044614（bits960036141），weight均1。统计在build-logs/encode-stage2-independent.json。必须据此先观测源work0，不再排除采样/求和而归因toe；host尺寸相等也不能替代运行时footprint观测。

a045405在同一shader/selector增加stage5直接Original.Load的源texel观测，以及按需stage6的实际scale/coverage，不增加宿主框架、不改变产品公式。R08比较器已在e0a312c复用原compare_dumps.py支持rgba32f、精确16B像素长度和细微值显示，拒绝未知格式误当half。定向CPU已验证half/f32一ULP变化与未知格式，不启动GPU。探针由执行者串行运行，当前源差异仍未修复。

## 18:02 正常注册阻塞：已定位注册表事务失败

- 旧成功的官方SDK探针原二进制、原app-local619及等待6000ms条件，本轮仍返回E_HANDLE；已有installer可正常枚举/GetExePath，但新增独立应用也失败。
- 独立无GPU探针观察D3D12Core公开API调用：NtCreateTransaction=STATUS_SUCCESS，有效transaction handle；RegCreateKeyTransactedW(app子键)=6801 / ERROR_RM_NOT_ACTIVE，随后RegisterApplication=0x80070006。不是SODB/PSDB加载阶段。
- 不经D3D12，直接CreateTransaction + RegOpenKeyEx(HKCU Software)=成功，RegCreateKeyTransacted(唯一测试键)=6801，未提交事务、关闭回滚后persisted_key=False。执行身份NUC\Kenny，restricted=False、AppContainer=0。
- 因而当前明确阻塞是本用户注册表事务不可用；不修改shader/权重/desc硬凑，不把手写注册表或诊断override冒充正常注册成功。本轮未执行系统事务修复、未清理任何已有installer。
- 原始代码/日志：artifacts/d3d12-parity-review/registration-probe/{registration_trace.cpp,registry_iat_trace.hpp,register-transactions.txt,direct-registry-transaction.txt,execution-token.txt,original-success-probe-held-now.txt}。
- 其他阶段一正确性/离线集成继续；阶段二仍延期。后续系统事务处理需单独确认作用范围，修复后复用同一无GPU注册probe复核。

## 18:15 生命周期与 W2 本轮处置(代码已提交,证据在本文件与提交信息里)

| 项 | 处置 | 证据 |
|---|---|---|
| R01 | 退休证据改为本代未完成 recordings 的 `nr::gpu::Uses` 快照,逐票 `split::completed`(不用 `Uses::ready` 的 64 次/2 秒退让) | `13d5b44`,`95e4cd0`;无 GPU 原类回归 `build-logs/retirement-strict-regression.*` |
| R05 | 面板的 "NR ran" 取 `Stage::record` 的 ran(与 Vulkan 路线 `run_renodx` 同源),copy-through 仍算成功路由 | `13d5b44` |
| R07 | `g.device`/`g.network` 统一 owner 锁,读者取稳定引用;`AcoNetwork` 的 `ready()/status()` 与其 `nets_` 写点同锁 | `13d5b44` |
| R02 | 槽按 `split::book` 的录制归属复用(严格完成),无票不复用;满块换新块,旧块 `end_recording()` 退休;两入口同路径;票据集合去重并按完成清除 | `a5e9df1`,`284c679`,`6012c73` |
| R03 | 计时槽带解析它的录制,完成才取样;忙则不计样本;查询堆随设备换代 | `7d966da` |
| R04 | 换设备的老 stage 按 stage 自身录制集合 + 注册表未完成票据退休,不跨锁 | `7d966da`,`284c679` |
| R08 | 探针抓取行距按纹理格式(不再假定 RGBA32F) | `284c679` |
| R11 | 包内 DXBC 落到 `dlssnr-amd-asd/wilsjo2-d3d12`(=`asd::folder()`),缺工具/预构建集则打包失败而非静默省略 | `1236de0`,`a236a2b` |
| R12 | `make()` 失败路径的 pipeline 初始引用由作用域守卫释放 | `cbd5b66` |
| R13 | GE-Proton 提取只在 d3d9.dll/dxgi.dll/version 齐全时跳过 | `a11f4e7` |
| R06 | 核查:`FreshCanary` 未配置时默认 0,无需改动 | `nr_d3d12_asd.cpp:1077` |

**已验证**:`build_wilsjo2_d3d12_test.sh` 与 `build_asi.sh`(NR_INT4=1)链接;离线 W2 宿主以包内路径
`pkg-check/dlssnr-amd-asd/wilsjo2-d3d12`(21 个 .dxbc)加载,26/26 模式齐全,ALL PASSED,22 dispatches。
**未验证**:槽复用/计时/退休边界的运行期语义仍无 GPU 侧定向案例;`build_package.sh` 在本工作区无法执行,其前置
`check_paths.sh` 既有失败(与本轮改动无关)。
**仍开放**:R09(提交顺序契约)与 R10(W2 借用 NR Control 的颜色空间猜测)不盲改,需要原版 Job/Session 串行证据与
真实交换链描述来源的定向验证。

## 19:10 R09 / R10 证据(代码层,未新增运行)

**R09 提交顺序契约。** 切点的顺序保证在 `nr_pe_d3d12split.hpp:85-97`:`Job::run` 在该队列的
`ExecuteCommandLists` 内、前半段已交给队列之后调用,并规定它必须让队列在 GPU 上等待自己的工作或不做事;因此一个
list 的两段在同一队列上按序执行,`cut()` 的续段继承同一次提交顺序。同一 `Built` 的 act/table/history 被多个录制
共享时,现存完成证据(`Recording` 的封签+各次提交的 fence 回答)只回答"GPU 跑完了没有",不回答"录制之间的先后"。
结论:单队列(list 内切分、同线程续段)的串行由 split 保证;跨队列(两个 DIRECT 队列交替提交同一 Built)没有证据,
也没有复现,属未验证边界,不据此加用户准入限制。定向验证需要两个队列的离线案例,不在本轮。

**R10 颜色空间来源。** W2 的输入空间取自 `nr::pe::menu::colour_space_guess`(`nr_pe_menu.cpp:2063`),调用点
`nr_asi_main.cpp:1732` 与 `:7453`。该函数先取"上一次在任意交换链上设过的空间"(格式相容时),再对 10-bit 交换链
每秒查询一次 `IDXGIOutput6::GetDesc1`。因此 W2 借用的是 NR Control 的猜测链,而不是调用者自己的
`DXGI_SWAP_CHAIN_DESC1`/输出描述。这是有意的语义对齐(同一帧在两条路线上被判成同一空间),不是 D3D12 API 的必要
条件;改为调用者来源会改变已发布行为,需真实交换链案例定向验证,本轮只记录来源,不改。

**PQ CPU 参考定位数据**(现有测试一次运行,`artifacts/windows/wilsjo2_d3d12_test/run-pqworst.txt`):
`PQ worst (79,253) ch0 input 0.154598832 GPU 0.216031283 CPU 7.3095589e-07 CPU-from-GPU-linear 0.21568051
linear [-68.1875 131.375 -10.1484375]`。把 GPU 实际读回的线性图按 CPU 公式编码得到 0.21568,与 GPU 的 0.21603 一致,
即 encode 段一致;偏差来自前段(解码 + 16F 存储)。该点 R 的 PQ 码低、G 接近满值(253/255):负 R 与高 G 是宽色域
饱和绿经 BT.2020->709 的正常结果,不是"近黑放大"。待核方向:PQ pow 的末位微差落到不同 half 后,逆矩阵正负抵消使红
通道跨 0,再被 PQ 非线性放大成 0 对 0.216。容差与着色器均未改动。

## 19:15 包交接(W2 DXBC)

- `build_package.sh` 在打包 `dlssnr-amd-asd/` 时把它需要的 wilsjo2 DXBC 放进 `dlssnr-amd-asd/wilsjo2-d3d12/`
  (运行时 = `asd::folder() + "\wilsjo2-d3d12"`,平铺 `wilsjo2_<name>.dxbc`)。
- 来源二选一:`NR_SPIRV_CROSS`+`NR_W2_DXBC`(Windows 主机现场构建;最后一步需要 d3dcompiler_47)或
  `NR_W2_DXBC_DIR`(预构建集)。两者都没有时打包**失败并说明**,不再产出缺程序的包。
- CI 的 `windows` job 不设置 `NR_ASD_DIR`、`fetch_deps.sh` 也不取 `artifacts/ref/asd-data`,因此 CI 本身不打
  `dlssnr-amd-asd/`(没有该 ini 时该路线本就不启用),本轮改动不影响 CI;受影响的是"带 ASD 目录"的本地/Windows 打包。
- 运行时缺该目录时的既有行为:该路线报 "its D3D12 programs: wilsjo2_encode.dxbc is missing",帧走桥,不崩。

### R09 边界补证(代码层)

- 我们自己的代码不建第二条 DIRECT 队列:W2/ASD 的录制全部进入游戏自己的 list,HUD 抓取也挂在游戏的 list/queue 上
  (`nr_asi_hudcap.cpp:580-616`,`hook_lists` 只 hook 游戏的 `ExecuteCommandLists`)。唯一的自有队列是 canary 用的那条
  (`nr_d3d12_asd.cpp:894`),它只跑 canary 管线、不碰 stage 的 `Built`。
- 因此本轮可证的边界是:同一队列上,一个 list 的切分与续段按 split 的顺序保证执行;跨队列共享同一 `Built` 的情形只能
  由**游戏自己**用多个 DIRECT 队列交错提交造成,我们既无复现也无证据,记录为未验证边界,不据此改变准入。

### 安装树交接(布局即运行时)

`asd::folder()` 取 **ASI 自身所在目录**(`nr_d3d12_asd.cpp:1109-1112`,`GetModuleFileNameW(self)`),`g_folder =
<asi dir>\dlssnr-amd-asd`;RenoDX/网络着色器走 `module_folder()`(同一 ASI 目录)下的 `dlssnr-amd`。因此一个可用的树是:

```
<game>\<asi dir>\dlssnr-amd.asi
<game>\<asi dir>\dlssnr-amd\shaders\{renodx,wilsjo2,rdna4,...}      Vulkan/运行时着色器与 W2 的 SPV
<game>\<asi dir>\dlssnr-amd-asd\dlssnr-amd-asd.ini                  Enabled/Register 等
<game>\<asi dir>\dlssnr-amd-asd\{*.sodb,*.psdb}                    ASD 的 SODB/PSDB
<game>\<asi dir>\dlssnr-amd-asd\wilsjo2-d3d12\wilsjo2_*.dxbc       21 个,平铺
```
无 `dlssnr-amd-asd.ini` 时 `start()` 直接返回(路线关闭,不写日志);有 ini 但缺 `wilsjo2-d3d12/` 时该路线报
"its D3D12 programs: wilsjo2_encode.dxbc is missing" 并走桥。该布局已用 `artifacts/windows/pkg-check` 实测:离线宿主
以 `dlssnr-amd-asd/wilsjo2-d3d12` 为参数,26/26 模式加载,22 dispatches 全通过。

## 19:25 定向收尾数据

**PQ worst 的独立定量**(`artifacts/windows/wilsjo2_d3d12_test/pq-worst-independent.txt`,他人复算):CPU half 线性
[-68.25, 131.375, -10.15625] 对 GPU [-68.1875, 131.375, -10.1484375],R/B 各差 1 个 half 档;逆矩阵红通道 CPU
-0.0006607 scRGB(钳 0)对 GPU +0.0388904 scRGB = 3.11 nits,G 约 9278 nits。0 对 0.216 由此产生:宽色域逆矩阵的抵消
把 1 half 档放大到跨 0,再经 PQ 非线性放大。**不是解码坏了**;原码值差保留为诊断,不放宽码值容差凑 PASS,改用与
VK 参考相同的判据(光域、按像素峰值相对误差)或把半精度/回编码分成两段判定。全样本尚未证明。

**extent 变化(不匹配的 plan)**:`renodx 1920 1080` 3 帧(`asd-harness/boundary-1080.txt`)——网络计划只为
3840x2160 构建,路线每次都报 "plan.bin is for 3840x2160" 并原样放过(host exit=7),没有崩溃、没有录制半帧、没有
退回 Vulkan 桥。即 extent 与计划不匹配时的边界行为是明确的拒绝,不是隐式跑错网络。

## 19:40 收尾:全样本判据与 resize 代价

**W2 PQ CPU 参考**:全样本诊断 `CPU full-sample existing-VK bounds: linear bad=0 worst=0.94025825; light bad=0
worst=0.314148121`(`wilsjo2_d3d12_test/run-fullsample.txt`,并保留 `run-pqworst.txt` 的原码值诊断)。按此把该断言
改为 VK oracle 已有的两段界限(线性 1 个 16F 步 + peak/16384;显示光 0.2%·peak_n + 0.05 nits),原码值最大值继续打印
作为诊断。复跑:`ALL PASSED: 22 dispatches, 0 failure(s)`(`run-cpucriterion.txt`)。未放宽为"凑 PASS":界限与 VK 参考
逐字相同,且全样本 0 越界。

**resize/换代边界的实际代价**:离线宿主只有 3840x2160 的 `plan.bin`;计划由运行时的 lowering 在真实安装里导出
(`nr_d3d12_asd.cpp:1119-1124` 读 ini 的 `[D3D12ASD] PlanDump`,`nr_runtime.cpp:3390-3393` 写 NRPLAN02),离线没有
可用的 lowering 入口,且它依赖当前被注册表事务故障阻塞的路径。因此一次**真** resize(新 extent + 匹配计划)本轮无法
构造;已测到的是不匹配计划下的边界行为:明确拒绝、不录半帧、不退回桥(`boundary-1080.txt`)。更换 extent 时 stage 会
按 key 重建、旧 stage 进入 `renodx_retired` 并按其录制票据释放——这段代码路径由 `-Wall` 全量编译覆盖,但没有 GPU 侧
定向运行。
