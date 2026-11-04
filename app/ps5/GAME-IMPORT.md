# On-console game setup

This candidate adds a native PS5 startup menu and XDVDFS ISO extraction. The
game executable and disc assets are supplied by the player, not the package.

## Player instructions

1. Put a supported Xbox 360 disc ISO in the app folder, next to `eboot.bin`:
   `/data/homebrew/PPSA99301/Your game.iso`. The filename can be anything ending
   in `.iso`, including uppercase `.ISO`.
2. Launch The Darkness. Select **Play** when an extracted game is already present,
   or select the ISO and press **X** to extract and play. Up/down chooses between
   multiple ISOs. Circle exits. If no ISO is found, X rescans without restarting.
3. Wait for extraction to finish. The screen shows the current filename, bytes
   copied and a progress bar. The original ISO is retained.

The root of USB drives `/mnt/usb0` and `/mnt/usb1` is also searched when those
mounts are accessible to the title. An ISO next to the installed app is the
primary path. Archives such as `.7z`, split ISOs and PS3 images are not supported.
Extract an archive to its single Xbox 360 ISO before copying it to the console.

## Storage and compatibility

The importer prefers `/data/homebrew/PPSA99301/Darkness`. If the title cannot
write to its install folder it uses `/download0/darkness-ps5/Darkness`, and
selects that folder automatically on future launches. `/app0` is read-only.
An existing extracted `/app0/Darkness` continues to work. The main menu uses
the artwork's Play and Exit rows directly, with a moving gold selection band
and chevron; the progress/error panel appears only during game setup. No import occurs when
an existing game is selected. Nonempty game folders are never merged or replaced.
Saves and video preferences remain separate and are not modified by setup.

Only `default.xex` hashes in the generated revision catalog are accepted, using
the same catalog as the runtime. The runtime still performs its full decoded
image verification. ISO partition layouts: XISO, XGD2, XGD3 and XGD1. `$SystemUpdate`
is skipped. File paths, table links, extents and resource limits are checked before
extraction. The ISO is read in bounded chunks, not loaded into memory whole.

Extraction writes to `Darkness.importing` with an ownership marker, checks the
extracted executable again, then renames the finished tree to `Darkness`.
An interrupted import can be retried; it restarts extraction rather than trusting
partial files. Only a staging folder with this importer's marker can be cleared.
The full extracted game requires additional free disk space while the ISO remains.

## Development and verification

- `tools/ps5-iso-import-test.sh` runs synthetic image, supported partition,
  traversal/cycle/bounds, interrupted-retry, symlink, existing-data and mutation tests.
- Set `DARKNESS_IMPORT_REAL_XEX` to a locally supplied original executable to
  additionally verify its supported hash and byte-identical extraction. The test
  creates and deletes its temporary images; it does not bundle game files.
- `setup-menu.rgba` is a raw RGBA copy of the user-provided menu artwork, with
  two little-endian uint32 width/height fields. The tiny setup alphabet is defined
  in source and does not depend on fonts extracted from the game.
- Developer `--game-dir` and display pipeline tests bypass the startup menu.
- ISO format reference: [XboxDev/extract-xiso](https://github.com/XboxDev/extract-xiso/blob/master/extract-xiso.c).

This candidate is built from the current working tree. It does **not** modify
the archived 40–42 FPS binary in `work/perf-render-uniform-candidate`, or the
console installation until explicitly installed. A build passing host tests
still needs console validation of menu display, storage permissions and extraction.
