# Bounded cache cleanup comparison

The candidate spreads the unused-resource sweep across eight frames while each
GPU cache remains under its existing byte budget. Over-budget caches still run
the original full orphan sweep and LRU eviction immediately. Cleanup still runs
only after the previous GPU submission has completed. No map iterator persists
across frames, and retained entries keep owning their source allocation.

Default: original full sweep. Create `bounded-cache-trim.enabled` in the title's
writable `darkness-ps5` directory to enable the bounded sweep live; remove it
to restore the original sweep. The existing once-per-second diagnostic polling applies the
change. `perf.txt` includes `[World] cache trim bounded=...` and entry counts.
Frame generation must remain off for this comparison.

Measure the same stationary slow view, with unchanged video settings, for at
least 30 seconds in each mode. Compare real FPS and CPU samples; do not infer an
FPS improvement from the cache microbenchmark. Then traverse the scene to check
uploads, resource turnover, and frame-time spikes. Both modes retain the same
hard budgets and may exceed them temporarily during a frame, as before; cleanup
at the following frame boundary restores the budgets.

Local checks: `tools/ps5-resource-cache-test.sh` (GCC, Clang, ASan/UBSan) and
`tools/ps5-world-draw-test.sh`. Covered cases include deferred cleanup, rehashing,
colliding hash chains, key reuse, LRU pressure, retained queued owners, and the
full-sweep switch. PS5 SDK compilation and import validation pass. Console
performance is pending; no real FPS gain is established yet.
