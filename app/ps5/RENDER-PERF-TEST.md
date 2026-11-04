# Render and capture performance candidate - 2026-11-02

Target: 60 FPS generally, demanding scenes at least50FPS. This target is not yet
met. Prior native-capture scene was38.1->39.5-39.6FPS. Fresh current-scene profiling
shows about41-42FPS; scene/resource counts differ, so this is not a new-build gain.

## Evidence and changes

Fresh30s signal capture:26896samples per role,30031.85ms, affinity unchanged1fff,
matching INSTALLED b16ba11c... symbols from work/perf-native-capture-candidate.
Native capture enabled/safe, no fallback/mismatch,1080p60blurONbloomONAAoffAF16,
FG0. Files work/render-fps-current-* and work/render-fps-current-profile.txt.
Memory-sample caller analysis verifies direct E8 return targets in the matching
ELF: render memcmp11.33%,memcpy6.15%; uniform shadow compares and CPU/GPU copies
are the main callers, shader-name matching also2.4%. Actual caller report is
work/render-memory-callers.txt; do not attribute every memory call to uniforms.
Render constants21.56%, queue waiting15.16%; producer queueWait0.30%. Renderer
is not the sole bottleneck despite85% renderShare. Both threads need work.

- Retain immutable queue-owned draw data for the vertex4096-byte shadow and
  fragment1008-byte tail, with the computed16-byte fragment prefix copied. Only
  captures sealed with exclusive ownership qualify. Public mutable/aliased/wrong
  owners take original copied shadow. Keep at most two CPU draw owners; reset
  each frame. Never compare GPU memory; every changed GPU block still gets fresh
  arena storage. Descriptor/pipeline/barrier/GPU completion lifetimes unchanged.
- Keep the ORIGINAL uniformBlock body as uniformBlockCopied for small blocks
  and fallback. Normal small blocks call it directly. The generalized shadow
  abstraction and custom equality experiments regressed and were removed.
- Adapt the existing64-slot two-observation descriptor recipes to the live bank
  path too. Current values are still copied/finite-checked and consumed rows
  reread. Source/binding and streaming checks unchanged. It remains opt-in.
- Correctness comparison now uses captureWorldDrawCanonical, bypassing recipes
  in the reference even when the optimization is enabled. A mismatch latches
  BOTH native capture and recipes off for the session, retaining the reference.
  The written-mask safety check also disables both.
- Counter writers use constinit single-word tagged lane handles (Lanealign64)
  and cached values, removing C++ TLS guard/field resolutions. Private lanes use
  relaxed load/store; shared overflow still uses atomic fetch_add; exact totals
  survive thread exit. No synchronization/publication counter was substituted.
- perf.txt rolls at2000report windows instead of permanently stopping FPS lines.

## Validation

- GCC/Clang counter/range/resource contracts and ASan/UBSan pass.80writers,
  private/shared/zero-private lanes, default/acquired writer, monotonic snapshots,
  exited totals. Full portable and PS5-inline/controlallocator capture suites pass.
-8000live capture comparisons now repeat descriptors to exercise recipe hits:
  6851exact full owned outputs/1149refusals; consumed-row/source races,256late row
  mutations, partial reset, ownership, real queue comparison and forced latch.
-2048canonical reference captures with recipes enabled prove zero recipe lookups.
- Actual world_draw.cpp allocator/descriptor/state/upload mock tests pass GCC and
  Clang ASan/UBSan. Tests cover exact GPU bytes/offsets, signed zero/NaN split
  prefix, batch retirement, prior GPU data immutability, owner replacement/frame
  reset, arena exhaustion, mutable/aliased/wrong-owner fallback and live disabling.
- Shader corpus/policy checks and SDK build-render-final.log exit0; imports346/0.
  Logs work/render-final-{portable,native,world,world-sanitized}-validation.log.

## Local timing - not PS5 FPS

Actual uniformBlock implementation, ten slots,4096sealed draws, realistic match
rates, alternating modes in one executable,9samples and5frames/sample; all GPU
vertex/fragment bytes verified outside timing. Final copied baseline matches
pristine implementation (~0.366us ordinary/~0.413us weighted). Retained path:
~0.323-0.326us ordinary (about11% saving),~0.378us weighted (about9%). Host upload
arena is cached, not PS5 write-combined memory; console measurement required.
Files work/uniform-render-{plain,weighted}.txt, pristine-*.txt, benchmark source.
The generalized abstraction was up to30%slower weighted vs pristine and rejected;
custom equality did not pay consistently and was also removed. Archives under
work/render-uniform-baseline are investigation only, not active source.

Native recipes: same-executable producer/consumer three-batch/background retire
benchmark, PS5 inline memory translation units, median case saving6.43%ordinary
(range2.93..13.79%),4.70%weighted(2.67..5.45%). All6,709,248native attempts per
workload succeed, zero fallback/mismatch, all owners retained. Source/results:
work/native_recipe_benchmark.cpp and work/native-recipe-{plain,weighted}.txt.
These percentages are NOT additive and are NOT whole-game FPS gains.

## Console test

Working native capture stays enabled, comparison mode off. New flags start OFF:
retained-uniforms.enabled and vertex-program-plans.enabled. Use
python work/render_perf_console.py baseline|retained|recipes|both|fetch.
The helper discovers the active sandbox suffix and verifies flag writes; it
never changes video settings, working native capture or frame generation.
The native comparison mode is separately controlled by native_capture_console.py
and is expensive: use briefly for correctness, not FPS measurement. No restart
needed between modes. Keep one slow view fixed and discard transition reports.
New perf fields: retained uniforms enabled/blocks/cpuBytesSaved and recipe safe.
Matching new artifacts archived work/perf-render-uniform-candidate/manifest.json.
Console was verified closed twice; guarded nine-file installation in progress.
Do not claim installed until installer exits0 and all files verify.

INSTALL COMPLETE: install-render-perf.log exit0, all9files uploaded and readback
verified. Decodedeboot129246096bytes, expected273-byte container lastpage diff;
libc0diff and title/menu/context assetsverified. This candidate is now INSTALLED.
Source/symbols work/perf-render-uniform-candidate. Previous b16ba11c native-only
archive remains the correct symbols for work/render-fps-current-cpu-samples.bin;
do not symbolize that older capture with this new build.
No video/input/FGsettings or live enable files changed during installation.
New retained/recipe switches startOFF; working native capture stays configuredON.
Next required step: userlaunch standalone PPSA99301 same slowview1080p60blurON;
then baseline/retained/recipes/both live via work/render_perf_console.py, no more
restartsbetweenmodes. Brief native comparison uses canonical recipe-bypassed
reference; mismatch latches native+recipesOFF. FPS50floor remainsUNCONFIRMED.

{
  "app/ps5/build/darkness_ps5": {
    "sha256": "b0d1417f434f955b4c9b94f6523a41ea52ca1c823d984b14aeb0b48f4fde9f4c",
    "bytes": 335615016
  },
  "app/ps5/build/link/eboot.elf": {
    "sha256": "bdb0dd3f1433784b78715b295e9be11b7590ef002995e43a7dd42c8c2810839d",
    "bytes": 129246096
  },
  "app/ps5/build/dist/PPSA99301/eboot.bin": {
    "sha256": "fb28d989c158f4f039ea1b4ed90ec95c2ce56fe954b631d2444435278162c9c5",
    "bytes": 128308941
  }
}

## Console result

 the last20 report windows after dropping2 transition reports.
Early fixed view: baseline39.515,retained39.460,baseline39.180,recipes39.675,
both39.840,baseline39.485FPS. Differences are small compared with natural
variation. Retained alone does not measurably improve FPS in this view.

The workload changed during the second combined stage: render batches/frame
rose from~10.7 to12 and FPS temporarily settled~37. This is NOT attributable
to the optimization alone; that stage later shifted~41.2FPS before mode changed.
Discard cross-view performance comparisons. The final matched-view pair gives
baseline40.965FPS vs combined41.530FPS (~0.565FPS/~1.38%, ~0.33ms). This supports
at most a small gain, not a major improvement. The50FPSfloor/60FPStarget is NOT met.

Canonical correctness comparison (recipesON/native compareON) completed about
2.8million draws,0fallback/mismatch,safety1. Recipe hit rate~98.5%. Retained blocks
actually activate and avoid CPU copies (latest19.3million blocks/~64.8GB
cumulative). Final capture counters91,280,110attempts/captures,0fallback/mismatch.
Native safety1 and recipe safety1. Expensive comparison mode remainsOFF.

Final live state: retained-uniforms.enabled PRESENT,vertex-program-plans.enabled
PRESENT,working native-draw-capture.enabled PRESENT,comparison flag ABSENT.
Both switches readback verified. No FPS cap/resolution/blur/input/save/FG changes.
