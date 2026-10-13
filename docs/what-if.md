# What-If Schedule Explorer

A what-if result is a fresh execution of a registered instrumented program.
ThreadScope does not predict outcomes in React or edit observations in saved JSON.
The original trace remains the comparison anchor, including when it passes.

## Use the debugger

Explore `lost-update`, select its second recorded schedule, and click decision 1
(`T1 load`). T0 and T1 were runnable; T2 was waiting for the increment workers.
Choose T0 and Run what-if. The actual counter outcome changes from 1 (assertion
failure) to 2 (pass). Select the first passing schedule, click decision 1
(`T0 store`), and choose T1 to reproduce the reverse change.

The original and modified timelines are displayed together, with the first
scheduling divergence outlined. Clicking either comparison timeline inspects
both at the same **logical decision index**, not elapsed time or equivalent
source positions. After divergence, the selected operations can differ.
Before/after resource snapshots are labeled separately; thread states and wait
dependencies are recorded before selection, or at the final stop. Changed values,
lock owners, thread states, dependencies, and final outcomes are highlighted.

Final outcomes compares each trace's own stop snapshot even when lengths differ.
If only one trace reached a later checkpoint, the other panel says it did not
reach that checkpoint; it never extrapolates or repeats a terminal state there.
Legacy missing snapshots remain “not recorded.” Reset to Original removes the
comparison and candidate choice, keeping the original loaded. A new branch always
uses that original, not the previous modified run. Both JSON files can be downloaded.

Only recorded runnable alternatives are selectable. Original, blocked, and
terminated choices are disabled. An engine-rejected candidate is also disabled
for that decision. A malformed/stale original is an error, not a valid comparison.
No candidate is applied automatically and no outcome appears while it is pending.

## Independent CLI

From the repository root, after building:

```sh
./build/threadscope run lost-update --schedule 0,1,0,1 --trace artifacts/original.json
./build/threadscope branch artifacts/original.json --step 1 --thread 0 --trace artifacts/modified.json
./build/threadscope replay artifacts/modified.json
./build/threadscope branch artifacts/modified.json --step 1 --thread 1 --trace artifacts/failing-again.json
./build/threadscope replay artifacts/failing-again.json
```

The first and last failing executions intentionally return exit 1. Modified
returns 0. Each branch first verifies the complete original event history and
outcome using strict replay. It then constructs a new scenario and scheduler:

1. Retain choices at indices strictly less than `--step`.
2. Replace the choice at `--step` with `--thread`.
3. At every unchanged event, check the runnable set, requested operation,
   observation, and available state/dependency metadata against the original.
4. At the branch checkpoint, validate the full recorded pre-choice runnable set,
   thread states, shared/lock snapshot and wait dependencies before granting a
   permit. The selected thread must be runnable in the actual fresh execution.
5. Continue using **lowest runnable logical thread ID** at each decision after
   the supplied prefix. This deterministic policy has no fairness guarantee.

The suffix is rerun; the old suffix is not copied. Limits come from the original
trace, including step bounds. Invalid choices stop without granting that
operation, return exit 2 and name the rejected ID, decision and actual runnable
set. Invalid steps (including Outcome) are rejected. Step bounds produce an
actual `step_limit` trace rather than a pass. The CLI refuses to overwrite the
original file, including equivalent paths, symlinks and existing hard links.
A failed original verification produces no new trace.

The library exposes `branch_config(trace, step, alternative)`. Call
`replay_trace(trace, factory)` first and require a matching stable result, then
call the fresh-state factory with the branch configuration. Exact-replay
configuration remains separate from prefix/branch checkpoint checks. A factory
must honor those checks and construct fresh resources/tasks each time.

## Local API

`POST /api/branch` accepts exactly these fields (the request is JSON-encoded):

```ts
{ trace: originalTrace, step: 1, thread: 0 }
```

`originalTrace` is the complete schema-v1 JSON object, never a path or string.
The endpoint accepts only registered programs, stable
outcomes, at most 1000 configured/recorded steps, and at most 5000 ms. The original
limits are preserved. It invokes the fixed CLI executable with an argument array
and no shell, writing input/output only in a server-owned temporary directory.
It runs `branch` and independently strictly replays the resulting stable trace.
The job guard covers both invocations; each process has a 30-second watchdog and
is killed on disconnect. These are two bounded invocations, not one exploration.

Response fields are `trace`, `step`, `thread`, `prefix`, `policy`
(`lowest_runnable_id`), `originalVerified`, `status`, `replay`, `output`, and
`exitCode`. `status` is `executed` only when the new trace also replay-verifies;
it includes passing, failing, deadlocked and step-bounded outcomes. The trace's
`outcome.status` distinguishes them. `invalid_prefix` means an actual nonrunnable
choice was rejected. `unverified` distinguishes new-run/replay divergence or
environmental errors. `replay` is null for unstable outcomes; all diagnostics
come from real CLI output. A stale original returns HTTP 409 and no branch;
invalid request fields return 400. Trace schema version 1 and its field names
remain unchanged. Branch metadata lives in the API response, not fabricated trace
operations.

## Verification and limits

Regression tests cover lost-update failure→pass and pass→failure, repeated fresh
runs and strict replay, invalid choices, stale metadata/outcomes, zero-prefix
branching, preserved files/state, modeled lock changes, step bounds, and shorter
trace comparison. A C++ test alternates startup and between-operation worker
delays while requiring identical branch/replay histories; arrival order is not a
scheduler input. Frontend tests cover recorded alternatives, engine-rejected
choices, divergence, resource differences, missing legacy metadata and absent
checkpoints. HTTP tests execute the real C++ binary in both directions.

Determinism applies only to controlled instrumented operations with deterministic,
bounded local work and fresh initial state. Timeouts and OS/process errors are
outside that guarantee. There is no arbitrary binary instrumentation, native
blocking-lock support, weak-memory exploration, fairness, search completeness
beyond bounds, or proof that changing one schedule fixes a program. Legacy traces
can validate only the metadata they contain. Build identity/hashes and untrusted
program execution isolation remain outside this milestone.
