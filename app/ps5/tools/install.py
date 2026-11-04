#!/usr/bin/env python3
"""Install The Darkness PS5 title on a console over its FTP server.

    app/ps5/tools/install.py --host 192.0.2.10 --port 2121 \
        --local app/ps5/build/dist/PPSA99301 \
        --dump "/path/to/your/extracted/Darkness"

The console layout is the one app/ps5/README.md documents:

    /data/homebrew/PPSA99301/eboot.bin
    /data/homebrew/PPSA99301/sce_module/libc.prx
    /data/homebrew/PPSA99301/sce_sys/param.json
    /data/homebrew/PPSA99301/Darkness/<your dump>

`--local` is the built dist folder (eboot.bin and sce_sys/param.json); `--dump`
is the extracted game dump, which is copied into the app folder's Darkness
directory. Nothing of the dump is read into the title; it stays a file on the
console.

Every file is written under a hidden temporary name and only replaces its
destination once complete, so an interrupted run leaves no half file under the
name the title reads. A run that is interrupted resumes: a temporary file that
is a prefix of the local one is continued with APPE from its stored size.

eboot.bin and sce_sys/param.json are published last, after the dump is in
place, so the title never appears installed while its game data is missing.
Files already present with the same size are skipped unless --all is given.
`--verify` downloads the three files the title reads first and compares their
SHA-256 with the local copies.

The console stores what is uploaded: its FTP server does not unpack a signed
module on write. It *decrypts* one on read, so a read-back of eboot.bin is the
link output build/link/eboot.elf apart from the SCE version record the container
takes with it, and a read-back of sce_module/libc.prx is build/link/libc.elf.
That decrypted form is what the size and --verify checks compare against; the
loader itself is handed the signed container.
"""

import argparse
import hashlib
import io
import os
import sys
import time
from ftplib import FTP, error_perm, error_reply
from pathlib import Path, PurePosixPath

PUBLISH_LAST = ("eboot.bin", "sce_sys/param.json")
# The Xbox system update payload is not game data and the title never opens it.
EXCLUDED = ("$SystemUpdate",)
# The container's magic, as a title's eboot and the runtime module use the two
# the tool writes (0x1D3D154F and 0xEEF51454).
SELF_MAGIC = (b"O\x15=\x1d", b"T\x14\xf5\xee")
ELF_MAGIC = b"\x7fELF"
# The container drops section-name padding at the end; nothing in the last page
# is loaded, so a difference confined there is the unpack, not a bad upload.
UNPACK_TAIL = 4096


def ensure_directory(ftp, path):
    current = ""
    for part in [p for p in path.split("/") if p]:
        current += "/" + part
        try:
            ftp.mkd(current)
        except error_perm as error:
            if not str(error).startswith(("550", "521", "550 ")):
                if "exist" not in str(error).lower():
                    raise


def remote_size(ftp, path):
    try:
        return ftp.size(path)
    except error_perm:
        return None


def delete_if_there(ftp, path):
    # This server answers DELE with "226 File deleted", which ftplib reports as
    # error_reply even though the file is gone; a 550 means there was nothing.
    try:
        ftp.delete(path)
    except error_perm as error:
        if not str(error).startswith("550"):
            raise
    except error_reply as reply:
        if not str(reply).startswith("2"):
            raise


def put(ftp, local, remote, log, reference=None):
    """Upload one file resumably, then move it into place under its real name.

    A signed SELF is served back decrypted, so this server reports the size of
    its ELF; when `reference` is that decrypted form the stored binary is
    expected to be its size instead of the container's.
    """
    size = local.stat().st_size
    folder, name = remote.rsplit("/", 1)
    ensure_directory(ftp, folder)
    temporary = f"{folder}/.{name}.part"

    stored = remote_size(ftp, temporary)
    if stored is not None and (stored >= size or stored == 0):
        delete_if_there(ftp, temporary)
        stored = None
    offset = stored or 0

    started = time.monotonic()
    sent = offset
    with local.open("rb") as source:
        if offset:
            source.seek(offset)
        command = "APPE " if offset else "STOR "

        def report(block):
            nonlocal sent
            sent += len(block)
            elapsed = time.monotonic() - started
            if elapsed < 1.0:
                return
            rate = (sent - offset) / elapsed / (1 << 20)
            percent = sent * 100.0 / size if size else 100.0
            log(f"    {percent:5.1f}%  {sent / (1 << 20):8.1f}/{size / (1 << 20):.1f} MiB"
                f"  {rate:5.1f} MiB/s")

        ftp.storbinary(command + temporary, source, blocksize=1 << 20, callback=report)

    stored = remote_size(ftp, temporary)
    if stored != size:
        with local.open("rb") as source:
            signed = source.read(4) in SELF_MAGIC
        unpacked = (reference is not None and reference.stat().st_size == stored and signed)
        if not unpacked:
            raise RuntimeError(f"{local}: stored {stored} bytes, expected {size}")
        log(f"    the console unpacked the signed eboot to ELF ({stored:,} bytes)")
    delete_if_there(ftp, remote)
    ftp.rename(temporary, remote)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def verify(ftp, remote, local, log):
    expected = sha256(local)
    buffer = io.BytesIO()
    ftp.retrbinary("RETR " + remote, buffer.write, blocksize=1 << 20)
    digest = hashlib.sha256(buffer.getvalue()).hexdigest()
    if digest != expected:
        raise RuntimeError(f"{remote}: {digest} does not match {expected}")
    log(f"  verified {remote} ({local.stat().st_size:,} bytes, sha256 {digest[:16]}...)")


def verify_unpacked(ftp, remote, local, reference, log):
    """Check what the console serves a signed module as against `reference`."""
    buffer = io.BytesIO()
    ftp.retrbinary("RETR " + remote, buffer.write, blocksize=1 << 20)
    stored = buffer.getvalue()
    if not stored.startswith(ELF_MAGIC):
        raise RuntimeError(f"{remote}: the stored eboot is not an ELF")
    expected = reference.read_bytes()
    if len(stored) != len(expected):
        raise RuntimeError(f"{remote}: {len(stored)} bytes, the link output is {len(expected)}")
    first = next((i for i in range(len(expected)) if stored[i] != expected[i]), None)
    if first is not None and first < len(expected) - UNPACK_TAIL:
        raise RuntimeError(f"{remote}: differs from the link output at byte {first}")
    differing = sum(1 for a, b in zip(stored[-UNPACK_TAIL:], expected[-UNPACK_TAIL:]) if a != b)
    log(f"  verified {remote}: ELF {len(stored):,} bytes, {differing} bytes differ from"
        f" {reference.name} in its last page, as the container unpack does")


def collect(local_root, dump_root, dump_name):
    """Every file to install, as (local path, path inside the app folder)."""
    files = []
    for path in sorted(local_root.rglob("*")):
        if path.is_file():
            files.append((path, path.relative_to(local_root).as_posix()))
    if dump_root is not None:
        for path in sorted(dump_root.rglob("*")):
            relative = path.relative_to(dump_root).as_posix()
            if relative.split("/")[0] in EXCLUDED:
                continue
            if path.is_file():
                files.append((path, f"{dump_name}/{relative}"))
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=2121)
    parser.add_argument("--user", default="anonymous")
    parser.add_argument("--password", default="")
    parser.add_argument("--local", default="app/ps5/build/dist/PPSA99301",
                        help="built title folder (eboot.bin, sce_sys/param.json)")
    parser.add_argument("--dump", default=None,
                        help="extracted game dump; copied into the app folder's Darkness directory")
    parser.add_argument("--dump-name", default="Darkness", help="folder inside the app the title reads")
    parser.add_argument("--remote", default=None,
                        help="app folder on the console (default /data/homebrew/<title id>)")
    parser.add_argument("--all", action="store_true", help="upload files already there too")
    parser.add_argument("--verify", action="store_true",
                        help="download what the title reads first and compare SHA-256")
    parser.add_argument("--plan", action="store_true", help="print the transfer plan and stop")
    args = parser.parse_args()

    local_root = Path(args.local).resolve()
    # sce_module/libc.prx is the loader's runtime module: a folder without it
    # starts and is refused at load (0xa0020102), so a dist folder missing it
    # is a build that never completed, not a title to install.
    for required in ("eboot.bin", "sce_sys/param.json", "sce_sys/icon0.png",
                     "sce_module/libc.prx"):
        if not (local_root / required).is_file():
            if required == "sce_sys/icon0.png":
                # ShadowMountPlus copies this icon into /user/app/<title id> and
                # abandons the install without it, so the title would sit in the
                # app folder with no home-screen entry at all.
                raise SystemExit(f"{local_root} has no {required}; run tools/title-art.py")
            raise SystemExit(f"{local_root} has no {required}; run tools/build.sh first")
    dump_root = Path(args.dump).resolve() if args.dump else None
    if dump_root is not None and not (dump_root / "default.xex").is_file():
        raise SystemExit(f"{dump_root} has no default.xex; that is not a Darkness dump")

    remote_root = args.remote
    if remote_root is None:
        title_id = None
        for line in (local_root / "sce_sys/param.json").read_text().splitlines():
            if '"titleId"' in line:
                title_id = line.split(":", 1)[1].strip().strip('",')
        if not title_id:
            raise SystemExit("no titleId in sce_sys/param.json; pass --remote")
        remote_root = f"/data/homebrew/{title_id}"

    # The decrypted forms the console serves the signed modules as: the link
    # output for eboot.bin, the tool's extract for the runtime module.
    reference = local_root.parent.parent / "link/eboot.elf"
    if not reference.is_file():
        print(f"note: no {reference}, the stored eboot will only be checked for its ELF header")
        reference = None
    libc_reference = local_root.parent.parent / "link/libc.elf"
    if not libc_reference.is_file():
        print(f"note: no {libc_reference}, the runtime module is not checked")
        libc_reference = None
    if libc_reference is None:
        reference_for = {}
    else:
        reference_for = {"sce_module/libc.prx": libc_reference}

    files = collect(local_root, dump_root, args.dump_name)
    ordered = ([f for f in files if f[1] not in PUBLISH_LAST] +
               [f for f in files if f[1] in PUBLISH_LAST])
    total = sum(path.stat().st_size for path, _ in ordered)
    print(f"{len(ordered)} files, {total / (1 << 30):.2f} GiB -> {args.host}:{args.port}{remote_root}")
    if dump_root is None:
        print("warning: no --dump, the title will stop at its 'put default.xex here' message")
    if args.plan:
        for path, name in ordered:
            print(f"  {path.stat().st_size:>12,}  {name}")
        return

    log = lambda line: print(line, flush=True)
    started = time.monotonic()
    with FTP() as ftp:
        ftp.connect(args.host, args.port, timeout=60)
        ftp.login(args.user, args.password)
        ftp.voidcmd("TYPE I")
        ensure_directory(ftp, remote_root)

        sent = skipped = 0
        done_bytes = 0
        for index, (path, name) in enumerate(ordered, 1):
            remote = f"{remote_root}/{name}"
            size = path.stat().st_size
            if not args.all and remote_size(ftp, remote) == size:
                skipped += 1
                done_bytes += size
                continue
            log(f"[{index}/{len(ordered)}] {name}  ({size / (1 << 20):.1f} MiB)")
            if name == "eboot.bin":
                put(ftp, path, remote, log, reference)
            else:
                put(ftp, path, remote, log, reference_for.get(name))
            sent += 1
            done_bytes += size
            elapsed = time.monotonic() - started
            log(f"  done; {done_bytes / (1 << 30):.2f} GiB in {elapsed / 60:.1f} min"
                f" ({done_bytes / elapsed / (1 << 20):.1f} MiB/s overall)")

        for name in ("eboot.bin", "sce_module/libc.prx", f"{args.dump_name}/default.xex",
                     "sce_sys/param.json", "CubeWnd.pc.xcr", "CubeWnd.pc.xcr.source.sha256",
                     "GameContext_Create.pc.xdf", "setup-menu.rgba"):
            local = next((p for p, n in ordered if n == name), None)
            if local is None:
                continue
            stored = remote_size(ftp, f"{remote_root}/{name}")
            expected = local.stat().st_size
            if name == "eboot.bin" and reference is not None:
                expected = reference.stat().st_size
            if name in reference_for:
                expected = reference_for[name].stat().st_size
            log(f"  {name}: {stored:,} bytes stored (local {local.stat().st_size:,}"
                f"{', unpacked ELF' if expected != local.stat().st_size else ''})")
            if stored != expected:
                raise SystemExit(f"{name} did not arrive whole")

        if args.verify:
            for name in ("eboot.bin", "sce_module/libc.prx", f"{args.dump_name}/default.xex",
                         "sce_sys/param.json", "CubeWnd.pc.xcr", "CubeWnd.pc.xcr.source.sha256",
                         "GameContext_Create.pc.xdf", "setup-menu.rgba"):
                local = next((p for p, n in ordered if n == name), None)
                if local is None:
                    continue
                if name == "eboot.bin" and reference is not None:
                    verify_unpacked(ftp, f"{remote_root}/{name}", local, reference, log)
                elif name in reference_for:
                    verify_unpacked(ftp, f"{remote_root}/{name}", local, reference_for[name], log)
                else:
                    verify(ftp, f"{remote_root}/{name}", local, log)

    print(f"==> {remote_root}: {sent} files sent, {skipped} already there, "
          f"{(time.monotonic() - started) / 60:.1f} min")


if __name__ == "__main__":
    main()
