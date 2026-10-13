# Validation history

The current full-repository audit passed 8/8 CTest suites in strict Debug,
Release, and nonrecovering standalone UBSan, plus 69/69 web tests and the production
TypeScript/Vite build. ASan/UBSan and separate TSan compiled but failed independent
runtime and engine-help probes; their full scans remain unverified. The benchmark
executed 160 schedules and verified 300/300 failing-trace replays.
See [the current audit](audit.md) for exact commands, reproductions, sanitizer
diagnostics and gaps, and [benchmark methodology](benchmark.md) for real measured
timings. The sections below retain historical milestone results; their counts
and statements about checks not yet rerun describe those earlier milestones.

Local environment: macOS arm64, Apple Clang 17.0.0, CMake 4.2.3.

## Passing checks

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure

cmake -S . -B build-ubsan -DCMAKE_BUILD_TYPE=Debug -DTHREADSCOPE_SANITIZER=undefined
cmake --build build-ubsan --parallel 4
ctest --test-dir build-ubsan --output-on-failure
```

At this milestone, the exploration and scenario-library implementation passed all six CTest suites in
Debug, standalone UBSan, and optimized Release: 25 scheduler cases, 12
exploration/replay cases, 10 scenario-library cases, the isolated exception-monitor deadlock regression,
diagnostic-allocation fault injection, and CLI integration. UBSan runs with
`-fno-sanitize-recover=undefined` so findings
fail a test. The optimized Release configuration also passes:

```sh
cmake -S . -B build-review-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-review-release --parallel 4
ctest --test-dir build-review-release --output-on-failure
```

The initial milestone had 22 library cases and no isolated exception-monitor
case. New regressions reproduced failures before the corresponding fixes;
see [review.md](review.md) for the evidence and remaining gaps.
The subsequent review of bounded exploration reproduced and fixed two false
exhaustion claims from factories overriding the default policy or step bound;
see [review-exploration.md](review-exploration.md) for its scope and evidence.

Actual CLI observations:

```text
run --schedule 0,0,1,1: completed, counter=2
run --schedule 0,1,0,1: assertion_failed, counter=1
explore: runs=6 failures=4 frontier_exhausted=true incomplete=false
replay failing JSON: every event and final outcome match
explore --fixed: runs=2 failures=0 frontier_exhausted=true incomplete=false
explore mutex-deadlock: runs=6 failures=2 frontier_exhausted=true incomplete=false
explore mutex-ordered: runs=2 failures=0 frontier_exhausted=true incomplete=false
```

All four exploration-generated failure files are replayed by the integration
test. Changed event values, outcomes, program IDs, and schema versions are
rejected. The core suite includes 100 identical replays of one failing schedule.
The CLI suite also reproduces a step-limit outcome; the library suite replays
a modeled deadlock and verifies blocked-thread states.

The exploration suite compares all six lost-update schedules and their encoded
JSON across 20 repeated clean searches, replaying each result. It checks three-way
DFS order, variable-length histories, terminated workers, exact/cutoff schedule
bounds, step-bounded loops, empty programs, prefix divergence from leaked state,
and trace persistence before the next run starts. The mutex scenario's four
completed and two deadlocked schedules match independent lock-sequence enumeration;
each is replayed ten times. The CLI tests cover named scenarios, aliases, new
shared snapshots, malformed snapshots, modified runnable sets/operations/values,
legacy-v1 replay, partial searches with saved failures, and error exit codes.

These tests exposed and fixed a previous bug rejecting valid forced prefixes
shorter than a deadlocked execution. Unused choices are still rejected.

The scenario suite adds consistent lock ordering and explicitly checks directed
mutex-owner/join dependencies, blocked-to-runnable transitions after release,
retained owners, recursive self-locks, cycle classification with an independent
runnable worker, and invalid dependency/category fields. Its third-worker test
executes three yields while the two lock requests stay blocked, demonstrating
that those waits do not consume the execution permit. All four registered
programs are searched five times with full-result/encoded-JSON equality; every
result is replayed three times in each repetition. A concrete opposite-order
deadlock is also replayed 20 times. The CLI replays the failing and corrected
lock examples five times each, rejects changed wait-for metadata before granting
the next acquisition, and checks legacy traces without graphs or event categories.
The exact scenario schedules, bounds, and limitations are in [scenarios.md](scenarios.md).
The subsequent focused review added mixed mutex/join cycles with alternating
startup delays, a 64-thread/4032-edge join graph, and an acyclic graph with shared
successors. Those cases passed against the existing implementation; no critical
defect or production-code correction was identified. See [review-scenarios.md](review-scenarios.md).

## Unavailable sanitizer checks on this machine

In the original milestone, AddressSanitizer/UBSan and ThreadSanitizer
configurations compiled, but their executables could not run successfully here,
even with sandbox restrictions lifted:

- AddressSanitizer's `threadscope --help` stalls after libc interceptor
  initialization, before reaching `main`. CTest's outer timeouts stop its tests.
- ThreadSanitizer's `threadscope --help` segfaults before `main`. macOS crash
  reports show `__tsan::SlotLock`, `wrap_dispatch_once`, dyld enumeration, and
  `__tsan::InitializePlatform` in its startup stack.

These were startup/runtime failures, not passing sanitizer checks or reports of
bugs found in the scheduler. ASan/TSan were not rerun against the reviewed code;
we do not claim local ASan/TSan verification.
Standalone UBSan runs successfully. The Linux sanitizer CI jobs are configured
but have not been run as part of this local milestone.

No performance benchmarks or cross-platform results are claimed.

## Visual debugger milestone

The CLI's optional `--all-traces` output passed all six CTest suites in Debug,
UBSan, and Release. The TypeScript/Vite build and all 38 parser/navigation and
real-engine API tests passed. Browser verification covered actual counter/lock
exploration, assertion/deadlock checkpoints, replay, bounded/no-failure states,
and responsive geometry. See [web-debugger.md](web-debugger.md) for commands,
scope, and remaining gaps.

## What-if milestone

All six CTest suites passed after the prefix/branch changes in Debug, Release,
and standalone nonrecovering UBSan. The scenario suite now has 13 cases. New
checks include lost-update failure-to-pass and pass-to-failure, 20 identical
branches/replays in both directions, pre-permit prefix/checkpoint divergence,
nonrunnable choices with actual runnable-set diagnostics, zero-length preserved
prefixes, step-bound replay and modeled deadlock-to-completion. Another case
alternates worker startup and between-operation delays, requiring identical
branch histories independently of native request arrival. CLI integration
verifies separate trace persistence, strict original validation, invalid inputs,
bounded outcomes and refusal to overwrite originals through direct paths,
symlinks or hard links.

The production TypeScript/Vite build passed and all 46 frontend/API tests passed.
HTTP tests use the current C++ executable, repeat both counter outcome changes,
verify the new traces through independent replay, reject edited originals and
invalid request fields, reproduce rejected choices, and preserve step bounds.
Comparison tests cover enabled/rejected alternatives, first divergence, shared
and lock state differences, missing metadata and absent checkpoints in shorter
histories. Browser automation at the built API origin confirmed both counter
changes, reset, disabled blocked choices, lock/dependency differences, no invented
checkpoints past deadlock, and stacked mobile comparisons without page overflow.

ASan/TSan were not rerun; their local startup failures above remain unresolved.
Linux CI and forced mid-run disconnect/watchdog overruns remain unverified.
See [what-if.md](what-if.md) for the implemented workflow and deterministic scope.

## Patch & Verify milestone

All six CTest suites passed in the existing Debug build; the C++ engine and its
scenarios were unchanged. Strict TypeScript checking, the production Vite build,
and all 53 frontend/API tests passed. New real-binary tests exercise both
Broken/Corrected pairs with identical settings, separate assertion/deadlock
counts, fresh initial state, repeated trace equality, original-program replay,
explicit cross-implementation divergence, schedule/step caps, input rejection,
pre-abort cleanup and a single guard across both searches. Shared parser tests
reject mixed settings, program identities and fabricated counts; initial UI
rendering requires prominent limitations and unknown metrics before any run.
Browser automation verified both pairs, broken assertion/deadlock replay,
corrected selection, preserved comparisons after debugger inspection, capped
search messaging, disabled failure replay when no bug was observed, and mobile
layout. See [patch-verify.md](patch-verify.md) for the implementation contract and
short presentation walkthrough. No new sanitizer, Linux CI, watchdog-overrun or
mid-run disconnect results are claimed for this UI/API milestone.
