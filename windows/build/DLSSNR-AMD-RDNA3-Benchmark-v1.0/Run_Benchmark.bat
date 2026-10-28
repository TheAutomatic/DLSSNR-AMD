@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

set "NR_ACO_DIR=%SCRIPT_DIR%aco-bundle"
set "NR_ACO_SHELL_DIR=%SCRIPT_DIR%aco-bundle\shells"
if "%NR_GPU%"=="" set "NR_GPU=0"

set "LOG_FILE=%SCRIPT_DIR%benchmark_results.txt"
echo ============================================================ > "%LOG_FILE%"
echo   DLSS-NR AMD RDNA3 (RX 7000 Series / 700M APU) Benchmark Log >> "%LOG_FILE%"
echo   Date: %DATE% %TIME% >> "%LOG_FILE%"
echo   Directory: %SCRIPT_DIR% >> "%LOG_FILE%"
echo ============================================================ >> "%LOG_FILE%"

echo.
echo ====================================================================
echo   DLSS-NR AMD RDNA3 (RX 7000 系列 / 700M APU) 原生真机基准压测
echo ====================================================================
echo   测试日志将自动写入同级目录的: [benchmark_results.txt]
echo   默认 GPU 索引: %NR_GPU% (如有核显+独显，可通过在系统环境变量设 NR_GPU=1 切换)
echo.
echo   请选择您的显卡类型与测试项目:
echo   [1] 桌面独显推荐: 1080p 压测 (Small + Original 完整模型 各 40 帧)
echo   [2] 桌面中高端独显: 1440p (2K) 压测 (Small + Original 完整模型 各 40 帧)
echo   [3] 桌面旗舰独显: 4K 极限压测 (Small + Original 完整模型 各 40 帧)
echo   [4] APU / 掌机 / 核显推荐 (780M / 760M): 640x360 压测 (各 40 帧)
echo.
set /p CHOICE="请输入数字选项 (直接回车默认运行 [1] 1080p): "

if "%CHOICE%"=="" set CHOICE=1

if "%CHOICE%"=="1" (
    set RES=1920x1080
    set PLAN_S=plan_1920x1080_small.txt
    set PLAN_O=plan_1920x1080_orig.txt
) else if "%CHOICE%"=="2" (
    set RES=2560x1440
    set PLAN_S=plan_2560x1440_small.txt
    set PLAN_O=plan_2560x1440_orig.txt
) else if "%CHOICE%"=="3" (
    set RES=3840x2160
    set PLAN_S=plan_3840x2160_small.txt
    set PLAN_O=plan_3840x2160_orig.txt
) else if "%CHOICE%"=="4" (
    set RES=640x360
    set PLAN_S=plan_640x360_small.txt
    set PLAN_O=plan_640x360_orig.txt
) else (
    echo 输入无效，默认执行 1080p 压测...
    set RES=1920x1080
    set PLAN_S=plan_1920x1080_small.txt
    set PLAN_O=plan_1920x1080_orig.txt
)

echo.
echo ====================================================================
echo   [测试 1/2] 正在运行 %RES% Small 快速模型 (40 帧连续压测)...
echo ====================================================================
echo ------------------------------------------------------------ >> "%LOG_FILE%"
echo [Test 1] %RES% Small Model (40 frames) >> "%LOG_FILE%"
echo ------------------------------------------------------------ >> "%LOG_FILE%"

nr_graph.exe --plan %PLAN_S% --model-pack dlssnr-small.bin --no-reuse --spv-dir aco-bundle\shaders --repeats 40 >> "%LOG_FILE%" 2>&1
if %ERRORLEVEL% EQU 0 (
    echo   ==^> [PASS] Small 模型 40 帧压测通过！
) else (
    echo   ==^> [FAIL] Small 模型压测异常，退出码: %ERRORLEVEL%
)

echo.
echo ====================================================================
echo   [测试 2/2] 正在运行 %RES% Original 完整大模型 (40 帧连续压测)...
echo ====================================================================
echo ------------------------------------------------------------ >> "%LOG_FILE%"
echo [Test 2] %RES% Original Model (40 frames) >> "%LOG_FILE%"
echo ------------------------------------------------------------ >> "%LOG_FILE%"

nr_graph.exe --plan %PLAN_O% --model-pack dlssnr.bin --no-reuse --spv-dir aco-bundle\shaders --repeats 40 >> "%LOG_FILE%" 2>&1
if %ERRORLEVEL% EQU 0 (
    echo   ==^> [PASS] Original 完整模型 40 帧压测通过！
) else (
    echo   ==^> [FAIL] Original 完整模型压测异常，退出码: %ERRORLEVEL%
)

echo.
echo ====================================================================
echo   压测流程已全部结束！
echo   请将同级目录下的 [benchmark_results.txt] 文件（或控制台输出）发回给我们。
echo ====================================================================
echo.
pause
