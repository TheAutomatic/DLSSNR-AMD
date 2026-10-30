#!/bin/bash
# Paths in NR's Windows code are UTF-8 (src/core/nr_path_u8.hpp says why): no call that reads a narrow path in the ANSI
# code page - fopen, the "A" file functions, CP_ACP, std::remove / std::rename, a file stream opened with a narrow string. Fails the build on one.
#   check_paths.sh            (from anywhere; checks windows/src/pe, windows/src/ui, windows/src/core)
set -uo pipefail
cd "$(dirname "$0")/../.."
files=$(ls windows/src/pe/*.cpp windows/src/pe/*.hpp windows/src/pe/*.inl windows/src/ui/*.hpp windows/src/core/*.cpp \
           windows/src/core/*.hpp 2>/dev/null | grep -v -e nr_path_u8.hpp -e nr_path_win.hpp)
bad=$(grep -nP '(?<![:\w])fopen\(|\bstd::fopen\(|\b(GetModuleFileNameA|GetFileAttributesA|GetFileAttributesExA|CopyFileA|CreateDirectoryA|CreateFileA|FindFirstFileA|FindNextFileA|GetPrivateProfileStringA|GetPrivateProfileIntA|WritePrivateProfileStringA|DeleteFileA|MoveFileA|MoveFileExA|LoadLibraryA|LoadLibraryExA|ShellExecuteA|GetFullPathNameA|RemoveDirectoryA)\(|\bCP_ACP\b|\bstd::(remove|rename)\([^)]*c_str\(\)|std::[io]?fstream(\s+\w+)?\s*[({](?!\s*[)}])(?!\s*(nr::u8::path|std::filesystem::path)\()' $files \
      | grep -vP '^\S+:\d+:\s*//')
if [[ -n $bad ]]; then
  echo "check_paths: narrow paths in the ANSI code page (use nr::u8::, src/core/nr_path_win.hpp):" >&2
  echo "$bad" >&2
  exit 1
fi
echo "check_paths: OK"
