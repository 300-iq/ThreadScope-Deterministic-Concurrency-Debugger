# Patch & Verify

ThreadScope compares two existing, compiled instrumented implementations. It
runs actual C++ bounded exploration for each, with identical schedule, step and
per-run timeout settings. It does not propose fixes or rewrite source code.

## Demonstrate in under 90 seconds

Build and start the app beforehand using [web-debugger.md](web-debugger.md).
The following is a short presentation walkthrough, not a benchmark or a runtime
promise under arbitrary OS load:

1. Open **Patch & Verify** in the top workflow selector. Leave **Lost-update
   counter** and the demo defaults: 6 schedules per implementation, 8 steps per
   run, 1000 ms timeout per run.
2. Click **Compare implementations**. Explain the actual results: the broken
   counter explored 6 schedules and found 4 assertion failures; the `fetch_add`
   implementation explored 2 schedules with no observed assertion failures or
   deadlocks. The bounds match, but the operation counts and scheduling trees
   differ. The prominent warning says bounded observations are not a proof of
   general correctness.
3. Click **Replay broken failure**. The selected failing schedule is reproduced
   by C++ and verified, retaining its assertion failure (CLI exit 1).
4. Click **Inspect broken trace** to open its real assertion in the existing
   debugger. The shared counter is 1, while the assertion expects 2. Return to
   Patch & Verify; its comparison and replay evidence are preserved. Select
   **Corrected** to inspect its own independent recorded schedules.
5. If time permits, choose **Lock-order deadlock**, then compare and replay. The
   opposite-order implementation explored 6 schedules with 2 modeled deadlocks;
   consistent ordering explored 2 schedules with none observed. The snippets
   show opposite versus consistent acquisition order.

Those counts were reproduced locally by tests with these exact programs and
bounds. The app never hardcodes them as results. Before execution it shows dashes,
and if either search fails to return a valid result it reports an error without
claiming a successful comparison. Preparation and package installation are outside
this presentation walkthrough.

## Engineering difference

| Supported scenario | Broken implementation | Corrected implementation |
| --- | --- | --- |
| Lost-update counter | `lost-update`: separate atomic load and store per worker | `lost-update-rmw`: one atomic `fetch_add` per worker |
| Lock-order deadlock | `mutex-deadlock`: workers acquire locks in opposite orders | `mutex-ordered`: both acquire `first`, then `second` |

The counter's split operations are individually atomic and race-free, but can
lose an update. No undefined data race is deliberately introduced. The modeled
locks use scheduler-controlled nonblocking transitions and tracked waiting;
workers do not wait inside native lock acquisition while holding a permit.
The UI shows relevant source excerpts and a concise explanation from the existing
examples, rather than an automatically generated patch.

## Results and replay boundaries

Both implementations can be selected. Each card displays actual explored schedules,
terminal assertion failures and deadlocks separately, plus completed and bounded/
error outcomes. A run stops at its first assertion failure or global modeled
no-progress deadlock. Counts concern schedules, not every possible failing
assertion in an unfinished program. Step bounds, schedule caps, incomplete search,
process errors and replay divergence remain explicit. A broken implementation
can show zero observed failures with low bounds; that is not classified as correct.
Changing inputs does not change recorded statistics: a note distinguishes the
last recorded settings from the next requested comparison.

Every failing replay uses the original trace's program identifier. The API accepts
only the trace object, with no alternate implementation parameter. A trace's
choices, runnable sets, operations, observations, dependencies and outcome belong
to that exact instrumented program. Replacing load/store with fetch_add changes
events and lengths; changing lock order changes resources and eligibility. An
old trace may therefore be incompatible with the corrected program. The workflow
never relabels or silently reuses it; corrected schedules come from a separate
clean search. Negative regression tests relabel a copy solely to confirm that
strict replay reports divergence for both existing pairs.

Inspection passes the selected implementation's actual exploration, bounds,
trace index and any real replay result to the existing debugger. Corrected trace
selection never inherits a broken replay result. Timeline navigation, JSON download
and What-If remain available there with their existing semantics.

## API

`POST /api/patch-verify` accepts exactly:

```json
{"scenario":"counter","maxSchedules":6,"maxSteps":8,"timeoutMs":1000}
```

`scenario` is `counter` or `lock-order`, identifying a fixed supported pair. The
server, not the client, maps it to the two registered executable scenarios.
Arbitrary implementation names, executable paths, commands and extra fields are
rejected. Bounds match normal exploration: 1..100 schedules, 1..256 steps,
100..5000 ms timeout per schedule. The response has `scenario`, `settings`,
`broken`, and `corrected`; each implementation result uses the existing
`Exploration` fields (`options`, `summary`, `traces`, `output`, `exitCode`, and
its concrete `scenario` name). Machine-readable trace schema v1 is unchanged.

One existing engine job guard covers both sequential searches. Each uses a separate
server-owned temporary output directory and a fixed CLI executable with no shell.
No execution state or schedule is transferred between implementations. Both use
the existing C++ DFS factory that rebuilds resources/tasks for every schedule.
Each child invocation has its 30-second watchdog; disconnect cancellation and
`finally` cleanup apply across the complete job. Combined output is capped at
16 MiB. Any incomplete/contradictory CLI protocol or mismatched program/bounds
is rejected rather than displayed as comparison success.

The same shared parser verifies identical settings, program identities, trace
counts, summary consistency and exit codes in the browser. All displayed failure
counts are derived from parsed actual traces. The API remains localhost-only
with its existing Host/Origin/JSON checks.

## Independent CLI equivalent

From the repository root, after building (run lines separately; intentional
bug discoveries exit 1):

```sh
./build/threadscope explore lost-update --max-schedules 6 --max-steps 8 --timeout-ms 1000 --all-traces --out artifacts/patch-counter/broken
./build/threadscope explore lost-update-rmw --max-schedules 6 --max-steps 8 --timeout-ms 1000 --all-traces --out artifacts/patch-counter/corrected
./build/threadscope replay artifacts/patch-counter/broken/failure-1.json

./build/threadscope explore mutex-deadlock --max-schedules 6 --max-steps 8 --timeout-ms 1000 --all-traces --out artifacts/patch-locks/broken
./build/threadscope explore mutex-ordered --max-schedules 6 --max-steps 8 --timeout-ms 1000 --all-traces --out artifacts/patch-locks/corrected
./build/threadscope replay artifacts/patch-locks/broken/failure-1.json
```

## Verification and limits

Run `ctest --test-dir build --output-on-failure`, and from `web/`, `npm run build`
and `npm test`. All six CTest suites and 53 frontend/API tests passed locally for
this milestone. New tests execute the actual binary for both pairs, compare
identical limits, check fresh initial state and repeated histories, verify broken
replay, reject cross-implementation trace reuse, cover schedule/step truncation,
reject untrusted options, and check cancellation/job guarding. Parser regressions
reject mixed settings/identities/fabricated counts. A server-rendered UI test
requires prominent limitations and six unknown metrics before execution.

Browser automation checked both pairs, broken failure/deadlock replay, corrected
selection, debugger inspection and return, low schedule bounds, stale-settings
messaging and mobile geometry. This is not a committed browser test suite.
The C++ engine was not changed. No new sanitizer or Linux CI results are claimed.
Forced watchdog overruns and mid-run disconnects remain unverified; pre-abort
cleanup and guard release have regression coverage.

Only explicitly instrumented operations with deterministic, bounded local work
are scheduled deterministically. This workflow has no arbitrary instrumentation,
weak-memory exploration, native blocking-lock support, automatic rewriting or
AI-generated fixes. Identical bounds do not imply identical schedules, equal
coverage or a performance improvement. Finding no failures within these bounds does not prove general race freedom or
general deadlock freedom.
