# Local visual debugger

The React/TypeScript interface calls the real C++ engine through a lightweight
Node/TypeScript adapter. It ships no demo execution data. Descriptions come from
CLI help, and every timeline comes from JSON recorded by exploration or a fresh what-if run. The C++
library has no frontend or Node dependencies.

## Launch

Requirements: the existing C++/CMake toolchain, Node 22.12+, and npm.
From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
cd web
npm ci
npm run dev
```

Open **http://127.0.0.1:5173**. One launcher starts both Vite and the API at
**127.0.0.1:4318**. Stop it with Ctrl+C. Ports are fixed; stop an existing instance
before starting another. To use built assets instead, from `web/`:

```sh
npm run build
npm start
```

Open **http://127.0.0.1:4318**. The same origin serves built assets and the API.

## Workflow and truthful state

Choose a scenario and bounds, then Explore schedules. All passing, failing,
and truncated runs are retained with the actual summary and exit code. Capped
searches are marked incomplete even when recorded runs pass. Old results are
cleared during exploration; no placeholder timeline or invented progress count
is shown. Errors and interrupted processes never become successful summaries.

Select a schedule, click an operation, or use backward/forward/start/outcome.
Arrow keys, Home, and End also navigate outside editable inputs. Jump to failure
selects the actual assertion or the final deadlock checkpoint. A blocked request
is never shown as an executed acquisition, and Outcome is not another operation.

Inspectors use the **before-selection** checkpoint: shared values, owners,
states, and wait dependencies are consistent with each other. Event detail
separately shows the selected operation's actual before/after observation.
Outcome inspectors use the final stop decision before cancellation. Columns
represent choices, not elapsed time. Missing legacy snapshots/graphs are marked
not recorded, never reconstructed as fictitious full state.

Verify replay performs a separate CLI invocation. Only its explicit verification
marker and matching exit code produce Verified. Reproduced failures return exit
1 while verified; a verified step-limit trace returns 2. Divergence and
environmental interruption remain distinct. Navigation alone never verifies.

Open trace labels local imported JSON as unverified until replay. Branching
strictly verifies an imported original before running; see [what-if.md](what-if.md)
for comparison, suffix policy and invalid-choice handling.

Patch & Verify compares the existing Broken/Corrected counter or lock-order
programs under identical bounds, with separate failure/deadlock counts and
implementation-bound replay. See [patch-verify.md](patch-verify.md) for a short demo.

Download saves the selected recorded JSON for independent CLI use. No data leaves
localhost.

## CLI bridge and API

```sh
./build/threadscope explore lost-update --max-schedules 6 --max-steps 6 --all-traces --out artifacts/ui-example
./build/threadscope replay artifacts/ui-example/schedule-2.json
```

`--all-traces` writes `schedule-N.json` from every real exploration callback.
Default failure-only output and `failure-N.json` files are preserved. Passing
traces are not fabricated through extra reruns. Trace schema/field names remain
unchanged. The adapter parses the actual final summary and requires all its trace
files. Counts, program, limits, failure outcomes, completeness, step-bound flags,
and exit codes must agree; missing/inconsistent output fails closed. Future CLI
summary changes must update and test this narrow adapter protocol.

- `GET /api/scenarios`: actual descriptions from CLI help.
- `POST /api/explore`: exactly `{scenario, maxSchedules, maxSteps, timeoutMs}`.
- `POST /api/replay`: exactly `{trace}`, with JSON data, never a path.
- `POST /api/branch`: exactly `{trace, step, thread}`; see [what-if.md](what-if.md).
- `POST /api/patch-verify`: exactly `{scenario, maxSchedules, maxSteps, timeoutMs}`
  with a supported pair ID (`counter` or `lock-order`); see [patch-verify.md](patch-verify.md).

The adapter uses fixed `build/threadscope`, a fixed repository cwd, `spawn` with
an argument array, and `shell:false`. Request fields cannot change the executable,
cwd, shell, or command. Only the four registered scenarios are whitelisted.
JSON never loads arbitrary code. Unknown fields and scenario names are rejected.

Exploration is limited to 1..100 schedules, 1..256 steps, and 100..5000 ms per run.
Replay accepts at most 1000 configured steps, 5000 ms, and 1000 recorded events.
The viewer accepts at most 8 MiB and 1000 events; larger traces use the CLI.
API output is limited to 16 MiB of traces, 512 KiB stdout, and 128 KiB stderr.
Each server allows one exploration/replay/branch/comparison job, using a unique temporary directory
removed in `finally`. A 30-second process watchdog and disconnect signal can kill
the child; neither implies verified execution. Catalog queries use five seconds.

Both servers bind localhost. The API checks Host and Origin, accepts only JSON
POSTs with at most 8 MiB, bounds header/body reading time, and emits no permissive
CORS. Built assets have a restrictive CSP. This is a trusted local developer
tool, not an authenticated public service: other local processes can call it.
Do not expose it remotely. Branch editing uses real fresh executions with validated prefixes. Arbitrary
instrumentation remains outside scope. Engine determinism, weak-memory and timeout limitations still apply.

## Verification actually performed

```sh
# Root
ctest --test-dir build --output-on-failure
# web/
npm run build
npm test
```

Strict TypeScript checking and the Vite build passed. All 69 tests passed:
actual C++ fixture parsing, legacy/malformed records, checkpoint and failure
navigation, and real-binary HTTP integration. API tests cover real failures and
corrected timelines, caps, exit-1/exit-2 verified replay, actual divergence,
path/command/origin/host rejection, pre-cancellation, and single-job guarding.
What-if regressions cover both counter outcomes, original verification, invalid
prefixes, preserved bounds, lock changes, and truthful comparison navigation.
Patch & Verify tests cover both real implementation pairs, repeated clean runs,
identical bounds, separate failure types, incompatible traces, and no invented
initial statistics. Comparison input and result identity/limits are validated.
Fixtures are only test data; the application never uses them, and API tests
execute the current binary rather than treating fixtures as backend proof.
A parser regression reproduced and fixed UTF-8 resource-name ordering
mismatching JavaScript UTF-16 ordering, using an actual C++ library trace.

All eight CTest suites passed in strict Debug, standalone UBSan,
and Release. Earlier milestone browser checks covered real exploration, assertions, deadlocks,
navigation, replay, no-failure/bounded states, and desktop/mobile geometry. The
built interface was checked at the API origin as well as the Vite origin.

The complete audit also added duplicate-key/nesting validation, stable outcome/
assertion checks, explicit malformed/HTML response errors, and deterministic
kill/close/watchdog/spawn-error unit tests. Integration tests continue to execute
the real binary; lifecycle mocks never produce execution results. The audit browser
pass used the rebuilt API origin for real exploration, What-If failure-to-pass,
final counter comparison, and replay-verified counter Patch & Verify. The latest
screenshots are in `docs/images`. See [audit.md](audit.md).

Local environment: macOS arm64, Apple Clang 17, Node 26.8.1. npm dependencies are
pinned in the lockfile. ASan/TSan runtime probes failed again in this audit;
Linux CI is configured, not locally verified. No forced watchdog overrun or
mid-run disconnect experiment was performed; pre-abort cleanup and job-guard
release are automated checks. Browser checks are manual automation, not a
committed browser suite. Performance measurements are documented separately in
[benchmark.md](benchmark.md); no universal race-freedom claim is made.
