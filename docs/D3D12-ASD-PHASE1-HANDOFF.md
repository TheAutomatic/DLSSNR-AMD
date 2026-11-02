# 阶段一交接(D3D12 / ASD / W2)

本文件是阶段一的审查入口:源码位置、构建与资产身份、已证明的结论、未证明的边界。细节与逐轮记录在
`docs/D3D12-ASD-FULL-ROUTE-AUDIT.md`。

## 源码

- 仓库:`C:\Users\Kenny\Downloads\wt-d3d12-asd-wiring`,分支 `feat/d3d12-asd-wiring`,HEAD 见 `git log -1`。
- 本轮改动集中在:`windows/src/pe/nr_d3d12_asd_backend.cpp`、`nr_d3d12_asd.cpp/.hpp`、`nr_d3d12_asd_net.cpp`、
  `nr_asi_wilsjo2_d3d12.hpp`、`nr_asi_wilsjo2.cpp`、`nr_dlssnr_model.cpp`、`windows/tests/d3d12_asd/*`、
  `windows/tests/wilsjo2_d3d12/nr_wilsjo2_d3d12_test.cpp`、`windows/build/build_package.sh`、`fetch_deps.sh`。

## 构建产物(本机实测,绝对路径)

| 产物 | 路径 | sha256 | 时间 |
|---|---|---|---|
| 产品 ASI(`NR_INT4=1`) | `C:/Users/Kenny/Downloads/wt-d3d12-asd-wiring/artifacts/windows/asi/dlssnr-amd.asi` | `e9025503b94483e29ef3c8c1a1c07dd6553e0a430a93c9acfbe61f390ebbb512` | 21:35 |
| 离线 D3D12 宿主(含 resize/drop 用例) | `C:/Users/Kenny/Downloads/wt-d3d12-asd-wiring/artifacts/windows/d3d12_asd_test/nr_d3d12_asd_test_619.exe` | `f1e7b0710056f48009927dfab5141f2042905d3c54864bad7c75d8c98ad6b29d` | 21:22 |
| Vulkan 参考宿主(含 `NR_PLAN_DUMP`) | `C:/Users/Kenny/Downloads/wt-d3d12-asd-wiring/artifacts/windows/d3d12_asd_test/nr_parity_host.exe` | `ab44d907b6bd3d403bcbaf6a5d773283f008b8f6a3e9ff0dc45ffebc022d5668` | 见文件时间及原始构建日志 |
| W2 离线测试 | `C:/Users/Kenny/Downloads/wt-d3d12-asd-wiring/artifacts/windows/wilsjo2_d3d12_test/nr_wilsjo2_d3d12_test.exe` | `ec760dfe89ae9c75d218bd2ebcdd4610ac89da751735d148f799772d9dca4812` | 见文件时间及原始构建日志 |
| 共享 HDR 解码(RGBA16F) | `C:/Users/Kenny/Downloads/wt-d3d12-asd-wiring/artifacts/windows/jxr-probe/pbz_rgba16f.bin` | `e900a596460d4e88a4b1f88f960da72d2aa6460a6a4842e2655b2a8c036b079b` | — |

构建命令(仓库根;脚本在工作区是 CRLF,必须 `tr -d '\r'` 后执行):

```bash
tr -d '\r' < windows/build/build_asi.sh            > windows/build/.x.sh && NR_INT4=1 bash windows/build/.x.sh
tr -d '\r' < windows/build/build_d3d12_asd_test.sh > windows/build/.x.sh && bash windows/build/.x.sh
tr -d '\r' < windows/build/build_parity_host.sh    > windows/build/.x.sh && bash windows/build/.x.sh
tr -d '\r' < windows/build/build_wilsjo2_d3d12_test.sh > windows/build/.x.sh && bash windows/build/.x.sh
```

## 输入与命令

- HDR 实图输入:`C:\Users\Kenny\OneDrive\视频\Captures\Phantom Blade Zero   2026_10_31 18_16_18.jxr`
  (JPEG XR,3840x2160,coded 64bppRGBAHalf/no colour context/线性 scRGB,RGB −0.180908..7.796875)。
  解码工具 `C:/Users/Kenny/Downloads/wt-d3d12-asd-wiring/artifacts/windows/jxr-probe/jxr_probe.exe`(WIC),f32 → `numpy astype('<f2')` → 上表的共享 half 文件。
- 两侧宿主均以 `NR_TEST_INPUT_HALF=<共享 half 文件>` 上传同一份字节;帧为 RGBA16F、present=scRGB(3)。
- 实图运行脚本:`C:/Users/Kenny/Downloads/wt-d3d12-asd-wiring/artifacts/windows/asd-harness/hdr12.bat`(D3D12)、`hdrvk.bat`(Vulkan 参考);对照报告
  `C:/Users/Kenny/Downloads/wt-d3d12-asd-wiring/artifacts/windows/hdr-compare/REPORT.md`,成图 `view_d3d12.png` / `view_vk.png` / `view_diff.png`。
- 计划导出(任意 extent,纯 CPU):`nr_parity_host.exe <root> W H <dir>` + `NR_PLAN_DUMP=<plan>`,
  日志 `asd-harness/plan-only.txt`(无设备行)。

## 已证明

1. **HDR 实图对照**:同一解码、同一 half 输入、两侧均为产品着色器(参考 SPV 由 `build_renodx.sh` 生成,
   `4990f85070f6acc7…`,`spirv-dis` 有完整算术),线性光域 **PSNR 96.09 dB**,max 0.0060,mean 6.5e-6,
   peak 7.7695(数据峰值)。此前 41.47 dB 与"高光色度钳制"结论因参考目录残留 stage-5 探针而作废,已撤回。
2. **W2 离线测试**:`ALL PASSED, 22 dispatches, 0 failures`;PQ CPU 参考改用与 Vulkan oracle 相同的两段界限
   (线性 1 个 16F 步 + peak/16384;显示光 0.2%·peak_n + 0.05 nits),全样本 `linear bad=0 worst=0.940;
   light bad=0 worst=0.314`,原码值最大差作为诊断保留。
3. **同进程 extent 换代**(`asd-harness/resize-case.txt`):4K 先录后切 1080,旧 stage 按录制集合释放
   (`retired at frame 6, reason key, 3 recording(s) done`),两帧 `Close/Signal/watch 0x0`、device reason 0x0、exit 0。
4. **丢弃录制**(`asd-harness/drop-case.txt`):录而不提 → never_submitted → feature 释放 → 下一帧重建。
5. **NR 关闭 copy-through**(`boundary-nroff.txt`):0 pass、成功返回、`Close 0x0`;面板 `ran` 为 false。
6. 安装树:`C:/Users/Kenny/Downloads/wt-d3d12-asd-wiring/artifacts/windows/pkg-check/`(`dlssnr-amd-asd/` 含 ini/SODB/PSDB + `wilsjo2-d3d12/` 21 个 dxbc;
   `dlssnr-amd/shaders/renodx` 19 个 HLSL;`shaders/wilsjo2` 26 个 SPV),离线宿主从该路径加载通过。

## 未证明 / 限制

- **完整 OptiScaler 发布包未构建**:CI pin 是 `OptiScaler_v10.0.0-pre1_20261030`,本机只有
  `C:\Users\Kenny\Downloads\OptiScaler_v10.0.0-pre1_4.1.2_Vulkan.7z`(20261020 内部版本,55,297,782 B),
  同名不同期;未改名冒充、未联网下载。`build_package.sh` 现在越过 `check_paths/check_imgui`,停在该归档缺失处。
- **注册**:外部普通 PowerShell 已实测 `registration_agility.exe` 两个 variant 均 `RegisterApplication = 0x0`(count 1)、
  `RemoveOwnProbe = 0x0`,`registry_standalone.exe` 在同一真实 HKCU key 上事务成功
  (`registration-probe/external-powershell-registration-success.txt`)。**6801/ERROR_RM_NOT_ACTIVE 只出现在本工具执行
  环境**,不是 Windows/hive/产品逻辑故障。**但这只证明注册调用本身正常,游戏宿主的 SODB/PSDB 注册与 ACO 真实执行
  仍未复核**。
- **跨队列边界**:我们自有代码只建 canary 一条队列;游戏侧多 DIRECT 队列共享同一 `Built` 无复现、未测(R09)。
- **R10**:W2 的输入颜色空间来自 `menu::colour_space_guess`,不是调用者交换链描述;改会变已发布行为,未改。
- Godot 测试环境只做了只读评估(见审计文档 21:45 段),未启动游戏、未移植 Godot。

## 结论口径

当前产物可作为**离线可审查构建**:渲染链、生命周期、W2 程序的数值与边界都有实测日志。不要称 release-ready:
真实宿主下的 ASD 注册+执行、完整 OptiScaler 包、跨队列提交都还没有证据。
