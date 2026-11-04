# Experimental frame-generation console test

This is a custom image-based optical-flow prototype, not an FSR integration.
It does not increase the game simulation or input rate. It is off by default.
The existing walking correction and the user's motion-blur setting remain active.

After installing and verifying the complete build with the game closed:

1. Start with `frame-generation.enabled` absent from the title's writable
   `darkness-ps5` directory. Load the same slow scene at 1080p/60, motion blur on.
2. Record a steady baseline from `perf.txt` for at least 30 seconds.
3. Create `frame-generation.enabled` in that directory over FTP. The renderer
   checks it once per second. No restart or video-settings change is required.
4. Allow warmup, then record another 30 seconds. Test slow camera turns, walking,
   zoom, subtitles, and a scene transition. Check silhouettes for ghosting.
5. Remove the enable file to return to the normal rendering path immediately.

The running title directory is normally
`/mnt/sandbox/PPSA99301_000/download0/darkness-ps5` over FTP, corresponding to
`/download0/darkness-ps5` inside the title.

`fg.txt` reports `realFps`, `generatedFps`, `displayFps`, `flowGpuMs`, warmup,
and scene-cut fallback counts. `perf.txt` also separates real/generated frames.
Generated FPS must never be described as game/simulation FPS. The existing
60 Hz FIFO presentation path may result in roughly 30 real plus 30 generated
frames; this prototype does not promise to preserve the previous 46–48 real FPS.
GPU cost, presentation pacing, latency, and image quality still require console
measurement. No automatic 30 FPS setting is written.

Create `frame-generation-capture.request` to save the next generated scene as
`fg-generated.png`. The raw scene capture excludes the separately replayed HUD
and subtitles. Writing the image causes a one-time hitch; exclude that interval
from performance comparisons. Unsupported scene sizes/formats or initialization
failures disable interpolation and retain real-frame presentation. Menus and
frames without a complete world presentation reset interpolation history.

Local validation uses `tools/ps5-interpolation-test.sh`: actual Vulkan compute
dispatches with synchronization validation, static image fidelity, bidirectional
motion against known midpoints, opaque moving objects, cuts, reset, odd-sized
resize, and offscreen real-frame rendering/copy. Passing those tests is not a
console performance or visual-quality result.

## First console result (2026-11-01)

Rejected. At 1080p the prototype produced approximately 30 real + 30 generated
queued presents per second, with 2.6–3.0 ms interpolation GPU cost. The user
reported no smoother motion and very visible artifacts. It was disabled live;
logs confirmed `requested=0 active=0 generatedFps=0`. Normal rendering resumed.
Synthetic motion tests did not predict gameplay quality. Do not enable this as
a default or describe this result as a stable 60 FPS solution.
