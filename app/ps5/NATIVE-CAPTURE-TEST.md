# Native draw capture candidate

The opt-in indexed-draw path prepares owned GPU constants directly from the
committed PS5 guest bank instead of constructing an intermediate 4 KiB binding
snapshot. It uses the canonical descriptor/key/range/mode/finite validator.
Source, device, selected record and bound shader are checked before and after
capture. Every consumed constant row is reread and compared bit for bit after
capturing the draw; unused rows remain zero in the owned output. No live guest
pointer escapes into the render queue.

All target, viewport, clip, fragment, sampler, streaming-residency and completed
texture-binding reads use the original snapshot code. Geometry is retained
before capture, images before queue admission, and only immutable owned draws
are published. Failed native captures preserve input geometry and take the
original owned-bank path, including its geometry-aware palette fallback.
Partial failures use full pooled-object reset. Immediate/diagnostic hooks retain
the original path. Windows never consumes the direct bank pointer.

## Live controls

Starts off. WorldDrawLayer checks these files once per second:

- `native-draw-capture.enabled`: enable native indexed capture.
- `native-draw-capture-compare.enabled`: compare every successful native capture
  with a fresh original capture before queueing. This mode costs extra CPU and
  must not be used for FPS measurement.

A difference retains the original draw and latches native capture off for the
session. The existing written-mask safety check also disables native capture
if it finds data outside the declared mask. Only a new process resets safety;
removing/recreating files cannot override it. `perf.txt` reports enabled,
compare, safe, attempts, captures, fallbacks and mismatches.

Use `python work/native_capture_console.py off|compare|on|fetch` with the
standalone PPSA99301 app running. The helper discovers the actual sandbox suffix,
verifies flag uploads, and does not change video, input or frame-generation
settings. DELE handles the console's 226 response. Fetch publishes local files
only after complete successful transfers.

One console session is enough to compare paths: baseline off in the same slow
view, briefly run correctness comparison, then enable without comparison for
measurement, and return off for a final baseline. No relaunch is required
between these switches. Stop on a safety latch or visible difference. Real
console measurements and the user's visual check remain outstanding.

## Offline verification

`tools/ps5-upstream-capture-test.sh` covers portable capture and GCC/Clang,
ASan/UBSan resource/range/counter contracts. `tools/ps5-native-capture-test.sh`
runs full capture contracts with PS5 inline memory reads and draw-control
allocation selected in the changed translation units under ASan/UBSan.

Native coverage: 8,000 randomized full capture cases (6,878 exact matches,
1,122 expected refusals), six material/fragment shapes including NaN fragment
payloads, a retained live 156-row palette, last-byte mutations in all 256 rows,
used-row and binding races, preserved ownership on failure, transfer on success,
partial-pool reset, real queue publication, original fallback and a forced
comparison mismatch with sticky safety fallback. Existing UI/movie/queue/PNG,
10,000 recipe comparisons and concurrent geometry-cache contracts also pass.

Same-executable full producer/consumer benchmarks alternate native off/on with
three bounded batches and background retirement, seven-sample medians and a
warmup frame after every switch. All 3,354,624 native captures per workload
retain geometry/image owners with zero fallback/mismatch. Target-like inline
read/control-allocator translation units and actual PS5 memory/compare wrappers
are linked. Nine texture/resource working sets give median producer CPU savings
19.20% ordinary (13.71-25.42%) and 8.03% large palette (4.11-9.44%). These are
synthetic capture costs, not game FPS, and exclude guest execution and GPU work.

Results/source: `work/native-capture-summary.txt`,
`work/native-capture-prospero-{plain,weighted}.txt`,
`work/run-native-capture-benchmarks.sh`. SDK build and import records live in
`app/ps5/build/build-native-capture.log` and `work/native-capture-imports.txt`.
