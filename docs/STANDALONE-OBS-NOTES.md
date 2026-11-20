# 独立模式 / OBS 调试与只读目录 `dlssnr-amd.ini` 修复说明

本文档记录在非管理员权限下于只读安装目录（`C:\Program Files\obs-studio\bin\64bit\obs64.exe`）运行 `dlssnr-amd.asi`（Standalone + `dxgi-standalone.dll`）时的已完成修复、只读 `dlssnr-amd.ini` 问题定位与修复完成情况。

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

## 二、已定位并修复的问题：只读目录下在 NR Control 切模型不生效

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

## 三、修复实现与验证状态

已将全链路 `dlssnr-amd.ini` 读写及辅助诊断输出统一路由至 `nr::u8::writable_ini_path()` / `nr::u8::writable_app_dir()`，确保在只读安装目录下首次从安装目录播种复制后保留用户编辑、不覆写既有设置，并在正常可写目录保持原有行为不变。

### 完整修改清单
1. `windows/src/core/nr_path_win.hpp`：新增 `clear_readonly()`、`seed_writable_ini()`、`writable_ini_path()`（线程安全缓存、只读回退 `%LOCALAPPDATA%\DLSSNR-AMD\apps\<tag>\dlssnr-amd.ini`、首次按 `dlssnr-amd.ini` → `dlssnr-amd\dlssnr-amd.default.ini` 顺序播种并清除 `FILE_ATTRIBUTE_READONLY`）。
2. `windows/src/ui/nr_ini_edit.hpp`：加固 `Ini::save()`（自动创建父目录、检查流写入状态并区分写 `.nrcontrol` 临时文件与 `MoveFileExW` 失败日志、失败时清理临时文件）。
3. `windows/src/core/nr_runtime.cpp`：新增 `writable_ini_file()`，替换全部 5 处 `dlssnr-amd.ini` 路径读取。
4. `windows/src/pe/nr_pe_log.cpp`：`read_settings()` 改用 `nr::u8::writable_ini_path()`。
5. `windows/src/pe/nr_pe_config.cpp`：`gpu_manual()`、`restore_ini()`、`apply_debug_env()`、`PreprocessFile::poll()` 统一改用 `nr::u8::writable_ini_path()` 并清除复制后的只读属性。
6. `windows/src/pe/nr_pe_control.cpp`：`read_pass_key()` 改用 `nr::u8::writable_ini_path()`。
7. `windows/src/pe/nr_dlssnr_model.cpp`：`preprocess_now()` 与 `renodx_now()` 改用 `nr::u8::writable_ini_path()`。
8. `windows/src/pe/nr_pe_session.cpp`：`wanted_network()` 与 `carry()`（`"resize history"`）改用 `nr::u8::writable_ini_path()`。
9. `windows/src/pe/nr_asi_main.cpp`：`chain_now()`、`hud_capture_wanted_at_start()` 改用 `nr::u8::writable_ini_path()`，`InitializeASI()` 在回退时输出重定向日志。
10. `windows/src/pe/nr_d3d12_passthrough.cpp`：`start()` 的 `g_ini` 改用 `nr::u8::writable_ini_path()`。
11. `windows/src/pe/nr_pe_optifix.cpp`：`find_locks` 线程与 `fix_optiscaler()` 改用 `nr::u8::writable_ini_path()`。
12. `windows/src/pe/nr_pe_menu.cpp`：`Menu::ini`、`marks_chain_file.poll`、`menu_key()`、`copy_report()`、`diag_request()`、`report_every`、`install_now()` 及 `nr-report-series.txt` 统一改用 `nr::u8::writable_ini_path()` / `nr::u8::writable_app_dir()`，移除未使用的 `game_dir()`。
13. `windows/src/pe/nr_pe_diag.hpp`：诊断 zip 输出至 `writable_app_dir()`，优先打包活动的 `writable_ini_path()`（回退时跳过安装目录旧 `dlssnr-amd.ini`）、`nr-report.txt` 与可写目录下的截图。
14. `windows/src/pe/nr_reshade_addon.cpp`：`DllMain()`、`draw_page()`、`page_test_writes()` 与 `Copy a report` 统一改用 `nr::u8::writable_ini_path()` / `nr::u8::writable_app_dir()`。
15. `windows/tools/test_d3d12_direct_r33.cpp`：扩展 Case I，验证可写/只读目录 `writable_ini_path` 行为、只读源 `seed_writable_ini` 清除 `FILE_ATTRIBUTE_READONLY`、`Ini::save()` 持久化、不覆盖用户已编辑模型、以及 `dlssnr-amd.default.ini` 回退播种。

### 已完成的本地验证
- `bash windows/build/check_paths.sh` → `check_paths: OK`
- `bash windows/build/check_imgui.sh` → `check_imgui: OK`
- `NR_TOOLS=1 bash windows/build/build_asi.sh artifacts/windows/asi` → 退出码 0（`dlssnr-amd.asi` 12,674,874 bytes）
- `x86_64-w64-mingw32-objdump -p artifacts/windows/asi/dlssnr-amd.asi` → PE 导入表严格保持为 `ADVAPI32.dll`、`KERNEL32.dll`、`msvcrt.dll`、`ole32.dll`、`SHELL32.dll`、`USER32.dll`、`VERSION.dll`（无静态 `vulkan-1.dll` / `d3dcompiler_47.dll`）
- OptiScaler / ReShade 路由编译检查：`nr_pe_optifix.cpp`（`optifix-OK`）、`nr_pe_menu.cpp`（`opti-menu-OK`）、`nr_reshade_addon.cpp`（`reshade-addon-OK`）均通过
- `artifacts/bench-pkg/test_d3d12_direct_r33.exe`（同步自 `artifacts/windows/asi/`）→ 退出码 0，Cases `A`、`B`、`B2`、`D01`（RDNA4 & RDNA3）、`C`、`D`、`E`、`F`、`F2`、`G`、`H`、`I`、`J`、`K` 全部 `[PASS]`
- **待用户实机确认**：将新编译的 `artifacts/windows/asi/dlssnr-amd.asi` 覆盖到 `C:\Program Files\obs-studio\bin\64bit\` 后，在 OBS 中于 `NR Control` 切换模型，确认日志不再出现 `could not save dlssnr-amd.ini` 且后台触发目标模型构建并切换生效。
