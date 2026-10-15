# Architecture

## Components

| Component | Responsibility |
| --- | --- |
| `threadscope_core` | Scheduler, controlled operations, DFS, trace encoding and replay |
| `threadscope_examples` | Four scenario factories with fresh resources/tasks per execution |
| `threadscope` | Independent CLI for run, explore, replay, and branch |
| `web/server` | Local HTTP adapter that invokes the fixed C++ executable |
| `web/shared` | Trace/request validation and implementation-pair metadata |
| `web/src` | Timeline, state inspection, What-If, and implementation comparison |

## Scheduling protocol

Logical IDs are assigned in registration order, with at most 64 workers. The
registration table is frozen before native threads start. A scheduler is single
use. Each context belongs to its registered native worker; foreign-thread use is
rejected before posting an operation. Contexts and referenced resources must not
escape their lifetimes. Registration and `run()` are single-owner APIs.

1. Every worker starts in `running` and executes deterministic, bounded local work.
2. An instrumented operation posts a pending request and waits on the scheduler's
   condition variable. Returning from the task publishes `terminated`.
3. The driver waits until every worker is pending or terminated.
4. Under the monitor, it recomputes runnable and blocked requests and collects
   enabled logical IDs in ascending order.
5. It validates a forced/replayed choice, or selects the lowest runnable ID.
6. The selected worker executes one bounded built-in action under the monitor;
   its event records before/after observations and registered resource snapshots.
7. The worker resumes local computation. No further choice occurs until all
   workers are quiescent again.

The monitor protects requests, grants, states, cancellation, and event history.
Native request arrival order cannot change the decision set. Relaxed atomic
operations are race-free and ordered by monitor handoffs; this does not model
the relaxed-memory behaviors of an uncontrolled C++ program.

## Operations and dependencies

Supported operations are atomic load/store/fetch-add, assert-equal, yield,
modeled mutex lock/unlock, and logical join. Register relevant resources using
`track()` before `run()`. Unique resource names give sorted snapshots. Untracked
resources retain operation observations but have no full-state snapshot.

Modeled mutex acquisitions remain pending while owned. Acquiring and releasing
ownership are nonblocking scheduler-controlled transitions. Workers never wait
inside a native application mutex while holding an execution permit. A non-owner
unlock is a worker error; recursive acquisition blocks on the same owner.

Logical join waits for registered target workers to terminate. Wait-for edges
describe unfinished joins and mutex owners. Deadlock requires unfinished workers
with no enabled operation. A cycle is reported separately: another runnable worker
can still make progress, and a terminated worker retaining a lock can cause
deadlock without a cycle.

## Exploration and replay

DFS advances the deepest remaining runnable alternative in ascending ID order.
Each leaf starts a fresh factory invocation. A spine references immutable prior
decisions rather than copying every candidate prefix. Recorded runnable states,
forced prefixes, lowest-ID default choices, and step limits are checked for drift.
Custom factories remain responsible for hidden local/global state reset.

`max_schedules` counts executions and `max_steps` counts controlled operations.
A final leaf at the schedule cap can exhaust the frontier. Step-limited leaves
remain unfinished even when the bounded frontier is exhausted. Environmental
errors stop search without declaring exhaustion. Full histories are retained;
search has exponential cost and no partial-order reduction.

Replay structurally validates the trace before execution. At each decision it
checks the runnable set, thread states, operation/resource/label, and available
before snapshots/dependencies before granting a permit. It then compares the
observation and after snapshot, followed by the complete terminal outcome.
There is no fallback choice, and divergence after mutation cannot be rolled back.
Legacy schema-v1 traces omit some metadata; replay checks the fields present.

What-If first verifies the original trace, then executes a fresh scenario with
the original prefix and one new choice. The prefix and branch checkpoint are
validated before permits. The suffix selects the lowest runnable ID. Original
and modified traces are stored separately and replayed independently.

Patch & Verify explores two registered implementations with identical bounds.
Only search settings are shared; a broken trace is not reused against a different
implementation. Event counts, operations, and enabled sets can change.

## Failure handling

The driver snapshots the stop state before cancellation. Unselected workers stay
parked until that snapshot, preserving failure-state determinism. Exceptions are
collected per worker; the lowest failing logical ID supplies a startup diagnostic
after the barrier. User-overridable exception diagnostics execute outside the
monitor. An allocation-free flag preserves errors when diagnostic copying fails.

Cleanup marks stopping, wakes waiters, and joins every successfully launched
native thread outside the monitor. Deadlines are checked after waiting even if a
predicate becomes true while the driver is descheduled. Timeouts and allocation/
thread-creation failures are environmental diagnostics, outside stable replay.
In-process cancellation cannot terminate infinite local work, native blocking,
blocking destructors, or code that suppresses cancellation indefinitely.

## Local API

| Route | Input |
| --- | --- |
| `GET /api/scenarios` | Catalog descriptions read from CLI help |
| `POST /api/explore` | `{scenario, maxSchedules, maxSteps, timeoutMs}` |
| `POST /api/replay` | `{trace}` |
| `POST /api/branch` | `{trace, step, thread}` |
| `POST /api/patch-verify` | `{scenario, maxSchedules, maxSteps, timeoutMs}`; pair is `counter` or `lock-order` |

The server binds to `127.0.0.1`, checks Host/Origin, requires JSON POST bodies,
rejects unknown fields, and limits requests to 8 MiB. It invokes only the fixed
`build/threadscope` executable using an argument array with `shell:false`.
Requests cannot select an executable, filesystem path, or shell command.

One exploration/replay/branch/comparison job runs at a time. Each job uses an
isolated temporary directory removed in `finally`. A 30-second child-process
watchdog and disconnect signal terminate unfinished executions. The adapter waits
for child closure before releasing a killed job. Catalog queries have a separate
five-second watchdog.

Exploration accepts 1–100 schedules, 1–256 steps, and 100–5000 ms per run. Replay
accepts at most 1000 configured steps and 5000 ms. The viewer caps traces at 8 MiB
and 1000 events. Output caps are 16 MiB of traces per job, 512 KiB stdout, and
128 KiB stderr. Summary counts, exit codes, trace identities and bounds must agree.

UI checkpoints show before-state with its corresponding wait dependencies; terminal
inspection uses each trace's own stop state. Shorter histories have absent
checkpoints rather than extrapolated values. A replay badge requires a real C++
verification result. This is a trusted local developer service, not an authenticated
remote execution service. See [trace-format.md](trace-format.md) for schema details.
