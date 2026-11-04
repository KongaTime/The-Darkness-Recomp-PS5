# The Darkness — PS5 title

This directory is the console build of the port. It is a separate CMake project
from the repository's root `CMakeLists.txt` (which is the Windows build and
refuses to configure anywhere else), and it links the parts of the runtime that
are ported to the PS5 already.

## Build

The controller's Options button maps to Start; clicking the touchpad maps to
the Xbox 360 Back (Select) button.
`tools/ps5-input-test.sh` checks gradual stick changes, stable idle packets,
touchpad press/release and stick-axis limits. Controller packets change with
every reported axis change; the guest owns the stick deadzones.

To investigate camera tick interpolation, create
`/download0/darkness-ps5/camera-timing.request` during gameplay. The original
message-32 player camera query records 30 seconds to `camera-timing.bin`, with
client/player tick indices and fractions, the authored tick duration, clock
values and the final pose. The capture observes guest values without replacing
them. Run `python tools/analyze_camera_timing.py CAPTURE` after retrieving the
file. General frame submission and shown VideoOut markers are logged separately.

Camera/zoom consumers that use the client's integer tick now also use its
fractional tick. Mixing that tick with a predicted player copy's phase could
move animation time backward between rendered frames. The correction is scoped
to camera attachment sampling, zoom FOV and weapon zoom/obstruction interpolation;
prediction and simulation updates retain their original clocks.
`tools/ps5-camera-test.sh [camera-timing.bin]` checks tick rollover and can replay
a DCT2 capture against the corrected clock pairing.

The DCT3 camera capture also records the selected entity's current/previous
positions and camera position before walking effects. Its stage summary checks
whether the entity position is held within a tick despite fractional updates.
The PS5 walking-camera correction carries the visual smoothing history by the
predicted base-position delta before the original smoothing and collision code.
This prevents smoothing from accumulating movement lag and then catching up in
a sudden step. Entity positions, rotation, simulation and motion blur retain
their original paths. History resets across cuts, owner/time changes and the
guest's 40-unit discontinuity limit. The correction is enabled by default;
`walking-camera.disabled` in the writable directory disables it live for diagnosis.

DCT4 adds read-only walking-stage observations: animated head point, position
after smoothing but before wall collision, position after the walking routine,
and smoothing elapsed time. Flags 16/32 identify enabled/applied correction
samples. Legacy head-filter fields remain readable in older captures; the
unsuccessful head-offset interpolation experiment has been removed.

The in-game **Options → Video** page uses the expanded native menu on PS5.
Brightness, FOV, bloom, motion blur, FXAA, texture filtering and the frame limit
apply while playing. Resolution and language apply after restarting the app.
Subtitle size is on **Options → Subtitles**. Preferences are stored in
`/download0/darkness-ps5/video.ini`; game saves remain separate.

The default is 720p with a 60 fps limit and 16x texture filtering. Higher render
resolutions cost GPU time and memory. Surfaces scale by an integer while the
guest keeps bounded allocation dimensions, matching the PC renderer's approach.
The display uses FIFO presentation; window mode and VSync switches do not apply
to this console surface. AA offers Off and FXAA; SMAA and MSAA are not implemented.

`tools/ps5-video-test.sh` checks menu generation, preference persistence, invalid
settings and fence reuse. World draw tests include streamed mip uploads with an
empty nonresident prefix. Surface tests check scaled allocations and partial clears.
The PS5 streamed handoff buffers up to three 512-command parts of the current
frame. A queued final part blocks publication of a later frame until consumed;
draw order, empty frames and shared resource lifetimes remain intact. Other
callers keep the single-slot default.
The constant arena reuses exact immutable block bytes within a frame and skips
unchanged descriptor bindings. For an in-session timing comparison,
`constant-reuse.disabled` disables block reuse; it is polled once per second.

Detailed engine profiling is off during normal play. Creating
`/download0/darkness-ps5/profile-engine.enabled` before launch enables it.
For lower-overhead CPU attribution, create `cpu-sampling.request` in that
directory during gameplay instead. It records thirty seconds of sampled engine
phases and best-effort guest function/caller pairs to `cpu-sampling.txt`, then
disarms. The sampler does not enable the per-call timers. Only the main producer
publishes phases; samples are wall-clock observations, not exact instruction
counts, and include time when that thread is not scheduled.
The capture also samples renderer phases, including old-command retirement and
queue waiting. Render-window timings retain fractional milliseconds for each
streamed part, avoiding accumulated integer truncation.
Captures also report the real guest/render affinity masks and sampled APIC CPU
IDs. For a reversible placement experiment, write two hexadecimal masks to
`thread-placement.request` in the same directory; zero restores that thread's
original mask. Requests cannot expand either original mask, and a failed pair
rolls back. `thread-placement.txt` records the requested, prior and final masks.
PS5 draw control blocks use a bounded 2 MiB slab with per-thread reserves and
normal allocator fallback on exhaustion or unsupported size/alignment. Strong
and weak ownership and the original draw recycler are unchanged.
Internally sealed queue snapshots clear only their recorded vertex vectors on
retirement; their untouched vectors were already zero. Public mutable captures
and snapshots with aliases or unknown masks still clear the whole bank. Value
copies discard the private queue tag. The existing unrecorded-vector safety
check also disables selective clearing for the session. For an in-session
comparison, `full-draw-reset.enabled` restores full clearing; `perf.txt` reports
the safety latch and cumulative vertex bytes avoided. Hot draw metadata is
stored ahead of the constant banks.

```sh
app/ps5/tools/build.sh            # or --clean
```

The optional `DARK_PS5_THIN_LTO` CMake switch enables cross-file optimization
and permits guest weak-alias inlining after resolving native overrides. After
normal configuration, use `cmake -S app/ps5 -B app/ps5/build
-DDARK_PS5_THIN_LTO=ON`, then build normally. Generated files stay unchanged;
only their compilation force-includes `lto_guest_calls.h`. Run
`bash tools/ps5-lto-hook-test.sh` to check overridden and original calls through
both direct calls and function pointers. This is an experiment; a successful
build does not establish an FPS improvement.

`DARK_PS5_NATIVE_COMMAND_EMISSION=ON` enables an experimental native fast path
for the original `828714A0` vector-constant packet writer. It emits the same
packet bytes and retains the original guest register, flag, trace and stack
effects; full rings, aliasing buffers and unsupported layouts run the original
routine. It does not skip dirty-state processing or texture streaming. The
option defaults to **OFF**: local large-upload gains do not establish a game
FPS gain, and the guarded path costs more for a single small upload.
`bash tools/ps5-command-emission-test.sh --benchmark` compares it against the
actual generated AOT body, including full guest-memory/context checks.
`SANITIZE=1 bash tools/ps5-command-emission-test.sh` adds address/UB checks.

Environment:

| Variable | Default | Meaning |
| --- | --- | --- |
| `PS5_SDK` | `$HOME/ps5-sdk` | the payload SDK, including `toolchain/prospero.cmake` |
| `PS5_NATIVE_TOOL` | `/mnt/c/kb/PS5_Vulkan/build/host/ps5-native-tool` | ELF → module layout → signed SELF |
| `PS5_RUNTIME_MODULE` | `/mnt/c/kb/PS5_Vulkan/runtime/libc.prx` | the loader's runtime module staged as `sce_module/libc.prx` |

The build needs the AOT output in `build_native/generated`:

```sh
python tools/recompile.py --output build_native/generated --verify
```

It also needs `glslangValidator` on the path (the renderer's shaders are
compiled to SPIR-V at build time) and `python3` (the SPIR-V is embedded as C
arrays, because the console has no shader compiler or module files at run
time). It also needs the engine material tables in `build_native/shaders`
(`tools/compile_world_fragment.py`, run by the build when missing).

The console-side code has host tests: they build the same translation units with
the console branch selected and drive a UI draw and a movie frame through the
producer/consumer handshake, the frame dump's PNG writer, and the pad reader
with a scripted controller; the audio test drives the guest's render callback
and the SceAudioOut feed with a scripted output:

```sh
tools/ps5-capture-layer-test.sh
tools/ps5-audio-test.sh
tools/ps5-world-shaders-test.sh
tools/ps5-world-surfaces-test.sh
tools/ps5-world-draw-test.sh
```

Result: `app/ps5/build/dist/PPSA99301/` — a signed `eboot.bin`,
`sce_module/libc.prx` and `sce_sys/`.

## What it contains

| Part | Source |
| --- | --- |
| The translated game | `build_native/generated/ppc_recomp.*.cpp` (21,830 functions) |
| Kernel and XAM hooks | `runtime/kernel/kernel_imports.cpp` |
| Weak trace stubs for the rest | `runtime/kernel/kernel_stubs.cpp` |
| File system hooks | `runtime/kernel/host_filesystem.cpp`, `host_filesystem_posix.cpp` |
| XEX decode and revision gate | `runtime/native/xex_image.cpp`, `xex_crypto_portable.h` |
| Guest memory, image and thread setup | `app/ps5/guest_image.cpp` |
| Failure report and guest timebase | `app/ps5/guest_support.cpp` |
| Entry point | `app/ps5/title_main.cpp` |
| Console pad (wired into `XamInputGetState`) | `runtime/native/input_ps5.cpp` |
| Console audio driver and output | `runtime/native/audio_ps5_driver.cpp`, `audio_ps5.cpp` |
| Display (RADV, `VK_KHR_display`) | `renderer/ps5/display.cpp` |
| Graphics backend (Vulkan) | `renderer/ps5/graphics_backend.cpp`, `renderer/ps5/shaders/*` |
| Engine render targets (Vulkan) | `renderer/ps5/world_surfaces.cpp`, `world_surface_policy.h` |
| World draws (Vulkan) | `renderer/ps5/world_draw.cpp`, `world_draw_policy.h` |
| World shader corpus (SPIR-V) | `app/ps5/tools/world-shaders.py`, `renderer/ps5/world_shaders.cpp` |
| Frame dump (PNG) | `renderer/ps5/png_writer.cpp`, `--dump-frames` |
| Capture hooks (guest D3D calls) | `renderer/engine/render_trace.cpp` |
| Frame decoders and queue | `renderer/engine/simple_mesh.cpp`, `stored_geometry.cpp`, `world_mesh.cpp` |

The generated `ppc_imports.cpp` (a trapping body for every import) is *not*
linked: `kernel_stubs.cpp` supplies weak stubs for every import the kernel layer
does not implement, and the generated strong copies would win over both.

## What it does

1. Opens the console's display: a `VK_KHR_display` surface on RADV, a
   swapchain at the panel's mode, and the pipelines the renderer draws with.
2. Reserves the 4 GB guest address space.
3. Reads `default.xex`, checks it against `config/game_revisions.toml`, and
   rejects anything else *before* the guest starts.
4. Maps the image at `0x82000000`, publishes the XEX header at `0x81000000`,
   reads the static TLS template and fills the AOT dispatch table.
5. Lays out the main guest thread (stack at `0x70000000`, PCR at `0x7ff00000`).
6. Initialises the kernel hooks, starts the guest on its own thread at
   `_xstart`, and consumes the engine's render commands on the main thread:
   a frame is presented for every frame the engine publishes.
7. Answers the guest's controller polls from the console's pad, so
   `XamInputGetState` reports a real gamepad (see Input).
8. Plays the guest's audio: each render callback's submit is queued and
   downmixed to the console's 16-bit stereo output (see Audio).
9. Owns the engine's own render targets: the guest's surface clears, the
   resolves that copy a finished surface into the guest's texture, and the
   frontbuffer copy into the swapchain image (see World surfaces).
10. Draws the world into them: every guest draw's geometry, vertex program,
    material, textures and render state (see World draws).

## Input

`runtime/native/input_ps5.cpp` reads the console's pad and presents it in
XInput's shape; `kernel_imports.cpp`'s `XamInputGetState` answers the guest from
it, so the guest's own input code and button prompts work unchanged.

| Console pad | Guest (XInput) |
| --- | --- |
| Cross / Circle / Square / Triangle | A / B / X / Y |
| Options | Start |
| D-pad | D-pad |
| L1 / R1 | LB / RB |
| L3 / R3 | Left / right stick click |
| L2 / R2 | Left / right trigger |

Two details matter on a console. The pad is opened on the initial login user
(`sceUserServiceGetInitialUser`), not on slot 0. And `scePadRead` reports only
samples taken since the last call, so a poll with no new sample keeps the last
state instead of looking like a disconnect; the packet number then changes only
when the state does (with a small deadband for stick jitter), which is what
XInput promises the guest.

Rumble is not wired: `XamInputSetState` is still a weak stub, so the pad never
vibrates.

## Audio

The guest's audio path is one render callback plus one submit call, and its
shape is the same as on Windows: the engine registers `{callback, argument}`
with `XAudioRegisterRenderDriverClient`, then calls
`XAudioSubmitRenderDriverFrame` with 256 frames of planar 6-channel float PCM.
On Windows XAudio2's worker pulls that callback; on the console
`runtime/native/audio_ps5_driver.cpp` does, from the SceAudioOut feed:

1. `sceAudioOutOutput` returns one grain (5.3 ms at 256 frames, 48 kHz), and
   `audio_ps5.cpp` asks the driver for the next one.
2. The driver enters the guest callback on the audio worker's guest thread
   (PCR at `0x6f000000`, stack at `0x6f100000`, the image's own TLS template)
   with the registered argument in `r3`, exactly where the Windows worker runs
   it.
3. The guest's `XAudioSubmitRenderDriverFrame` copies the planar submit into a
   four-grain queue, converted to frame-major float.
4. The oldest queued grain is downmixed to stereo and packed to 16-bit:

| Xbox 5.1 channel | Console stereo |
| --- | --- |
| FL / FR | left / right |
| FC | both, -3 dB |
| LFE | dropped |
| SL / SR | left / right, -6 dB |

The queue is backpressure, not buffering: when it is full the driver stops
asking the guest for grains but keeps playing the ones it has, so latency
cannot grow; a submit past the depth is refused and counted. A guest that
submits nothing produces silence. If
the output will not open (or the port is busy), the client stays registered and
plays nothing, so the game still boots.

The driver's token validation matches the Windows client: a submit or
unregister with a stale handle returns `AUDERR_INVALID_HANDLE`, and a second
register while one is active returns `HRESULT_FROM_WIN32(ERROR_BUSY)`.

While audio plays it prints a health line every 10 seconds — `callbacks`,
`starved`, `dropped` and the queue depth — so a console session can be judged
without quitting to read the unregister totals.

`tools/ps5-audio-test.sh` covers the whole path on the host: the callback
handoff and argument cell, planar conversion, the downmix and 16-bit
saturation, silence, queue depth, stale handles, a failed output, and the real
feed thread delivering audible grains through a scripted SceAudioOut.

The on-console check — what to listen for and what the `[Audio]` lines should
say — is [AUDIO-CONSOLE-TEST.md](AUDIO-CONSOLE-TEST.md).

## Graphics

The console has no D3D11, so `renderer/ps5/graphics_backend.cpp` is the console's
side of the seam `renderer/d3d11/engine_preview.cpp` fills on Windows: it draws
the engine's render command stream into the display's swapchain.

That stream is produced by the engine's own capture layer, the same code the
Windows build runs: `render_trace.cpp` wraps the guest's original draw, upload,
frame and movie functions, and `simple_mesh.cpp` decodes the state they leave in
the engine's context into `SimpleMesh` commands. The guest publishes a frame
with `previewEndFrame()` and waits for a consumer at each frame boundary;
`app/ps5/title_main.cpp` is that consumer, and hands each frame to the backend.
The only difference from the Windows build is the guest-read guard: with no SEH,
the console relies on its single committed 4 GB guest mapping and the range
check (`copyRenderMemory`/`equalRenderMemory`), which
`tools/ps5-capture-layer-test.sh` exercises on the host.

What it draws today is the engine's simple meshes -- A8 glyph atlases, RGBA8
artwork, and the full-screen YUV movie path -- with the D3D11 preview's shaders,
row-major projection, blend state and letterboxing translated to SPIR-V:

| D3D11 | Console |
| --- | --- |
| `vertexMain` (`mul(v, projection)`) | `renderer/ps5/shaders/mesh.vert` (same order, y flipped for Vulkan) |
| `pixelMain` (A8 coverage) | `mesh_alpha.frag` |
| `colorMain` | `mesh_color.frag` |
| `videoMain` (NV12) | `mesh_video.frag` |

Shaders are compiled by glslang at build time and embedded as SPIR-V words
(`app/ps5/tools/spirv-header.py`); RADV compiles them to machine code at run
time, as it does for every title.

### World shaders

The world renderer cannot compile HLSL on the console, so its programs are built
ahead of time. `app/ps5/tools/world-shaders.py` reads the original `VP.xrg`
template and the authored `ProgramCache.xpc` descriptors, compiles every
distinct vertex program in its three raster forms (no user clip, clip distance,
cull distance) and the original fragment catalog with the same alpha-test and
bloom wrappers the Windows renderer appends: 573 vertex forms and 161 fragment
programs. `renderer/ps5/world_shaders.cpp` selects them with the Windows
renderer's keys. Bindings are shifted so one layout fits every program
(cbuffers 0..4, textures 16..31, samplers 32..47). The vertex stage keeps set 0
and the fragment stage is moved to set 1, because both authored stages name
binding 2 -- the vertex viewport constants and the fragment alpha test -- and one
set cannot hold two different buffers at one binding.

The template declares every vertex input and the compiler keeps only the ones a
program uses, so an input's location is a property of the compiled module, not of
the vertex struct. The generator reads each module's `OpName`/`OpDecorate` pairs
and emits the attribute table the draw layer builds its pipeline from; a module
whose inputs cannot be read fails the build instead of drawing at a guessed
offset.

This is the shader half of the world renderer; the surfaces it draws into are
below, and the draws themselves after them.

### World surfaces

The world renderer's other half is its render targets. The engine never
allocates one itself: it publishes commands naming Xenos surfaces by key (see
`world_mesh.h`), and on Windows `WorldRendererD3D11` owns the D3D11 textures
that answer for them. On the console `renderer/ps5/world_surfaces.cpp` owns
their Vulkan equivalents:

| Engine command | Console |
| --- | --- |
| `WorldClear` | a keyed colour (RGBA16F) or depth/stencil (D24S8) surface, cleared whole or over a rectangle |
| `WorldResolve` | a surface copied, through the original fetch exponent, into the guest's texture in its own format |
| frontbuffer present | the resolved frontbuffer filling the swapchain image, before the UI |

Surfaces are created on demand, grown when a command asks for more and never
shrunk; a new one is cleared, so a resolve never reads uninitialised memory.
The guest's four resolve formats map to `R8G8B8A8_UNORM`,
`A2B10G10R10_UNORM_PACK32`, `R16G16B16A16_UNORM` and (depth only)
`R32_SFLOAT`; the ten-bit frontbuffer is never narrowed on the way in, because
the original display ramp consumes all ten bits.

Which rectangle a resolve copies, how far its eight-pixel fringe may run past a
surface, what a clear clips to and when a surface grows are the Windows
renderer's rules, kept free of Vulkan in `world_surface_policy.h` so the host
build checks them instead of the console (`tools/ps5-world-surfaces-test.sh`,
including the 1290-against-1288-pixel frontbuffer fringe at 1080p). The
resolve's reject reasons keep the Windows renderer's numbering, so
`[World] resolve rejected: source-extent` means the same thing on both.

The copy itself is the original `Transfer` program: a full-screen triangle
whose fragment stage fetches the source texel, scaled by `exp2(exponent)`
(`renderer/ps5/shaders/transfer.vert`, `transfer.frag`). The frontbuffer step is
its own stage (`present.frag`): it fills the swapchain image from the
frontbuffer, exactly at equal extents and fitted when the panel's size differs,
which is where the Windows display path fits its target to the window.

The engine's own commands and the swapchain share one command buffer, and a
render pass cannot be interrupted, so the image's pass is closed when a clear or
a resolve arrives and reopened — keeping its contents — when something draws
again. The present is a draw into that pass, which is why the UI lands on top of
the scene.

### World draws

`renderer/ps5/world_draw.cpp` is `WorldRendererD3D11::draw` on RADV. One engine
draw carries everything it needs, so the layer is a set of caches and one
recorded command:

| Engine data | Console |
| --- | --- |
| geometry snapshot | decoded once with the engine's own decoder, uploaded to a vertex buffer (256 MB LRU, as Windows) |
| index stream | uploaded to an index buffer (64 MB LRU) |
| vertex program | the precompiled variant its option key selects; the layout is that module's attribute table |
| fragment program | the precompiled original by name and flags, with the alpha-test and bloom wrappers |
| material constants | ten dynamic uniform buffers in a per-frame arena: the 4 KiB constant bank, references, viewport, clip planes, fragment constants, texture scales, alpha test, colour lookup, bloom and shadow control |
| textures | uploaded once per `ColorImage` with the levels it owns, sampled through the guest's own sampler state; a finished surface the engine resolved is sampled through its resolved view instead |
| render state | depth test/write and the reversed-Z comparison, both stencil faces, blend factors and write mask, culling, scissor, viewport and depth bias |
| targets | the draw's own colour (RGBA16F) and depth (D24S8) surfaces, through a framebuffer grown to the union of their extents, as D3D11 retains a matching pair |

The readings of the guest's 160-byte state block -- the depth table, the stencil
faces, the blend factor table and its alpha counterpart, the write mask, the
winding, the scissor and the depth-bias arithmetic -- live in
`world_draw_policy.h`, free of Vulkan, so `tools/ps5-world-draw-test.sh` checks
them against the Windows renderer's tables instead of the console. The reject
reasons keep the Windows renderer's step numbering, so `[World] draw rejected:
texture` means the same thing on both.

Two console conventions are worth naming. HLSL reaches Vulkan through glslang
unchanged, so clip space still has +y up; the viewport is therefore flipped
(negative height) and the front-face enumeration is inverted against the D3D11
rasterizer. And Vulkan's scissor is always active, so a draw without the
guest's scissor bit covers its whole render area.

Constants and descriptor sets are per draw and per frame: the arena is reset
with the frame, and a draw that would overrun it is reported rather than
recorded. Pipelines are cached by the full state key, so the opening level
builds a few hundred and then reuses them.

Each draw currently opens and closes its own render pass. Consecutive draws into
the same pair of surfaces could share one -- RADV is immediate-mode, so the load
and store ops are cheap, but the pass bookkeeping is per draw and is the next
performance step once the picture is right.

The console reports the draw path in `[World]` lines: `draws=` and `rejected=`
with the reason names above, then `pipelines=`, `shaders=`, `textures=`, and the
cached vertex and texture megabytes. `printCounters()` also lists the first
distinct rejections with the program that caused them, which is what a console
session needs to tell a missing program from a missing texture.

Two checks run without the guest:

```sh
# eboot.bin --pipeline-test 120   draws a checkerboard quad through the real pipeline
# eboot.bin --frames 120          clears and presents, the display-only check
```

## Frame dumps

A dump saves the first presented frames as PNG files, so the console's own
output can be inspected without a capture card:

```sh
# eboot.bin --dump-frames 3                 writes /download0/darkness-ps5/frames/frame-0001.png ...
# eboot.bin --dump-frames 10 --dump-dir /download0/dumps
# eboot.bin --pipeline-test 120 --dump-frames 1   dumps the pipeline check itself
```

The image is copied out of the swapchain in the same command buffer that drew
it, then written after the display has taken the frame, so the file is exactly
what the console showed. `/download0` is the title's writable application-data mount; the title's own
directory is read-only. Each file is about 25 MB at 3840x2160, because the
console has no zlib and the writer stores the pixels uncompressed
(`renderer/ps5/png_writer.cpp`), so dump a handful of frames, not a recording.

Dumps cover frames the renderer presents. `--frames` presents through the
display's own clear path and is not dumped; `--pipeline-test` and everything the
guest publishes are. The swapchain must accept the transfer-source usage for
this to work; if the display surface does not, the title reports
`[FrameDump] Not available: ...` and plays normally.

The dump's write path is host-tested with the rest of the capture layer
(`tools/ps5-capture-layer-test.sh` decodes the produced PNG with Python's zlib
and checks every pixel and chunk CRC).

## What it does not do yet

* **Multisampled surfaces.** Surfaces and draw pipelines are single-sampled.
  The engine's MSAA setting would have to resolve each surface before the
  engine's own resolve reads it; until then a draw renders without MSAA and the
  resolve has nothing to average.
* **Generated mip levels.** A texture is uploaded with the levels its
  `ColorImage` owns. An image decoded without authored levels (a `maxLOD=0`
  resource) has one, and its sampler is clamped to it; the Windows renderer's
  CPU-generated chain is not reproduced yet, so such an image can shimmer at
distance.
* **Alpha to coverage.** The guest's alpha-to-coverage bit becomes the shader's
  always-pass alpha function, which is what the D3D11 renderer does in its
  wrapper, but the console has no coverage samples to blend, so the edge
  smoothing it would produce is absent.
* **Display gamma.** The engine's gamma ramp (`displayGamma`, the calibrated
  presentation the Windows preview runs before it presents) is counted as
  unported, so the console converts the frontbuffer but does not apply the
  original ramp.
* **On-console evidence.** The title ends with `[EnginePreview] observed=...
  accepted=... frames=...` and `[Graphics] frames=... meshes=...`: `observed`
  counts the guest's draw calls as the hooks see them, `accepted` the ones the
  decoders understood, and `frames` the ones the backend presented. A movie
  frame logs `[EnginePreview] Copied original YUV video frame WxH` once.
* **Movie backdrop canvas.** The original movie painter draws its own black
  background on a fitted canvas. `FullscreenMovieBackdropScope`
  (`runtime/native/kernel.cpp`) restores the original canvas for that one draw
  and is part of the Windows layer; on the console the fitted canvas stays, so
  a full-screen movie's background quad can be sized slightly differently. The
  movie frame itself is unaffected.
* **Native features.** FOV, mouse look and the developer tools are patched into
  the guest by `*MidAsmHook` functions that own native state. `not_ported.cpp`
  defines them as no-ops, which leaves the guest's original values in place.
  They print once when reached.
* **Rumble.** Pad vibration is not wired (`XamInputSetState` remains a weak stub).
* **Audio evidence on hardware.** The path is host-tested, not yet heard on a
  console: the SceAudioOut port format and grain pacing follow the Edge of Time
  title's working output, but this title's own mix has not been on a TV.
* **Audio mixing controls.** Voice-category volumes and mastering-voice volume
  are not wired; the guest's mix is played at its natural level. The volume
  mask import answers zero ("nothing changed"), as before.

## Console layout

```
/data/homebrew/PPSA99301/
    eboot.bin
    sce_module/libc.prx      <- the loader's runtime module (build.sh stages it)
    sce_sys/param.json
    sce_sys/icon0.png
    Darkness/default.xex        <- your own dump, not shipped
/download0/darkness-ps5/frames/      <- --dump-frames output, created on demand
```

Only a dump whose SHA-256 matches a revision in `config/game_revisions.toml` is
accepted; the port never embeds game data.

`tools/install.py` puts that layout on a console over its FTP server, resuming
interrupted transfers and checking what arrived:

```sh
app/ps5/tools/install.py --host 192.0.2.10 --local app/ps5/build/dist/PPSA99301 \
    --dump /path/to/Darkness --verify
```

The console stores what is uploaded, signed SELF included, and its FTP server
serves a signed module back *decrypted*: a read-back of `eboot.bin` is
`build/link/eboot.elf` apart from the SCE version record the container takes
with it, and a read-back of `sce_module/libc.prx` is `build/link/libc.elf`. That
decrypted form is what `--verify` compares; the loader itself is handed the
container.

The home-screen entry itself is made by ShadowMountPlus, once, when it first
sees the folder. It copies `sce_sys/icon0.png` into `/user/app/PPSA99301` and
then links the game in, so a folder without that icon never registers. The
payload logs exactly that, and nothing appears on the home screen:

```
[ACTION] Installing: The Darkness (PPSA99301)
[COPY] Failed to copy staged icon: /data/homebrew/PPSA99301/sce_sys/icon0.png -> /user/app/PPSA99301/icon0.png
[RETRY] limit reached (2/2): The Darkness (PPSA99301)
```

`tools/title-art.py` draws that icon (512x512, standard library only, no Pillow
on the build machines); `tools/build.sh` runs it when the file is missing and
copies it into the dist folder, and `tools/install.py` refuses a dist folder
without it. After the icon lands on the console, restart ShadowMount so its
scan reaches the folder again - a run that has already given up on a title
skips it (`[SKIP] mount/register retry limit reached`) until the payload is
started again.

### The launch is refused by the kernel

With the icon in place the title registers and launches, and the console refuses
the module before any of its own code runs:

```
[GAME] started: PPSA99301 pid=167
[MDBG] fatal error for PPSA99301:
0xa0020102 (PRX_SCE_MODULE_LOAD_ERROR)
thread name: eboot.bin
```

`main` never runs: the title appends its milestones to
`/data/darkness-ps5/boot.log` (`trace()` in `title_main.cpp`), and the file is
never created. Ruled out by comparing this module with homebrew titles that do
load on the same console (PPSA99200 Kong³, PPSA99302): the import list (the same
module set; the web kernel import this title used to carry is gone - `-lkernel`
in `CMakeLists.txt`), the segment layout, the process parameters, the relocation
types and the symbol counts. The upload path is not it either: the console's FTP
server unpacks the signed SELF to the ELF `build/link/eboot.elf` already is, and
a verbatim ELF copy is refused the same way.

The startup was the first suspect and is no longer one. This title used to link
the SDK's `crt1.o` - a *process* startup, with a null `DT_INIT`/`DT_FINI` - while
the titles that load here enter through their own startup, so `app_crt.cpp` now
provides it: `_start` takes the console's process parameters, `_init_env` comes
from libSceLibcInternal, and the module carries non-null `DT_INIT`/`DT_FINI`
(`0x7adfe0`/`0x7ae080`). Rebuilt, installed and launched, the console refused it
with the same `0xa0020102` and no `boot.log`, so the entry point was not it.

What the folders that do launch have, and this one did not, is
`sce_module/libc.prx` - the loader's runtime module, which the loader mounts into
the process before `eboot.bin`. Every homebrew title installed on this console
carries one (its stored form is the raw ELF the FTP server unpacks from the
signed container, 1,335,962 bytes in PS5_Vulkan's clean-room build).
`tools/build.sh` now stages PS5_Vulkan's generated module there, after checking
it against `runtime/libc.prx.sha256`, and `tools/install.py` refuses a dist
folder without it.

That changed the failure, which is how the loader was made to name the module it
refuses:

```
[MDBG] log load error for PPSA99301:
[rtld] <178> ERROR self_load_shared_object:2758: B: error 46 (libc.prx)
```

The module was there but in the tool's extract form - a bare ELF - and the
loader takes `sce_module` entries as **signed SELF modules**. `tools/build.sh`
now stages PS5_Vulkan's `runtime/libc.prx` container itself (after checking it
against `runtime/libc.prx.sha256`) and keeps its decrypted extract in
`build/link/libc.elf` for `install.py`'s read-back check. With that, the loader
loaded `/app0/sce_module/libc.prx` (the kernel log lists its fingerprint) and
went on to the eboot.

### The title runs

Past the module load the console executes `eboot.bin`. What it reports comes
from `trace()` in `title_main.cpp`, which writes each milestone to the kernel
log with `sceKernelDebugOutText` (a title sees only its own sandbox, so the
kernel log is the readout that always arrives; `ps5_console.py`'s klog capture
on port 3232 reads it live):

```
[Darkness] main (argc 1)
[Darkness] rlimit as: cur 8796093022207 MiB, max 8796093022207 MiB
[Darkness] rlimit data: cur 32768 MiB, max 32768 MiB
[Darkness] mmap 256 MiB: ok (errno 0)
[Darkness] mmap 512 MiB: failed (errno 12)
[Darkness] display failed: vkCreateInstance failed (VkResult -3)
[Darkness] guest memory failed: errno 12
```

The wall is the process's memory allowance, not the loader or the startup: an
anonymous mapping of 256 MiB succeeds and 512 MiB fails with ENOMEM, and RADV
answers `VK_ERROR_OUT_OF_HOST_MEMORY` at `vkCreateInstance` for the same reason.
The 4 GB guest reservation therefore returns before the guest starts, and `main`
returns 1. The memory instrumentation in `title_main.cpp` is bring-up code and
goest when the allowance is understood.

Experimental interpolation console testing is documented in
[FRAME-GENERATION-TEST.md](FRAME-GENERATION-TEST.md). It is disabled by default;
real game frames and generated display frames are measured separately.
