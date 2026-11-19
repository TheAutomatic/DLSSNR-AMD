# 独立模式 / OBS 调试与只读目录 `dlssnr-amd.ini` 故障排查说明（WIP）

本文档记录在非管理员权限下于只读安装目录（`C:\Program Files\obs-studio\bin\64bit\obs64.exe`）运行 `dlssnr-amd.asi`（Standalone + `dxgi-standalone.dll`）时的已完成修复、当前新定位到的问题，以及尚未改完的 WIP 状态。

---

## 一、已完成并实机验证的改进（OBS 独立模式）

以下能力已合入当前分支代码并在 OBS 实机日志与 `test_d3d12_direct_r33.exe`（Cases A–K）中验证通过：

1. **只读安装目录回退至 `%LOCALAPPDATA%\DLSSNR-AMD\`（日志、缓存、截图、报告）**
   - 实现于 `windows/src/core/nr_path_win.hpp`（`writable_cache_root` / `writable_app_dir`）及 `nr_pe_log.cpp`、`nr_pe_config.cpp`、`nr_pe_menu.cpp`、`nr_asi_main.cpp`、`nr_runtime.cpp`。
   - OBS 无管理员权限运行时，日志、ACO/Vulkan 缓存、`NR Screenshots`、`nr-report.txt` 可正常落盘到 `C:\Users\Kenny\AppData\Local\DLSSNR-AMD\apps\obs64_7ab1ae36\`。

2. **无引导 D3D11 Present → D3D12 ACO 直通桥接**
   - 实现于 `windows/src/pe/nr_d3d11_d3d12_bridge.hpp`、`windows/src/pe/nr_d3d11_d3d12_bridge.cpp` 及 `nr_dlssnr_model.cpp` / `nr_asi_main.cpp`。
   - 独立模式下的 D3D11 Present 应用（如 OBS、PotPlayer 等）不再被迫走 Vulkan Bridge，可通过 NT shared texture + shared fence 同步桥接直接在 Zero-Heap D3D12 ACO ELF 管线上运行 NR。

3. **AMD 1-level 间接描述符句柄（Indirect Descriptor Handle）支持**
   - 实现于 `windows/src/pe/nr_d3d12_outer_elf.hpp`（`OuterSlot::srd_data` / `OuterHeapPool::srd_data_` / `verify_srd_layout`）及 `nr_d3d12_passthrough_backend.cpp`。
   - 适配 `amdxc64.dll` 间接描述符模式（`SRV16F` / `UAV16F` / `SRV32D` 指针解引用校验），已清除所有临时诊断逻辑，零每帧额外开销；OBS 日志实测输出 `[d3d12] outer SRD staging: indirect descriptor handle mode (1-level deref verified)`。

4. **多 SwapChain 主目标仲裁（消除 OBS 双窗口逐帧重建抖动）**
   - 实现于 `windows/src/pe/nr_swapchain_arbiter.hpp`、`windows/src/pe/nr_pe_menu.cpp`（`is_primary_swapchain` / `draw_on`）及 `nr_asi_main.cpp`。
   - 解决了 OBS 主预览（`2412x1128`）与 4K 全屏投影（`3840x2160`）每帧交替触发 `feature` 与 RenoDX Stage 销毁/重建的问题；实测 4K 全屏投影稳定运行（`12.2 ms` GPU / `0.11 ms` CPU），主预览稳定在 `4.44 ms` GPU / `0.106 ms` CPU。

---

## 二、已定位但尚未改完的问题：只读目录下在 NR Control 切模型不生效

### 1. 问题现象
在 OBS 中打开 `NR Control` 切换模型（例如从 `CC_SMALL_FP8_EXPERIMENTAL` 切换为其它模型）时，界面下拉框看似已经改变，但实际推理仍然一直在跑之前的 `small` 模型。

### 2. 日志证据
来自实机日志 `C:\Users\Kenny\AppData\Local\DLSSNR-AMD\apps\obs64_7ab1ae36\dlssnr-amd.log` 第 111 与 153 行：

```text
[nr] NR Control: could not save dlssnr-amd.ini (error 2)
```

### 3. 根因分析
- 此前只把 **日志、缓存、截图、报告** 重定向到了 `%LOCALAPPDATA%\DLSSNR-AMD\apps\<tag>\`，但 **`dlssnr-amd.ini` 仍然指向只读的模块目录**（`C:\Program Files\obs-studio\bin\64bit\dlssnr-amd.ini`）。
- 当用户在 `NR Control` 切模型时，`nr::ini_edit::Ini::set(...)` 修改了内存中的配置行（因此 UI 界面看起来切了），随后 `nr::ini_edit::Ini::save()` 尝试创建临时文件 `dlssnr-amd.ini.nrcontrol` 并调用 `MoveFileExW`，因目录不可写而失败（`error 2`），磁盘文件未更新。
- 底层 `NetworkSwitch::frame()`（`nr_pe_session.cpp` → `nr_pe_config.cpp`）持续从磁盘读取未变化的旧 `dlssnr-amd.ini`，导致模型并未真正切换。

### 4. 模型热切换链路确认
已排查 `nr_dlssnr_model.cpp::evaluate_d3d12` → `d3d12_session().frame_network()` → `nr_d3d12_passthrough_net.cpp::ready()` 的后台构建与 `waiting = 1` 直通逻辑：D3D12 直通路径热切模型链路本身已完整接通，只要将所有 `dlssnr-amd.ini` 读写路径统一到可写目录并保证首次从安装目录播种拷贝，即可生效。

---

## 三、当前分支 WIP 改动进度（尚未改完，未重新编译与验证）

本次暂停时，只读 `dlssnr-amd.ini` 重定向方案**仅完成了前半部分修改**，因此当前提交为 **WIP（未完成、未重新编译、未跑验证）**：

### 已修改部分
- `windows/src/core/nr_path_win.hpp`：新增 `clear_readonly()`、`seed_writable_ini()`、`writable_ini_path()`。
- `windows/src/ui/nr_ini_edit.hpp`：加固 `Ini::save()`（确保父目录存在、检查流写入状态、区分写临时文件与 `MoveFileExW` 错误并清理残留 `.nrcontrol`）。
- `windows/src/core/nr_runtime.cpp`：新增 `writable_ini_file()`，已替换全部 5 处 `dlssnr-amd.ini` 路径构造。
- `windows/src/pe/nr_pe_log.cpp`：`read_settings()` 已改用 `nr::u8::writable_ini_path()`。
- `windows/src/pe/nr_pe_config.cpp`：`gpu_manual()`、`restore_ini()`、`apply_debug_env()`、`PreprocessFile::poll()` 已改用 `nr::u8::writable_ini_path()` 并清除复制后的只读属性。
- `windows/src/pe/nr_pe_control.cpp`：`read_pass_key()` 已改用 `nr::u8::writable_ini_path()`。
- `windows/src/pe/nr_dlssnr_model.cpp`：`preprocess_now()` 与 `renodx_now()` 已改用 `nr::u8::writable_ini_path()`。
- **仅局部修改的文件**：
  - `windows/src/pe/nr_pe_session.cpp`：已改 `wanted_network()`；`carry()` 中的 `resize history` 尚未改。
  - `windows/src/pe/nr_asi_main.cpp`：已改 `chain_now()`；`hud_capture_wanted_at_start()` 与 `InitializeASI()` 尚未改。
  - `windows/src/pe/nr_reshade_addon.cpp`：已改 `draw_page()` 的 `Ini` 构造；`DllMain()` 与 `page_test_writes()` 尚未改。
  - `windows/src/pe/nr_pe_diag.hpp`、`windows/src/pe/nr_d3d12_passthrough.cpp`、`windows/src/pe/nr_pe_optifix.cpp`：目前仅添加了 `#include "nr_path_win.hpp"`，调用点尚未替换。

### 待完成清单
1. `windows/src/pe/nr_pe_menu.cpp`：将 `Menu::ini`、`marks_chain_file.poll`、`menu_key()`、`copy_report()`、`diag_request()`、`report_every`、`install_now()` 及 `nr-report-series.txt` 统一改用 `writable_ini_path()` / `writable_app_dir()`。
2. `windows/src/pe/nr_pe_diag.hpp`：诊断包输出目录改用 `writable_app_dir()`，并优先打包活动的 `writable_ini_path()`（回退时跳过安装目录旧 `dlssnr-amd.ini`）、`nr-report.txt` 与可写目录下的截图。
3. `windows/src/pe/nr_asi_main.cpp`：更新 `hud_capture_wanted_at_start()`，并在 `InitializeASI()` 打印 `dlssnr-amd.ini` 重定向提示日志。
4. `windows/src/pe/nr_pe_session.cpp`：更新 `carry()` 中的 `"resize history"` 读取路径。
5. `windows/src/pe/nr_d3d12_passthrough.cpp`：更新 `start()` 中的 `g_ini = nr::u8::writable_ini_path(...)`。
6. `windows/src/pe/nr_pe_optifix.cpp`：更新 `find_locks` 线程与 `fix_optiscaler()` 中的 `dlssnr-amd.ini` 路径。
7. `windows/src/pe/nr_reshade_addon.cpp`：更新 `DllMain()` 的 `config_path` 与 `page_test_writes()`。
8. `windows/tools/test_d3d12_direct_r33.cpp`：在 Case I 增加 `writable_ini_path` / `seed_writable_ini` / `Ini::save()` 持久化与不覆盖用户编辑的回归测试。
9. **完整编译与验证**：运行 `check_paths.sh`、`NR_TOOLS=1 bash windows/build/build_asi.sh artifacts/windows/asi`、OptiScaler/ReShade 相关编译检查、PE 导入表检查、`test_d3d12_direct_r33.exe`（Cases A–K），并在 OBS 只读目录下实测切换模型。
