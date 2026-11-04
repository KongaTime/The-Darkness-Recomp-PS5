#!/usr/bin/env bash
# Check that every symbol the PS5 title imports is exported by a module the
# title loads.
#
# A title loads neither libkernel_sys's exports nor libScePosixForWebKit's, but
# the SDK's stub directory contains both. An import only their stubs define
# links and resolves to null on the console, which faults at its first call
# rather than at link time. This is the check PS5_Vulkan's link recipe applies
# before signing; run it on the linked ELF, before the native tool converts it.
#
#   tools/ps5-check-imports.sh app/ps5/build/darkness_ps5
set -uo pipefail

SDK=${PS5_SDK:-$HOME/ps5-sdk}
NM="$SDK/bin/llvm-nm"
elf=${1:?usage: ps5-check-imports.sh ELF}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The undefined dynamic symbols: what the title asks the loader for.
"$NM" -D --undefined-only "$elf" | awk '{ print $2 }' | sed 's/@.*//' | sort -u > "$work/undefined"

# What every module a title loads actually exports. The build's own AGC stubs
# are modules too: RADV imports the console's sceAgc* API, the public SDK ships
# no stubs for it, and app/ps5/tools/build.sh generates link stubs for exactly
# those names and hands them to the native tool. They are found next to the ELF
# (build/stubs) because that is where the build puts them.
{
    for library in "$SDK"/target/lib/*.so; do
        case ${library##*/} in
            libkernel_sys.so | libScePosixForWebKit.so) continue ;;
        esac
        "$NM" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done
    for library in "$(dirname "$elf")"/stubs/*.so; do
        [[ -f $library ]] || continue
        "$NM" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done
} | sort -u > "$work/defined"

comm -23 "$work/undefined" "$work/defined" > "$work/missing"

printf 'imports checked: %s\nunexported:      %s\n' \
    "$(wc -l < "$work/undefined")" "$(wc -l < "$work/missing")"

if [[ -s "$work/missing" ]]; then
    printf '\nimports that no module a title loads exports (null at run time):\n' >&2
    cat "$work/missing" >&2
    exit 1
fi
printf '\nno import resolves to nothing\n'
