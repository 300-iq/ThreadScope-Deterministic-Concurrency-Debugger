# Architecture

## Scope and guarantee

ThreadScope explores the order of explicitly instrumented operations in small
C++ programs. It runs actual `std::thread` workers, but it does **not** intercept
arbitrary instructions, unmodified binaries, native locks, or external I/O.
Determinism requires deterministic, bounded worker-local computation between
instrumented operations, fresh state for each run, and no uncontrolled shared
state access. It guarantees the controlled operation order and recorded state
observations under those assumptions.

This guarantee is conditional on reaching those checkpoints within the run's
wall-clock budget. OS descheduling can cause a timeout even for a valid replay.
Timeout diagnostics, including the final progress snapshot, are not deterministic
model outcomes and are not claimed to be replayable.

The monitor introduces synchronization between operations. This is exploration
of operation interleavings, **not** a C++ weak-memory model checker. In particular,
it does not enumerate hardware reorderings or all relaxed-atomic outcomes of an
uncontrolled program. It is a bug finder within its explicit model and bounds,
not a proof of correctness for arbitrary concurrent C++ code.

## Components and ownership

- `threadscope_core`: scheduler, result/event types, bounded stateless DFS, and
  JSON encoding/decoding. No CLI, HTTP, browser, or UI dependencies.
- `threadscope_examples`: recreates the counter, tasks, and scheduler each run.
- `threadscope`: selects run/explore/replay/branch, checks arguments, writes traces, and
  reports exit codes. A static registry names lost-update, its RMW fix, and the
  two-mutex deadlock and consistent-order scenarios; JSON does not load arbitrary code.
- Tests: library scenarios plus CLI/file integration.
- `web/`: React/TypeScript trace inspection and a localhost Node adapter. The
  adapter invokes the fixed CLI in a child process, validates bounds, loads real
  all-run traces, and separately verifies replay. No frontend code changes the
  C++ scheduler. See [web-debugger.md](web-debugger.md).

The scheduler assigns contiguous IDs in registration order. The registration
table is frozen before launching workers. A `Context` is owned by its worker and
must not be shared. Each operation checks the caller's native thread ID under
the monitor before posting a request; foreign live-context use is rejected.
This check does not make escaped or dangling contexts safe. Resources must
outlive `run()` and belong to one run at a time.
Registering new threads during a run and reusing a scheduler are rejected. The
first implementation limits registration to 64 workers.

## Scheduling protocol

1. Launch every registered worker; initial state is `running`.
2. A worker submits its next built-in operation and parks on the scheduler's
   condition variable. It is now a pending request. Workers that return become
   `terminated`.
3. The driver waits until **every** worker is pending or terminated. This barrier
   is necessary: selecting the first worker to arrive would depend on OS timing.
4. Under the monitor, recompute each pending request's eligibility. A lock with
   an owner, or a join with unfinished targets, is `blocked`; eligible requests
   are `runnable`. Collect runnable IDs in ascending order.
5. Choose a forced schedule/replay entry, or the lowest runnable ID by default.
   An unavailable ID is a mismatch, never silently skipped.
6. Grant exactly one request. The selected real worker executes its built-in
   operation under the monitor and records its observation. No user callbacks
   execute under the monitor. Other workers remain parked.
7. The selected worker continues bounded local computation outside the monitor.
   The driver waits for its next request or termination before choosing again.

The same barrier is used for failure reporting. After a controlled failure,
the failing task unwinds before its wrapper publishes termination. Unselected
workers stay parked until the driver records the final state table; only then
does it cancel them. This prevents OS cancellation wakeup order from changing a
replayable failure outcome. Startup/local exceptions are collected per thread;
after the barrier, the lowest failing logical ID supplies the diagnostic.
User-overridable exception `what()` methods execute outside the monitor.
Diagnostic-copy allocation failures retain a per-thread error flag, with a
fallback message supplied by the driver instead of letting an exception escape
the worker wrapper.

The monitor is a real `std::mutex`; waiting uses a real `std::condition_variable`
with predicate checks. Grants, states, event history, and cancellation are
protected by that mutex. All worker threads are joined before `run()` returns.
There are no detached threads, spin polling, random arrival-order choices, or
unsynchronized accesses to scheduler bookkeeping.

## State and primitive model

`AtomicInt` holds `std::atomic<int>`. `load`, `store`, and `fetch_add` are each one
controlled operation, using relaxed atomics. The monitor supplies the execution
order. Separate load/store operations do not form a transaction: worker-local
values can become stale between them. The example's `local + 1` only operates on
0 or 1, so it does not risk signed overflow.

`ControlledMutex` represents ownership and eligibility. Lock acquisition and
unlock are controlled events. Workers never park inside a native mutex held by
another logical worker. A non-owner unlock is a recorded worker error; a
recursive acquisition blocks because the mutex is non-recursive. There is no
implicit unlock when a task terminates. If a terminated task retains a needed
mutex, remaining blocked tasks cause a deadlock result.

Pending requests also retain their awaited mutex or logical join targets under
the monitor. After refreshing eligibility at the quiescent barrier, blocked
requests produce sorted, unique directed dependencies from the waiting thread
to the mutex owner or unfinished join target. Both pre-selection events and
the final stop decision record this wait-for graph. The graph uses actual
resource identity internally; names are its serialized labels. A recursive
lock produces a self-edge. Waiting on a terminated lock owner is retained as a
dependency; joins omit terminated targets. Thus graph reporting does not infer
waiting from OS wakeups or depend on native lock acquisition order.

Cycle detection is a bounded graph traversal over at most 64 logical IDs.
Global deadlock detection remains the absence of enabled operations with
unfinished workers. A cycle can exist while an unrelated worker is still
runnable, and a retained owner can deadlock a run without a cycle. The CLI
distinguishes these facts. Lock acquisition itself is only an owner-state write
under the scheduler monitor; it never calls a native application mutex lock.

`join_all` is an instrumented wait for logical termination. It does not call a
blocking OS thread join from a worker. Empty target lists are immediately
eligible; invalid/self targets are rejected. The driver owns actual OS joins.

`assert_equal` reads at its selected step, records observed and expected values,
and terminates the run on failure. Assertions do not change the resource value.
`yield` is an explicit scheduling point without a state mutation.

## Events and replay

Each event contains a zero-based step, selected logical thread, ordered runnable
set, the state table **before** selection, operation kind, resource name, label,
and before/after observation. Assertion events also contain expected value and
pass/fail; operation errors carry a message. Names/labels are caller supplied,
so callers should use stable, unique resource names.
`EventType` classifies these operations as `shared_state`, `lock_acquire`,
`lock_release`, `assertion`, `join`, or `yield`. The JSON `type` is derived from
`operation`, so the two cannot disagree in a generated history; a conflicting
type in an input trace is rejected.

`Scheduler::track()` registers built-in atomic/mutex snapshot readers before
launching workers. The registry is frozen during a run and sorted by resource
name, so registration/OS arrival order cannot change snapshot order. Snapshots
cover all registered values/owners before and after each operation, and at the
stop decision. Register all relevant resources; the engine cannot discover
arbitrary shared memory. Old untracked callers still record operation observations.

Termination is reflected in subsequent state tables and the final outcome;
it is not an extra schedule choice. Final states are captured at the stop
decision, **before** cancellation cleanup, to retain blocked/stalled information.
There are no timestamps or platform-specific OS thread IDs in the trace.

Strict replay restores every selection. Before granting it, the driver compares
the runnable/state tables, pending operation/resource/label, and recorded shared
state before the operation. Metadata divergence stops before executing the
operation. Observations and shared state after the operation are checked after
execution; those divergences are not rolled back. `replay_trace()` additionally
compares every recorded event and final status/message/state table/shared state.
Missing, extra, unavailable, or divergent choices fail explicitly. Legacy v1
traces without shared snapshots validate all fields they actually recorded.
Recorded wait-for graphs are compared before granting each operation and at the
final outcome; legacy traces omitting graphs retain their original checks.
Timeouts and worker exceptions preserve their diagnostic status even when replay
choices remain; the CLI reports that verification could not finish instead of
claiming a schedule mismatch.

`program` is a manually versioned scenario ID: `lost-update/v1`,
`lost-update-rmw/v1`, `mutex-deadlock/v1`, or `mutex-ordered/v1`.
It is not a cryptographic binary/source fingerprint. Changed
code that produces different recorded behavior is rejected; source revisions
with identical observed behavior are indistinguishable in this milestone.
Timeout/environmental-error traces are diagnostic, not accepted for CLI replay.
There is no arbitrary code loading from JSON.

## Bounded exploration

Stateless DFS reruns a scenario factory from fresh resources for each forced
prefix. A spine holds one frame per step and references immutable decisions in
retained run results. Backtracking advances the deepest unvisited runnable ID,
popping exhausted frames; choices are visited in ascending ID order. The
remaining unforced suffix uses the lowest-ID policy. It never
changes per-thread program order. There is no partial-order reduction, fairness
filter, state hashing, schedule minimization, or random search yet.

Registered factories construct every resource/task/scheduler locally on every
call. Custom factories must do the same; the library cannot clone arbitrary C++
captures. Prefix checks detect changes in recorded history/pre-choice state and
stop with divergence instead of claiming an exhaustive search. They do not prove
the absence of hidden untracked state. Factories must also honor the supplied
configuration. The explorer rejects histories exceeding the requested step bound
or choosing a non-minimum runnable ID in the unforced suffix: its backtracking
would otherwise skip lower choices and falsely report exhaustion. These checks
run before the result callback. They detect observable violations, not every
possible factory error or elapsed-time-budget override.
The DFS spine is linear in depth; retained
run histories still consume memory proportional to recorded schedules/events.

The optional result callback runs after all workers join and before advancing
the next schedule. The CLI uses it to persist assertion/deadlock traces immediately.
If saving fails, exploration stops with an error; it never silently drops a trace.

Each run ends at step bounds, failure, completion, or deadlock; search backtracks
and continues until no alternatives remain or its schedule cap is reached.
`frontier_exhausted` describes the explored **bounded tree**, not correctness.
The CLI also marks a search `incomplete` if any run reached a safety limit or
environmental error. The six possible orders of the example's four update
operations are exhaustively covered; the checker can run only afterward.
`schedule_bound_reached` means an unvisited alternative remains at the cap;
`step_bound_reached` means at least one run was truncated. A cap equal to the
total number of leaves does not incorrectly report an incomplete search.

## Limits and cancellation

- Step bounds stop an instrumented infinite loop after the configured number of
  operations. Blocked workers are awakened by cancellation.
- No runnable requests with unfinished workers means a logical deadlock. This
  detects modeled lock/join dependencies, not arbitrary native deadlocks.
- A steady-clock deadline detects a worker failing to reach its next request or
  a run consuming too long. Condition-variable waits use that deadline.
- Failures and worker exceptions trigger cancellation and cleanup.

**A timeout is cooperative, not a hard wall-clock termination guarantee.** C++
has no safe way to kill a `std::thread`. After detection, `run()` still joins its
workers. Bounded local work may finish after the deadline; an infinite loop or
blocking native call outside the framework can prevent joining indefinitely.
Applications that accept untrusted/uncooperative tasks need subprocess isolation
and an external watchdog. The CLI runs our built-in factories; CTest imposes
outer process timeouts. Do not catch and suppress the
framework's private cancellation control flow with a catch-all in a task.
The bounded-work assumption also includes task destructors and exception
diagnostic callbacks. Framework operations can throw cancellation; task code
must not allow that exception to escape a `noexcept` destructor.

## Future work, outside this milestone

Add source/build fingerprints, schedule shrinking, additional modeled
synchronization primitives, and broader isolation before expanding workload
scope. The local API now runs the CLI in a child process with bounded options
and a watchdog, exposing recorded traces to React/TypeScript/Vite. Schedule
editing remains future work and must validate choices against actual execution.
JSON is the UI boundary, and the scheduler remains a separate C++ library.
Direct CLI/library callers retain cooperative joining limitations.

## What-if branching

`branch_config` derives a forced prefix from a structurally validated original
trace and one replacement choice. The CLI first strictly replays that entire
original, then runs the scenario factory again with fresh resources and workers.
The new scheduler validates all unchanged prefix events, including observations,
and checks recorded eligibility/state/dependencies at the branch checkpoint
before granting the alternative. After that choice, the ordinary lowest-runnable
ID suffix policy resumes. Exact replay retains its original separate contract.
The original is never used as mutable execution state or overwritten as output.
The Node API invokes this CLI path, separately verifies the new stable trace,
and returns both the real outcome and verification status. React compares actual
recorded checkpoints and never calculates a counterfactual execution. See
[what-if.md](what-if.md) for limits, fields and comparison semantics.

## Patch & Verify comparison

The localhost adapter maps one supported pair ID to two existing registered
programs. A single job guard encloses separate fresh DFS searches with identical
schedule, step and timeout limits. The existing exploration adapter validates
both real CLI outputs, and a shared comparison parser checks identities, settings,
counts and outcomes before returning/displaying them. No trace or execution state
is supplied to the other implementation. React reports assertion failures and
deadlocks separately from real terminal outcomes and retains implementation-bound
replay/inspection. The C++ engine and trace schema remain unchanged. See
[patch-verify.md](patch-verify.md) for the API, limits and demonstration workflow.
