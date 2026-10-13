# Review of bounded exploration and replay milestone

Scope: the latest DFS spine, named scenario factories, resource snapshots,
pre-grant replay checks, full-result replay validator, and associated tests and
claims. Existing scheduler failure handling was inspected for interactions with
these changes. No frontend, primitives, or unrelated features were added. A
source snapshot was saved before review; new regressions ran against the unfixed
explorer before implementation changes.

## P2: A factory overriding the suffix policy silently skipped schedules

DFS increments each recorded choice index and visits only higher remaining IDs.
It assumes the unforced suffix always starts with the lowest runnable ID. The
new decision validation checked sorted runnable sets and membership but never
checked this assumption.

`factories_cannot_silently_skip_choices` constructs fresh resources and real
workers on every call, but its factory accidentally forces initial choice 1 for
two single-yield workers. Before the fix, exploration reported exhaustion after
only schedule `1,0`; it never visited `0,1`. The regression failed. An additional
case overrides a later unforced choice (`0,2,1`) and verifies rejection before
the persistence callback reports a successful run or another factory starts.

Fix: check every unforced event against the lowest runnable ID. A violation
returns an explicit `replay_mismatch` diagnostic and leaves `exhausted=false`.
Forced prefixes retain their existing history checks. The built-in factories
already honored this policy; their expected schedule lists remain unchanged.

## P2: A factory overriding the step limit produced false completeness

`factories_cannot_silently_override_bounds` requests one step but constructs a
fresh scheduler with a factory that changes the limit to two. Its single worker
performs two yields. Before the fix, the explorer accepted this completed run
and reported an exhaustive search without a step-limit indication. The regression
failed independently of the policy regression.

Fix: reject returned histories longer than the requested bound before reporting
them to callbacks or adding their decisions to the DFS spine. This is detection
after the factory returns; it cannot forcibly stop a factory that ignores the
configuration. Factories remain responsible for honoring it.

## Determinism assessment

No new uncontrolled arrival-order choice or unsynchronized scheduler access was
identified in the supported snapshot/replay path. The driver still waits for
every worker to park or terminate, refreshes eligibility under its monitor, then
records sorted runnable IDs and shared resource snapshots. Pre-grant comparison
rejects changed request metadata or before-state; post-grant comparison checks
observations and after-state. Neither wall time nor OS thread IDs enter stable
traces. This assessment and passing tests are not a proof of all executions.

The two defects concern invalid custom factories and false search-completeness
claims, rather than races in the built-in scenarios. Fresh resources alone were
an insufficient factory contract: honoring forced choices, default policy, and
bounds is required too. The contract now states this explicitly. Recorded-event
checks cannot prove the absence of hidden state or certify wall-clock compliance.

## Verification and gaps

The two new cases failed against the unfixed explorer and passed after the fix.
All five CTest suites passed after rebuilding in Debug, standalone UBSan with
nonrecovering undefined-behavior checks, and optimized Release: 25 scheduler
cases, 12 exploration/replay cases, isolated exception-monitor and diagnostic
allocation regressions, and CLI integration.

```sh
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --build build-ubsan --parallel 4
ctest --test-dir build-ubsan --output-on-failure
cmake --build build-review-release --parallel 4
ctest --test-dir build-review-release --output-on-failure
```

ASan/TSan were not rerun; their previously documented local startup failures
remain unresolved, so race-sanitizer verification is unavailable. Linux CI and
other platforms were not run. The OS deadline and final timeout progress remain
nondeterministic; no forced driver-suspension experiment was performed. Worker
code and teardown must cooperate: arbitrary native waits or infinite local
loops can still prevent joining. Hidden untracked factory state remains outside
the guarantee. No benchmark claims are made.
