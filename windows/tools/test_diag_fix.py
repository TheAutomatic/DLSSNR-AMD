#!/usr/bin/env python3
"""CPU-only feature/extension legalization and diagnostic integration regression."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    binary = Path(tmp) / "test_diag_fix"
    subprocess.run(shlex.split(os.environ.get("CXX", "g++")) + [
        "-std=c++17", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
        "-I" + str(ROOT / "windows/build/x86"),
        "-I" + str(ROOT / "toolchain/Vulkan-Headers/include"),
        str(ROOT / "windows/tools/test_diag_fix.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

runner = (ROOT / "windows/tools/diag/run_blackgate_fixes.bat").read_text()
assert 'if not defined runs set "runs=3"' in runner
assert "for %%F in (baseline device-nobda device robust all)" in runner
assert 'set "NR_DIAG_STAGE="' in runner
assert 'if "%1"=="baseline" set "NR_DIAG_FIX="' in runner
for fragment in ('start "" /wait "%CD%\\Game.exe"', '"logs\\fix_%1_run%2.log"',
                 'choice /c YN /n /m "black? y/n "', '>>"logs\\results.txt"',
                 'if not exist "Game.exe"', 'if not exist "logs" exit /b 1', '"log_status=MISSING"'):
    assert fragment in runner, fragment
device = (ROOT / "windows/src/pe/nr_pe_vkdevice.cpp").read_text()
assert "instance_versions[*out]" in device and "std::min(a->api, properties.apiVersion)" in device
assert "nr::diagfix::legalize" in device
assert "added ? augmented.info : *info" in device and "*added ? augmented.info : *info" in device
assert device.index("nr::diagfix::remember(*out, caps)") < device.index("if (device_callback) device_callback(*out)")
context = (ROOT / "windows/src/core/nrvk.hpp").read_text()
renodx = (ROOT / "windows/src/core/nr_renodx.cpp").read_text()
session = (ROOT / "windows/src/pe/nr_pe_session.cpp").read_text()
assert "nr::diagfix::pipeline_robustness(prf.pipelineRobustness, device)" in context
assert "nr::diagfix::pipeline_robustness(prf.pipelineRobustness, host.device)" in renodx
assert "!nr::diagfix::capabilities(device).subgroup" in context
assert "if (nr::diagfix::device()) bda = nr::diagfix::capabilities(handles.device).bda" in session
assert "selected %s shader path" in session
validation = (ROOT / "windows/tools/diag/VALIDATION.txt").read_text()
assert 'set "NR_DIAG_FIX=all"' in validation and "06926-06929" in validation
assert "SPIR-V-1.6" in validation and "未知" in validation
print("PASS: fix runner default 3, ordered modes, normal processing, baseline env reset, launch/wait, log/prompt/error paths; API capture/conservative fallback, logical-device integration, core/KHR lookup and bilingual all-mode validation instructions")
