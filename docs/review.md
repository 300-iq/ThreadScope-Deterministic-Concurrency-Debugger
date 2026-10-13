# Skeptical review of milestone 1

Scope: the previous milestone's scheduler, example, explorer, JSON/replay code,
CLI, tests, and claims. No frontend, API, or new scheduler primitives were added.
The original sources were preserved before review. Regressions were executed
against unfixed behavior before applying the corresponding corrections.

## Findings and reproductions

### P1: Cancellation changed failure snapshots according to OS wakeup order

`perform()` woke parked workers when `failure` became true. Those workers could
publish termination before the driver copied `final_states`. The driver also
bypassed the quiescent barrier on failure and the failing operation marked its
worker terminated before task unwinding. Event choices could match perfectly
while the full assertion-failure result differed across replay.

`failure_snapshot_precedes_cancellation` fails one worker while a join request
and 30 yield requests are parked. It checks the pre-cancellation states and
compares 25 full-result replays. Against the old code, a replay result differed
from the original result. The old two-increment example concealed this problem
because its other workers had already terminated before its checker asserted.

Fix: parked workers wait for a grant or the driver's `stopping` flag, not the
failure flag. The driver waits for quiescence before reporting failures, and
actual task termination is published by the wrapper after unwinding. It copies
the final state table before starting cancellation cleanup.

### P1: User-defined exception diagnostics could deadlock the scheduler monitor

The wrapper called virtual `std::exception::what()` under the monitor. A custom
implementation waiting for cleanup could prevent the driver from acquiring the
same monitor to enforce its deadline and cancel workers.

`exception_what_does_not_hold_monitor` uses two latches. One worker's exception
diagnostic waits for the other worker's cleanup. That cleanup needs cancellation
of its instrumented yield. There are no sleep-based guesses about who wakes
first. The old code hangs; the isolated CTest case hit its five-second outer
timeout. With the fix, the driver's deadline cancels the waiter, whose cleanup
releases the diagnostic callback, and both threads are joined.

Fix: copy exception diagnostics outside the monitor; publish the copied error
and termination atomically under the monitor afterward. Uncooperative diagnostics
can still prevent joining, just like any unbounded task code; the monitor itself
is no longer held while calling them.

### P1: Allocation while copying an error diagnostic could abort the process

The error handler assigned `what()` to a string without containing failures of
that assignment. A `bad_alloc` while copying a long diagnostic escaped the
worker wrapper and called `std::terminate`, ending the entire process instead
of reporting a worker error and cancelling/joining its other workers.

`diagnostic_allocation_test` injects allocation failures in just the throwing
worker after `what()` returns its long message. Before the fix, CTest reported
an aborted subprocess with an uncaught `std::bad_alloc`. The test uses its own
isolated executable so the allocation hook does not affect other tests or the
product allocator.

Fix: preserve an allocation-free per-thread error flag, contain failures while
copying diagnostics, and let the driver supply a fallback message. The test
verifies the injection occurred, a `worker_error` result is returned, no operation
was executed, and the pending worker was cancelled/joined normally. This is a
targeted diagnostic-copy test, not a general process-wide OOM resilience claim.

### P2: Startup exception selection used arrival order instead of logical IDs

Concurrent exceptions competed to set a single global failure message. Switching
which worker delayed its startup changed the reported error for otherwise
identical logical errors. The driver could also snapshot another worker still
running instead of collecting a stable checkpoint.

`startup_errors_are_not_arrival_ordered` alternates which worker delays and
requires the same outcome naming worker 0. The old code reported worker 1 when
worker 0 was delayed.

Fix: collect errors per logical thread, publish each with termination, and choose
the lowest failing ID after the barrier. Deadline-triggered interruptions remain
environmental diagnostics, not stable model outcomes.

### P2: Replay masked environmental failures as schedule mismatches

The final exact-schedule check replaced a timeout with `replay_mismatch` whenever
choices were unconsumed. The earlier failure branch did the same to worker errors.
The CLI then called any different outcome a replay mismatch. A correct program
on a delayed host could therefore appear to have diverged from the trace.

`replay_timeout_keeps_environmental_status` replays a recorded yield after bounded
local delay exceeds the replay deadline. The old result was `replay_mismatch`,
not `timeout`. It also checks that a local exception preserves `worker_error`.

Fix: retain timeout/worker-error statuses and their diagnostics; the CLI reports
that replay could not be verified. The deadline check now precedes terminal-state
classification, so a driver that resumes past the budget cannot bypass it merely
because the barrier predicate is true.

### P2: A deadlock silently discarded a forced schedule's remaining choice

The deadlock branch checked unused choices only for an exact replay, not for a
forced prefix. `deadlock_detection` now also requests prefix `0,1,0` in the
two-lock cycle: after `0,1`, both workers are blocked and the third choice cannot
run. The old result was `deadlock`, silently ignoring the requested third choice.

Fix: reject an unconsumed forced prefix when reporting deadlock. Valid deadlock
traces and deadline/step-limit interruption semantics remain covered by tests.

## Test and claim corrections

The timeout test previously required its final state to be exactly `running`.
If the driver is descheduled until the worker submits its request, `runnable`
is also a valid timeout snapshot. The test now checks the deadline, absence of
executed events, and the permitted progress states instead of assuming an OS
wakeup order. The determinism contract now explicitly excludes timeout progress
snapshots and requires runs to reach checkpoints within their wall-clock budget.

## Verification actually run

Before fixes: the main library suite failed the failure-state replay, startup
error ordering, and replay-timeout regressions; the isolated monitor test timed
out. The new non-exact deadlock-prefix check also failed before its fix.
The diagnostic-allocation regression aborted the process before its fix.

After fixes: Debug, standalone UBSan, and optimized Release each passed all
four CTest suites: 25 library cases, the isolated monitor regression, isolated
diagnostic-allocation fault injection, and CLI integration. The original
exploration still visits six schedules, finds four
lost updates, replays all four failure files, and passes both RMW-fix schedules.
The library tests retain 100 replays of the original lost-update scenario and
add 25 full-result replays of the early-failure scenario with 32 workers.

```sh
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure

cmake --build build-ubsan --parallel 4
ctest --test-dir build-ubsan --output-on-failure

cmake -S . -B build-review-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-review-release --parallel 4
ctest --test-dir build-review-release --output-on-failure
```

## Remaining limits and verification gaps

- ASan/TSan execution remains unverified because of the previously documented
  runtime startup failures on this machine. This review does not claim sanitizer
  race detection; successful UBSan checks are not a substitute for TSan.
- Linux CI has not run. Local Debug/Release results are for Apple Clang on macOS
  arm64, not evidence of all-platform correctness.
- Worker code, destructors, and exception diagnostics must cooperate and return.
  No hard termination of arbitrary uncooperative threads is provided; joining can
  still hang in that unsupported case.
- The actual CLI runtime-timeout/error reporting branch was changed, but the
  built-in scenario was not forcibly descheduled or made to throw through the
  CLI during testing. Its underlying library statuses are regression tested;
  CLI stable replay, changed outcomes, malformed inputs, and step limits pass.
- Deadline detection depends on wall time and OS progress. The late-driver
  deadline ordering was reviewed in code, not verified by forcibly suspending
  the driver. No deterministic elapsed-time or timeout-snapshot claim is made.

No undefined shared-state data race was identified in the supported built-in
operations during this review. That observation is not a proof of race freedom
for arbitrary task code or all executions.
