#!/bin/bash
# The ReShade add-on draws its page through ReShade's ImGui, a function table built by MSVC: an ImGui function that
# returns an ImVec2 / ImVec4 by value returns it through a hidden pointer there, where mingw's g++ expects a register -
# called directly from the add-on's code it corrupts the stack and the game crashes later (a combo box clicked, ...).
# The page code calls src/ui/nr_control_pages.hpp's text_size() / room() / cursor_screen_pos() instead. Fails the
# build on a direct call in the code the add-on compiles (src/ui, src/pe/nr_reshade_addon.cpp).
#   check_imgui.sh            (from anywhere)
set -uo pipefail
cd "$(dirname "$0")/../.."
files=$(ls windows/src/ui/*.hpp windows/src/pe/nr_reshade_addon.cpp)
bad=$(grep -nP 'ImGui::(GetWindowPos|GetWindowSize|GetContentRegionAvail|GetContentRegionMax|GetWindowContentRegionMin|GetWindowContentRegionMax|GetCursorPos|GetCursorStartPos|GetCursorScreenPos|GetItemRectMin|GetItemRectMax|GetItemRectSize|CalcTextSize|GetMousePos|GetMousePosOnOpeningCurrentPopup|GetMouseDragDelta|GetFontTexUvWhitePixel|ColorConvertU32ToFloat4)\(' $files \
      | grep -vP '^\S+:\d+:\s*//' | grep -vP '^\S+/nr_control_pages\.hpp:\d+:\s*return ImGui::(CalcTextSize\(t\)|GetContentRegionAvail\(\)|GetCursorScreenPos\(\));')
if [[ -n $bad ]]; then
  echo "check_imgui: an ImGui function returning an ImVec2 / ImVec4 called directly (ReShade's MSVC table; use text_size() / room() / cursor_screen_pos()):" >&2
  echo "$bad" >&2
  exit 1
fi
echo "check_imgui: OK"
