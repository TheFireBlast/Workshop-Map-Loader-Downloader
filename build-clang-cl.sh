#!/usr/bin/env bash
# Cross-compile the Release x64 plugin DLL on Linux with clang-cl + lld-link.
#
# Needs: clang, lld, and the MSVC CRT + Windows SDK from xwin:
#   cargo install --locked xwin
#   xwin --accept-license --arch x86_64 splat --output ~/.cache/xwin
#
# Env: XWIN (default ~/.cache/xwin), BAKKESMOD_SDK (folder with include/ and lib/pluginsdk.lib,
#      e.g. a clone of https://github.com/bakkesmodorg/BakkesModSDK), JOBS.
# Output: plugins/WorkshopMapLoader.dll
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/Pluginx64"
XWIN="${XWIN:-$HOME/.cache/xwin}"
BAKKESMOD_SDK="${BAKKESMOD_SDK:?set BAKKESMOD_SDK to the BakkesMod SDK folder}"
SDK_CASE="$ROOT/build/sdk-case"
rm -rf "$SDK_CASE"
python3 -I "$ROOT/tools/case-shadow.py" "$BAKKESMOD_SDK/include" "$SDK_CASE" "$SRC"
OBJ="$ROOT/build/clang-cl"
OUT="$ROOT/plugins"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "$OBJ" "$OUT"

CFLAGS=(
  --target=x86_64-pc-windows-msvc -fuse-ld=lld
  /c /nologo /std:c++20 /O2 /MT /EHsc /GS /Gy /Oi /W3 /wd4996
  /utf-8 /Zc:char8_t-
  -Wno-microsoft -Wno-c++11-narrowing -Wno-unused-command-line-argument -Wno-unknown-pragmas -Wno-deprecated-declarations
  /DNDEBUG /DBAKKESMODPLUGINHTTPREQUEST_EXPORTS /D_WINDOWS /D_USRDLL /DUNICODE /D_UNICODE
  /I"$SRC" /I"$SRC/LibrariesUsed/include" /I"$SDK_CASE"
  /imsvc"$XWIN/crt/include"
  /imsvc"$XWIN/sdk/include/ucrt" /imsvc"$XWIN/sdk/include/um"
  /imsvc"$XWIN/sdk/include/shared" /imsvc"$XWIN/sdk/include/winrt"
)

SOURCES=(
  BakkesPluginsMapInstaller.cpp PluginTexts.cpp PluginConfig.cpp Utils.cpp FileExplorer.cpp
  IMGUI/imgui.cpp IMGUI/imguivariouscontrols.cpp IMGUI/imgui_additions.cpp IMGUI/imgui_demo.cpp
  IMGUI/imgui_draw.cpp IMGUI/imgui_impl_dx11.cpp IMGUI/imgui_impl_win32.cpp IMGUI/imgui_rangeslider.cpp
  IMGUI/imgui_searchablecombo.cpp IMGUI/imgui_timeline.cpp IMGUI/imgui_widgets.cpp
  pch.cpp WorkshopMapLoader.cpp WorkshopMapLoaderGUI.cpp WorkshopMapLoaderSettingsGUI.cpp
)

compile() {
  local src="$1" obj="$OBJ/$(echo "$1" | tr '/' '_').obj"
  local extra=()
  # imgui_rangeslider.cpp calls templates (SliderCalcRatioFromValueT, RoundScalarWithFormatT) that are only
  # defined in imgui_widgets.cpp; MSVC leaves out-of-line copies there, clang inlines them all away.
  [[ "$src" == IMGUI/imgui_widgets.cpp ]] && extra=(/Ob0)
  echo "compile $src"
  clang-cl "${CFLAGS[@]}" "${extra[@]}" "/Fo$obj" "$SRC/$src"
}
export -f compile
export SRC OBJ
printf '%s\n' "${SOURCES[@]}" | CFLAGS_DECL="$(declare -p CFLAGS)" \
  xargs -P "$JOBS" -I{} bash -c 'eval "$CFLAGS_DECL"; compile "$1"' _ {}

echo "link"
lld-link /nologo /DLL /MACHINE:X64 /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF \
  "/OUT:$OUT/WorkshopMapLoader.dll" \
  "/LIBPATH:$XWIN/crt/lib/x86_64" "/LIBPATH:$XWIN/sdk/lib/um/x86_64" "/LIBPATH:$XWIN/sdk/lib/ucrt/x86_64" \
  "/LIBPATH:$SRC/LibrariesUsed/lib" "/LIBPATH:$BAKKESMOD_SDK/lib" \
  "$OBJ"/*.obj \
  pluginsdk.lib zlib.lib libcurl.lib cpr.lib jsoncpp.lib ws2_32.lib crypt32.lib wldap32.lib urlmon.lib \
  advapi32.lib user32.lib d3d11.lib d3dcompiler.lib dxgi.lib normaliz.lib shell32.lib ole32.lib
echo "built $OUT/WorkshopMapLoader.dll"
