# The Darkness Recomp for PS5

An experimental PS5 homebrew port of the Xbox 360 version of **The Darkness**, based on [portingpete’s The Darkness Recomp](https://github.com/portingpete/The-Darkness-Recomp).

![The Darkness Recomp Play and Exit menu](app/ps5/assets/menu-preview.jpg)

## Download and install

Open [GitHub Releases](https://github.com/KongaTime/The-Darkness-Recomp-PS5/releases) and download `The-Darkness-Recomp-PS5-v0.1.0-preview.1.zip` under **Assets**. Use a PS5 setup that already supports launching homebrew titles from `/data/homebrew`.

1. Extract the ZIP. Copy its `PPSA99301` folder to `/data/homebrew/` on your PS5.
2. Put your own supported Xbox 360 `.iso` beside `eboot.bin` in that folder. If you already have an extracted dump, place its files in `PPSA99301/Darkness/`, with `default.xex` directly inside `Darkness/`.
3. Launch **The Darkness** with your homebrew loader. Select **Play** for an existing dump, or select your ISO and press **X** to import. Keep the ISO in place until extraction finishes.

The archive includes the PS5 app and its launcher artwork. Bring your own copy of the game. See [Game setup](app/ps5/GAME-IMPORT.md) for supported images, storage locations, and recovery after an interrupted import.

## Preview status

- The launcher and **Play** have been tested on PS5 with an existing extracted dump.
- The ISO reader and extractor passed host tests. A full ISO import on PS5 is still unverified.
- Expect variable performance. An earlier PS5 build measured roughly 40–42 FPS in a demanding scene. This preview has no measured performance target.
- The public ZIP omits the video-menu overrides generated from the development game dump. The runtime falls back to your dump’s original menu files. This exact ZIP still needs a console smoke test.

## Build and credits

See the [PS5 build notes](app/ps5/README.md) for the toolchain and [game revision catalog](config/game_revisions.toml) for supported executables. The PS5 work started from upstream commit `98f0bcd`. Upstream authorship is retained in Git history. See [COPYING](COPYING) and the third-party notices in the source tree.
