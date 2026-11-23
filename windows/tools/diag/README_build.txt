Blackgate background-build comparison

Put dlssnr_amd.addon32 in the usual add-on location. Put run_blackgate_build.bat
next to Game.exe and run it from that folder (optional argument: runs per mode,
default 3). Keep Layer1 and NR enabled. Stay at least 15 seconds in the 3D scene
on every run, then exit and answer "black? y/n". Validation layers are not needed.

The runner clears NR_DIAG_STAGE, NR_DIAG_FIX and NR_DIAG_BUILD* between runs.
It tests baseline, late, late-gpu and late-pipelines with normal processing.
Frames pass through while the background build waits or runs. Results append
to logs\results.txt; logs go to logs\build_<mode>_run<R>.log.
Keep [Log] Enabled=1 and ClearOnStart=1. NR normally logs beside the add-on;
if that is not beside Game.exe, set NR_LOG_PATH to that log's full path first.

Check every log's NR_DIAG_BUILD banner. Baseline has no wait lines. Other modes
must show wait begin/end with elapsed seconds from t0 (first DLL_PROCESS_ATTACH
instruction, add-on load). The deadline is t0+8 seconds, NOT 8 seconds after the
build is requested. A build requested after the deadline will not sleep.
late: delay the entire background build, including CPU planning/unpacking.
late-gpu: plan/unpack immediately, delay GPU object creation, allocations,
uploads and submissions. Physical-device capability queries still run early.
late-pipelines: upload weights and generate noise immediately, then delay the
network pipelines and all later build work. Noise generation has its own small
compute pipeline; that intentionally remains before this gate.
Adapters are built after the graph, so they follow its gate too.

For a manual run, unset NR_DIAG_STAGE and NR_DIAG_FIX, then set NR_DIAG_BUILD to
one mode above (unset/empty/invalid = baseline). NR_DIAG_BUILD_DELAY overrides
the deadline in seconds: finite 0..86400, fractions allowed; invalid = 8.
The runner deliberately clears overrides so its comparison always uses 8 s.
This is a diagnostic, not a demonstrated black-screen fix.

Blackgate 后台构建对比

将 dlssnr_amd.addon32 放到原来的插件位置。把 run_blackgate_build.bat 放在
Game.exe 旁，在该目录运行；可选参数为每种模式的次数，默认 3 次。
保持 Layer1 和 NR 开启。每次进入 3D 场景后至少停留 15 秒，退出游戏，再回答
“black? y/n”。本轮不需要验证层。

脚本每次清除 NR_DIAG_STAGE、NR_DIAG_FIX 和 NR_DIAG_BUILD*，依次测试 baseline、
late、late-gpu、late-pipelines，保持正常处理。后台等待或构建期间画面直接通过。
结果追加到 logs\results.txt，日志保存为 logs\build_<模式>_run<次数>.log。
保持 [Log] Enabled=1、ClearOnStart=1。NR 默认在插件旁写日志；若插件不在
Game.exe 旁，先将 NR_LOG_PATH 设为日志完整路径。

检查每份日志的 NR_DIAG_BUILD 横幅。baseline 没有等待记录；其他模式应有
wait begin/end 和从 t0 起算的秒数。t0 为插件 DLL_PROCESS_ATTACH 的第一条指令。
截止时间是 t0+8 秒，而不是构建请求后再等 8 秒；超过截止时间则不再睡眠。
late：延迟整个后台构建，包括 CPU 规划和权重解包。
late-gpu：立即规划和解包，延迟 GPU 对象创建、分配、上传和提交；物理设备查询仍提前运行。
late-pipelines：立即上传权重和生成噪声，然后延迟网络流水线和后续工作。
噪声生成自身的小型计算流水线有意保留在等待之前。适配器在图构建之后创建。

手动测试时清除 NR_DIAG_STAGE 和 NR_DIAG_FIX，设置 NR_DIAG_BUILD；未设、空值或
无效值均为 baseline。NR_DIAG_BUILD_DELAY 可指定 0..86400 秒的有限数值，支持小数，
无效值默认 8 秒。脚本会清除覆盖值，确保每种模式统一使用 8 秒。
本轮仅为诊断，并不代表已证明修复黑屏。
