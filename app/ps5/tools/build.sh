#!/usr/bin/env bash
# The Darkness on PS5 - build, link and sign the title.
#
#   app/ps5/tools/build.sh [--clean]
#
# Needs the PS5 payload SDK (PS5_SDK, default $HOME/ps5-sdk) with its toolchain,
# and the native tool that turns the link result into the console's signed
# eboot.bin (PS5_NATIVE_TOOL, default the PS5_Vulkan checkout's build).
#
# Result: app/ps5/build/dist/PPSA99301/ - eboot.bin, sce_sys/param.json. Copy
# that folder to the console; the game dump goes in its Darkness/ folder.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
app=$(dirname "$here")
root=$(cd -- "$app/../.." && pwd)
sdk=${PS5_SDK:-$HOME/ps5-sdk}
vulkan=${PS5_VULKAN_DIR:-/mnt/c/kb/PS5_Vulkan}
tool=${PS5_NATIVE_TOOL:-$vulkan/build/host/ps5-native-tool}
build=$app/build
generated=$root/build_native/generated

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

[[ -f $sdk/toolchain/prospero.cmake ]] || fail "no PS5 SDK at $sdk (set PS5_SDK)"
[[ -x $tool ]] || fail "no PS5 native tool at $tool (set PS5_NATIVE_TOOL)"
[[ -f $generated/sources.cmake ]] ||
    fail "no AOT output at $generated
  Generate it first: python tools/recompile.py --output $generated --verify"

if [[ ${1:-} == --clean ]]; then
    rm -rf "$build"
fi
mkdir -p "$build"

# RADV imports AGC, which the public SDK ships no stubs for. Small modules that
# name those imports stand in, as PS5_Vulkan's own title links them (the same
# sources, and they must be named the way RADV's DT_NEEDED expects).
agc_stubs=$build/stubs
mkdir -p "$agc_stubs"
for module in libSceAgc:agc_canary_link_stub libSceAgcDriver:agc_driver_canary_link_stub; do
    name=${module%%:*}
    source=$vulkan/vendor/ps5/sdk/stubs/${module##*:}.c
    [[ -f $source ]] || fail "no AGC stub source at $source"
    [[ -f $agc_stubs/$name.so ]] && continue
    printf '==> %s link stub\n' "$name"
    PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" -std=c11 -O2 -fPIC -c "$source" \
        -o "$agc_stubs/$name.o"
    "$sdk/bin/prospero-lld" --shared -soname "$name.prx" -o "$agc_stubs/$name.so" "$agc_stubs/$name.o"
done

cmake=$(command -v cmake || true)
[[ -n $cmake ]] || fail "no cmake"
if command -v ninja >/dev/null; then generator=Ninja; else generator="Unix Makefiles"; fi

printf '==> configuring (%s)\n' "$generator"
# Bundle the same expanded in-game page as the PC, with console-supported rows.
menu_assets=$build/video-menu
python3 "$root/tools/compile_video_menu.py" --platform ps5 \
    --source "$root/Darkness/Content/Gui/CubeWnd.xcr" --output "$menu_assets/CubeWnd.pc.xcr" \
    --archive-source "$root/Darkness/Content/Xdf/GameContext_Create.XDF" \
    --archive-output "$menu_assets/GameContext_Create.pc.xdf"
"$cmake" -S "$app" -B "$build" -G "$generator" \
    -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$sdk/bin/prospero-clang" \
    -DCMAKE_CXX_COMPILER="$sdk/bin/prospero-clang++" \
    -DDARK_AGC_STUBS="$agc_stubs"

printf '==> compiling\n'
"$cmake" --build "$build" --parallel "$(nproc)"

elf=$build/darkness_ps5
[[ -f $elf ]] || fail "the build produced no $elf"

# The console's loader takes the module layout the native tool writes, then a
# signed container around it.
title_id=$(sed -n 's/.*"titleId": *"\([^"]*\)".*/\1/p' "$app/sce_sys/param.json")
[[ -n $title_id ]] || fail "no titleId in sce_sys/param.json"
dist=$app/build/dist/$title_id
mkdir -p "$dist/sce_sys" "$build/link"

printf '==> linking the module layout\n'
"$tool" link --in "$elf" --out "$build/link/eboot.elf" \
    --stub-dir "$sdk/target/lib" \
    --stub "$agc_stubs/libSceAgc.so" --stub "$agc_stubs/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf

printf '==> signing\n'
"$tool" self --sign --in "$build/link/eboot.elf" --out "$dist/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$dist/eboot.bin" >/dev/null

# The loader's runtime module. A title folder on this console is not complete
# without sce_module/libc.prx: the loader mounts it into the process before the
# eboot, and a folder without one is refused at load with 0xa0020102
# (PRX_SCE_MODULE_LOAD_ERROR) before the title's first instruction - which is
# what this title logged until the module was staged here. PS5_Vulkan generates
# the clean-room shim from its own manifests (make libc there); every title that
# loads on this console carries it, so this port uses it rather than
# reimplementing a C library.
runtime=${PS5_RUNTIME_MODULE:-$vulkan/runtime/libc.prx}
cp "$menu_assets/CubeWnd.pc.xcr" "$menu_assets/CubeWnd.pc.xcr.source.sha256" \
   "$menu_assets/GameContext_Create.pc.xdf" "$dist/"
cp "$app/assets/setup-menu.rgba" "$dist/"
[[ -f $runtime ]] || fail "no runtime module at $runtime (run 'make libc' in $vulkan)"
(cd "$(dirname "$runtime")" && sha256sum --check --strict --quiet libc.prx.sha256) ||
    fail "$runtime does not match its recorded digest"
mkdir -p "$dist/sce_module"
# The app image carries the signed container, as the titles that load on this
# console do: the loader parses sce_module entries as SELF modules. A bare ELF
# staged here - the tool's extract - is refused at load with
#
#   [rtld] ERROR self_load_shared_object:2758: B: error 46 (libc.prx)
#
# which is what this title logged on 2026-10-24. The console's FTP server serves
# a signed module *decrypted* on read (that is why a read-back is a plain ELF of
# 1,335,962 bytes); it does not unpack anything on write, so what is uploaded is
# what the loader gets.
cp "$runtime" "$dist/sce_module/libc.prx"
"$tool" self --inspect --file "$dist/sce_module/libc.prx" > /dev/null
# The decrypted form that same read returns, for install.py to check what
# arrived: the module image with its SCE version record (PT_SCE_VERSION,
# 0x6fffff01) zeroed.
"$tool" self --extract --file "$runtime" --out "$build/link/libc.elf"
python3 - "$build/link/libc.elf" <<'PY'
import struct, sys
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
phoff = struct.unpack_from("<Q", data, 0x20)[0]
phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
for index in range(phnum):
    kind, _, offset, _, _, filesz, _, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + index * phentsize)
    if kind == 0x6FFFFF01:
        data[offset:offset + filesz] = bytes(filesz)
open(path, "wb").write(data)
PY
printf '==> runtime module: %s bytes signed, %s bytes as the console serves it\n' \
    "$(stat -c %s "$dist/sce_module/libc.prx")" "$(stat -c %s "$build/link/libc.elf")"
# The console makes the home-screen entry from sce_sys/icon0.png, and a folder
# without one never registers. Draw it when it is not checked in.
[[ -f $app/sce_sys/icon0.png ]] || python3 "$here/title-art.py"
cp "$app/sce_sys/param.json" "$dist/sce_sys/param.json"
# pic0 is the home-screen background; pic1 is the launch splash, the art the
# Shell zooms in when the tile is opened. The Shell draws pic1 over the title
# until the title hides it, so it ships together with the call that hides it
# (title_main.cpp's sceSystemServiceHideSplashScreen, made on the first
# presented frame): without that call the splash stays over a running game,
# which is what 2026-10-25's launch looked like.
for asset in icon0.png pic0.dds pic1.dds snd0.at9; do
    [[ -f $app/sce_sys/$asset ]] && cp "$app/sce_sys/$asset" "$dist/sce_sys/$asset"
done
# The title's game directory, empty: the dump is the player's to place, and
# nothing of it is ever built into the title.
mkdir -p "$dist/Darkness"

printf '==> %s: %s (eboot.bin %s bytes)\n' "$title_id" "$dist" "$(stat -c %s "$dist/eboot.bin")"
cat <<EOF

The title accepts a user ISO beside eboot.bin (or on USB) through Game Setup.
An existing extracted dump is also supported:

  $dist/Darkness/default.xex

Only a revision in config/game_revisions.toml is accepted; anything else is
rejected before the guest starts.
EOF
