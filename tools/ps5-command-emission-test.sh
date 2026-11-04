#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/app/ps5/build/host-tests"
mkdir -p "$out"
# The oracle is the actual verified AOT body, not a second handwritten model.
python3 - "$root" "$out/command-emission-oracle.cpp" <<'PY'
import pathlib,sys
root=pathlib.Path(sys.argv[1]);parts=['#include "ppc_recomp_shared.h"\n']
for unit,name in [('68','sub_828714A0'),('71','__savegprlr_21'),('71','__restgprlr_21')]:
    source=(root/f'build_native/generated/ppc_recomp.{unit}.cpp').read_text()
    signature=f'PPC_FUNC_IMPL(__imp__{name}) {{'
    start=source.index(signature);end=source.index('\n}\n',start)+3
    if name.startswith('__'):
        parts.append(f'__attribute__((alias("__imp__{name}"))) PPC_WEAK_FUNC({name});\n')
    parts.append(source[start:end])
pathlib.Path(sys.argv[2]).write_text('\n'.join(parts))
PY
extra=()
if [[ ${SANITIZE:-0} == 1 ]]; then extra=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
clang++ -std=c++20 -O2 -g -mssse3 "${extra[@]}" '-D__declspec(x)=' \
    -I"$root" -I"$root/build_native/generated" \
    -I"$root/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde" \
    "$root/tests/ps5_command_emission_tests.cpp" "$root/app/ps5/command_emission.cpp" \
    "$out/command-emission-oracle.cpp" "$root/app/ps5/fast_memory.cpp" \
    -Wl,--wrap=memcpy -Wl,--wrap=memmove -Wl,--wrap=memset -o "$out/command-emission-tests"
"$out/command-emission-tests" "$@"
