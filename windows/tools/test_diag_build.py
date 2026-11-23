#!/usr/bin/env python3
"""CPU-only background-build gates and runner regression."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    binary = Path(tmp) / "test_diag_build"
    subprocess.run(shlex.split(os.environ.get("CXX", "g++")) + [
        "-std=c++17", "-Wall", "-Wextra", "-pthread",
        str(ROOT / "windows/tools/test_diag_build.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

graph = (ROOT / "windows/src/core/nr_graph.cpp").read_text()
gpu_gate = graph.index('Mode::late_gpu, "graph-device"')
pipeline_gate = graph.index('Mode::late_pipelines, "network-pipelines"')
assert graph.index('timer.mark("weights-unpack")') < gpu_gate
assert gpu_gate < graph.index("initialize();", gpu_gate) < graph.index("if (!ctx.device) ctx.create();", gpu_gate)
for phase in ("arena-alloc", "input-image", "weights-upload", "noise-field"):
    assert gpu_gate < graph.index(f'timer.mark("{phase}")') < pipeline_gate
assert pipeline_gate < graph.index("create_kernels(spv_dir, g_chain_kern)") < graph.index('timer.mark("arena-zero")')
runtime = (ROOT / "windows/src/core/nr_runtime.cpp").read_text()
assert "s.diagnostic_before_device = create_timing;" in runtime
assert "if (nr::diagbuild::selected(nr::diagbuild::Mode::late_gpu))" in runtime
assert runtime.index("const auto create_timing =") < runtime.index("vkCreateQueryPool(s.ctx.device") < runtime.index("s.diagnostic_before_device = create_timing;")
session = (ROOT / "windows/src/pe/nr_pe_session.cpp").read_text()
worker = session.index("build_thread = std::thread([this, host, config, temporal, access_copy")
assert worker < session.index("nr::diagbuild::Background diagnostic_build;", worker) < session.index('Mode::late, "whole-build"', worker) < session.index("std::make_unique<Runtime>(locked", worker)
assert session.index('Mode::late_gpu, "weights-reload"', worker) < session.index("reload_network(locked", worker)
addon = (ROOT / "windows/src/pe/nr_reshade_addon.cpp").read_text()
assert addon.index("(void)nr::diagbuild::origin();") < addon.index("nr::pe::set_module(module);")
assert "t0=first DLL_PROCESS_ATTACH instruction (add-on load)" in addon
runner = (ROOT / "windows/tools/diag/run_blackgate_build.bat").read_text()
for fragment in ('if not defined runs set "runs=3"', "for %%B in (baseline late late-gpu late-pipelines)",
                 'set "NR_DIAG_STAGE="', 'set "NR_DIAG_FIX="', "set NR_DIAG_BUILD 2^>nul",
                 'if "%1"=="baseline" set "NR_DIAG_BUILD="', 'start "" /wait "%CD%\\Game.exe"',
                 '"logs\\build_%1_run%2.log"', 'choice /c YN /n /m "black? y/n "',
                 '>>"logs\\results.txt"', 'if not exist "Game.exe"', 'if not exist "logs" exit /b 1',
                 '"log_status=MISSING"'):
    assert fragment in runner, fragment
instructions = (ROOT / "windows/tools/diag/README_build.txt").read_text()
assert "15 seconds" in instructions and "15 秒" in instructions
assert "Validation layers are not needed" in instructions and "本轮不需要验证层" in instructions
assert "wait begin/end" in instructions and "NR_LOG_PATH" in instructions
print("PASS: production build gates ordered after unpack/before GPU and after noise/before network pipelines; pre-graph query-pool deferral; background-only whole-build/reload gates; add-on-load origin; runner resets/default/modes/logs/prompt/error paths; bilingual instructions")
