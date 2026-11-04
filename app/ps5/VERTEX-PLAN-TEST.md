# Vertex descriptor-plan performance experiment

The PS5 capture path keeps 64 small descriptor plans per thread. Each plan
contains vertex options, references, written masks and constant-copy ranges.
The full descriptor must match, and the original binding key is still checked.
Overlapping/adjacent constant ranges are merged only within an owned bank.
One-off descriptors use the original path; a second observation is required
before a recipe is admitted, without evicting an existing useful plan early.
Every draw still uses its own captured constant bytes, and every consumed vector
is copied and checked for nonfinite values. No transforms, bone matrices, color
values, guest pointers or geometry ownership are cached by these plans.

This differs from the rejected whole-bank generation cache: only the 80-byte
descriptor is compared, not the changing 4 KB constant bank. Collisions in the
64-slot index are misses, never permission to reuse a different descriptor.
Windows keeps its original preparation path. Host tests opt in through
`DARK_TEST_VERTEX_PLAN_CACHE`; PS5 enables the code through `__PROSPERO__`.

The build is experimental. Local full-queue measurements include improvements
and regressions, so there is no established console FPS gain. Runtime controls
permit a same-process comparison:

- No `vertex-program-plans.enabled`: original descriptor preparation (default).
- Create that file: enable descriptor plans.
- Remove it: return to the original preparation path.
- Keep `bounded-cache-trim.enabled` absent during this comparison to isolate
  plans from the separate bounded cache-cleanup experiment.
- Keep frame generation off and video settings unchanged.

Controls live in the title's writable `darkness-ps5` directory and are polled
once per second. `perf.txt` records enabled state and batched lookups/hits;
lookup totals are rounded down in 1,024-call batches per thread.
Compare stationary views for at least 30 seconds each, alternating off/on/off.
Discard launch/loading/capture intervals. Check characters, lighting and
animation during subsequent movement; disable the experiment if it regresses.

Validation: `tests/vertex_program_plan_cases.h`, invoked by capture-layer tests
when the test macro is enabled, compares both paths across 10,000 randomized
cases, all 27 modes in all eight stages, changed constant banks, nonfinite
values, invalid keys/ranges, written masks, failure output and thread isolation.
The complete capture/ownership/queue suite also runs with ASan/UBSan. PS5 SDK
compilation and all 346 imports pass.

## Upstream adaptation (2026-11-02)

The local eight-slot experiment was replaced with the cache/range-merging
design from upstream d0b6e6a (PR #144), retaining the PS5 written masks,
canonical original-path fallback, current owned banks, and opt-in control.
The upstream recent-two-resource membership optimization was also adapted.
`tools/ps5-upstream-capture-test.sh` runs the independent range/union contract,
resource-set reference contract and complete capture suite under ASan/UBSan.
A pooled-reuse regression checks that a failed merged capture cannot leave
values outside the original failure prefix in the next draw.

The lean live-guest-bank capture and Windows resource-event implementation
are separate work and are not enabled by this adaptation. No console
installation or FPS result is implied by the local test/build results.
