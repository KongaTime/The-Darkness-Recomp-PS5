#!/usr/bin/env python3
"""Name the console's host instruction samples.

    tools/ps5-symbolize-samples.py cpu-samples.bin [app/ps5/build/darkness_ps5]

app/ps5/cpu_sampling.cpp writes cpu-samples.bin during a capture (request it by
uploading cpu-sampling.request to /download0/darkness-ps5/): a magic word, the
runtime address of DarkRecomp::Ps5::runCpuSampler, the stack words per sample,
then for the guest and the render thread a count and that many (rip, stack
words) records. The anchor gives the load slide against the unstripped ELF. A
sample outside the title's executable segments (libc memcpy, kernel waits) is
charged to the first stack word that points into the title, marked "via libc"
or "via kernel". Output: per thread, the hottest functions and the hottest
source lines, as shares of that thread's samples.
"""
import collections
import struct
import subprocess
import sys

MAGIC = 0x32534D5048524B44
ANCHOR = '_ZN10DarkRecomp3Ps513runCpuSamplerER10PPCContextRKNSt3__16atomicIbEE'
ROLES = ('guest', 'render')


def executable_ranges(elf):
    with open(elf, 'rb') as f:
        data = f.read(1 << 20)
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    ranges = []
    for i in range(phnum):
        kind, flags, _, vaddr, _, _, memsz, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        if kind == 1 and flags & 1:
            ranges.append((vaddr, vaddr + memsz))
    return ranges


def anchor_address(elf):
    out = subprocess.run(['llvm-nm', '--defined-only', elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == ANCHOR:
            return int(parts[0], 16)
    sys.exit(f'error: {ANCHOR} is not in {elf}')


def symbolize(elf, addresses):
    if not addresses:
        return {}
    text = '\n'.join(hex(a) for a in addresses) + '\n'
    out = subprocess.run(['llvm-symbolizer', '--obj=' + elf, '--functions=linkage', '--demangle',
                          '--no-inlines', '--relativenames'],
                         input=text, capture_output=True, text=True, check=True).stdout
    lines = [l for l in out.split('\n')]
    names, i = {}, 0
    for a in addresses:
        while i < len(lines) and not lines[i].strip():
            i += 1
        function = lines[i] if i < len(lines) else '??'
        location = lines[i + 1] if i + 1 < len(lines) else '??'
        names[a] = (function, location)
        i += 2
    return names


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    elf = sys.argv[2] if len(sys.argv) > 2 else 'app/ps5/build/darkness_ps5'
    with open(path, 'rb') as f:
        data = f.read()
    magic, anchor, words = struct.unpack_from('<QQQ', data, 0)
    if magic != MAGIC:
        sys.exit('error: not a cpu-samples.bin capture (or an older format)')
    record = 8 * (1 + words)
    slide = anchor - anchor_address(elf)
    ranges = executable_ranges(elf)
    inside = lambda a: any(lo <= a < hi for lo, hi in ranges)

    offset, per_role = 24, []
    for role in ROLES:
        count, = struct.unpack_from('<Q', data, offset)
        offset += 8
        samples = [struct.unpack_from(f'<{1 + words}Q', data, offset + record * k) for k in range(count)]
        offset += record * count
        resolved = []
        for rip, *stack in samples:
            if inside(rip - slide):
                resolved.append((rip - slide, ''))
                continue
            caller = next((w - slide for w in stack if inside(w - slide)), None)
            # libkernel (system calls, waits) is the first module mapped.
            via = ' (via kernel)' if 0x800000000 <= rip < 0x800010000 else ' (via libc)'
            # The return address follows the call; step back into it.
            resolved.append((caller - 1, via) if caller is not None else (None, via))
        per_role.append((role, resolved))

    unique = sorted({a for _, r in per_role for a, _ in r if a is not None})
    names = symbolize(elf, unique)
    for role, resolved in per_role:
        total = len(resolved)
        print(f'== {role} thread: {total} samples ==')
        if not total:
            continue
        functions, lines = collections.Counter(), collections.Counter()
        unknown = 0
        for address, tag in resolved:
            if address is None:
                unknown += 1
                continue
            function, location = names.get(address, ('??', '??'))
            functions[function + tag] += 1
            lines[f'{location}  [{function[:60]}]{tag}'] += 1
        print(f'  outside the title and its caller unknown: {unknown} ({100.0 * unknown / total:.1f}%)')
        print('  hottest functions:')
        for name, n in functions.most_common(40):
            print(f'    {100.0 * n / total:6.2f}%  {name}')
        print('  hottest lines:')
        for name, n in lines.most_common(40):
            print(f'    {100.0 * n / total:6.2f}%  {name}')
        print()


if __name__ == '__main__':
    main()
