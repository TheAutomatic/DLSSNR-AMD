# 执行收敛指令（用户要求立刻修正，2026-11-02）

> 最新执行范围（用户2026-11-02决定）：当前只完成阶段一正确性、正常注册和完整离线集成。阶段二性能比较暂不执行，预留Mochi；本文件下文所有“自动进入性能计划/Q0–Q6”的旧指令均撤销。保留原性能计划和已有证据。阶段一完成后整理构建、资产、输入、命令及结果交接，不等待用户操作、不启动正式性能测试。


本文调整两份计划的执行范围与优先级，不重启既有调查，不抹去已发现缺陷或证据。

1. 当前生命周期修改先形成可审查 checkpoint。只补当前已证实的资源提前释放、描述符在飞覆盖所需的最小录制 owner/完成管理和旧 device 保留。复用现有设施，不将其扩成通用框架，不新增并列的计数环、allocator 或诊断系统。
2. 回到目标案例：4K HDR10、现有0.0.8.1模型、完整生产入口、现有游戏对象由离线宿主提供。网络同输入重放已逐位一致，复用该证据；直接定位实际 v6 编码路径的输入末位差，不再重查已一致的机器码、权重、噪声和整个71层。
3. 完成目标案例的完整离线集成：输入→编码→ACO模型→输出，必要的连续帧与已发现生命周期边界。正常程序化注册 E_HANDLE 仍需解决；诊断 override 成功不能替代产品注册成功。
4. 阶段一结束后整理交接；阶段二真实bridge/ASD性能对照由Mochi后续接手，本轮不执行。

停止扩展：所有HDR/SDR模式矩阵、DX9/11、无关平台、通用生命周期框架、重复验证相同三帧、只增说明或接口却不接实际路径。测试仅围绕具体缺陷或可区分的候选原因。不得以收敛为由忽略已确认的在飞资源问题，也不得用未修复版本做正式性能结论。

若某依赖确实阻塞，记录精确代码位置和最短处理方案，同时继续不依赖它的当前 codec/注册工作。完成一项立即继续下一项，不等待阶段确认或定时监督。每阶段状态必须区分已实现、实际验证、未完成。


## 全路线统一审查入口（2026-11-02）

已完成相对原版73文件的全分类核查，含已做/未做、必要适配与重复遗漏：`C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring\docs\D3D12-ASD-FULL-ROUTE-AUDIT.md`。以模块表、R01–R13和已完成证据作为统一清单；旧局部结论不代表全路线完成。沿原P0–P5/Q0–Q6和当前codec目标继续，RenoDX/W2生产可达分支一起核实际使用/退休；不重复权重/71层，不造框架，不扩无关平台，不交给用户操作。


## 执行重心纠正（用户质疑耗时，2026-11-02）

最近过度串行追逐HDR微差和probe，注册与确认的生产缺陷未同步推进，执行重心偏离。当前分工：主会话负责正常程序化注册首次失败定位；omp直接处理R01/R05/R07及生产可达W2必要修正。HDR源纹理一half ULP差异与探针证据保留，继续按实际需要分析，但不作为注册或其他独立工作先决条件，不增设逐位零差门槛、不改数学硬凑参考。阶段二继续延期。

本轮真实无GPU注册定位：独立用户级installer身份，CoInitializeEx、factory、CreateInstaller及GetPrecompileTargets均S_OK（3目标）；builtin/实际exe两种desc的RegisterApplication均E_HANDLE，应用数始终0。尚未传入SODB/PSDB，因而此错误不在ACO对象加载阶段。原始日志与最小程序在artifacts/d3d12-parity-review/registration-probe/。不凭此直接宣称Windows或驱动故障；下一步针对该调用的注册后端定位，不重查shader/权重。自动审批拒绝的清理初始化验证没有执行，已采用不清理状态的目标枚举替代。

## 18:02 正常注册阻塞：已定位注册表事务失败

- 旧成功的官方SDK探针原二进制、原app-local619及等待6000ms条件，本轮仍返回E_HANDLE；已有installer可正常枚举/GetExePath，但新增独立应用也失败。
- 独立无GPU探针观察D3D12Core公开API调用：NtCreateTransaction=STATUS_SUCCESS，有效transaction handle；RegCreateKeyTransactedW(app子键)=6801 / ERROR_RM_NOT_ACTIVE，随后RegisterApplication=0x80070006。不是SODB/PSDB加载阶段。
- 不经D3D12，直接CreateTransaction + RegOpenKeyEx(HKCU Software)=成功，RegCreateKeyTransacted(唯一测试键)=6801，未提交事务、关闭回滚后persisted_key=False。执行身份NUC\Kenny，restricted=False、AppContainer=0。
- 因而当前明确阻塞是本用户注册表事务不可用；不修改shader/权重/desc硬凑，不把手写注册表或诊断override冒充正常注册成功。本轮未执行系统事务修复、未清理任何已有installer。
- 原始代码/日志：artifacts/d3d12-parity-review/registration-probe/{registration_trace.cpp,registry_iat_trace.hpp,register-transactions.txt,direct-registry-transaction.txt,execution-token.txt,original-success-probe-held-now.txt}。
- 其他阶段一正确性/离线集成继续；阶段二仍延期。后续系统事务处理需单独确认作用范围，修复后复用同一无GPU注册probe复核。
