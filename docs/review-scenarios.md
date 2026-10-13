# Review of scenario library, wait-for graphs, and typed events

Scope: the previous milestone's consistent lock-order scenario, pending mutex/
join bookkeeping, graph construction and cycle traversal, event categories,
graph/category JSON parsing, graph-aware DFS/replay checks, CLI reporting, and
associated tests and claims. Existing scheduler barriers and cancellation were
inspected specifically for interactions with those changes. A source snapshot
was preserved before this review.

## Findings

No critical correctness defect, new data race, or deadlock in the supported
implementation was identified. No production-code fix was made, and no new
feature was added. This is a review result supported by the checks below, not a
proof of race freedom or determinism for every execution.

The graph is built under the scheduler monitor, after eligibility refresh at
the all-worker barrier for stable model outcomes. Pending mutex pointers refer
to resources required to outlive the run; pending join targets are validated
before publication. Only blocked slots contribute dependencies. Terminated
join targets are removed, while terminated mutex owners remain meaningful.
Selected/running or terminated slots do not expose stale pending wait metadata.
All final model state is copied before cancellation wakes other workers, so OS
cancellation order cannot alter that recorded graph.

Lock readiness and acquisition remain separate controlled phases, both under
the same monitor, with no intervening application operation. An acquisition is
an ownership write, not a blocking native application mutex call. Global
deadlock remains no enabled operation with unfinished workers; a cycle alone
does not terminate independent runnable work. The consistent-order factory
constructs fresh locks for every invocation and releases both.

Before-grant replay checks include recorded dependencies. Full-result replay
also compares event and final graphs, including when a custom factory ignores
the expected-event configuration. Legacy traces omitting graph/category fields
retain their existing checks. Structural graph parsing intentionally does not
certify complete program behavior: missing or changed dependencies in an
otherwise structurally valid graph are rejected by execution/replay comparison.
The existing tests exercise both structural rejection and this distinction.

No new unsupported claim of universal race freedom was found. Timeouts are
already documented as environmental, potentially stale progress snapshots;
the graph built on that diagnostic path is outside deterministic replay.

## Added verification

Two coverage gaps were addressed with tests against the unchanged implementation:

- `mixed_wait_graph_ignores_startup_timing` constructs a mutex/join cycle and
  alternates a three-millisecond bounded delay before either worker's initial
  request. Ten runs and ten replays with the opposite delay compare complete
  results; encoded run JSON must also match. The test does not assume a native
  arrival order or a precise timer wakeup. A five-second cooperative budget
  keeps the deliberate delay separate from the deadline being tested.
- `maximum_thread_id_and_dense_join_graph` parks all 64 logical threads on joins
  of their 63 peers. It checks a zero-event deadlock, all blocked states, all
  4032 edges, highest ID 63, JSON round-trip, and strict replay. It also checks
  that shared successors in an acyclic diamond do not produce a false cycle.

Both passed without implementation corrections. Existing tests retain repeated
search and replay of all four scenarios, blocked-to-runnable transitions,
recursive/retained-owner locks, progress alongside a cycle, malformed graph and
category rejection, before-grant divergence, final-graph comparison, old trace
compatibility, and CLI replay/error reporting.

## Verification actually run

After rebuilding, all six CTest suites passed in Debug, standalone nonrecovering
UBSan, and optimized Release: 25 scheduler cases, 12 exploration/replay cases,
10 scenario cases, isolated exception-monitor and diagnostic-allocation tests,
and CLI integration.

```sh
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --build build-ubsan --parallel 4
ctest --test-dir build-ubsan --output-on-failure
cmake --build build-review-release --parallel 4
ctest --test-dir build-review-release --output-on-failure
```

## Remaining unverified behavior

ASan/TSan were not rerun; the documented local sanitizer-runtime startup problems
remain unresolved. Passing UBSan does not verify the absence of data races.
Linux CI and other platforms were not executed. No experiment forcibly suspended
the scheduler driver or exercised every allocation-failure point. The startup
delay tests perturb timing but cannot cover every possible OS scheduling pattern;
an OS stall beyond the cooperative budget can still produce a timeout.

Resources must outlive the run, contexts must stay on their owning workers, and
factories must honor configuration and reset their state. Hidden untracked
memory, native synchronization, external I/O, weak-memory reorderings, and
uncooperative task/teardown code remain outside the supported deterministic
model. Joining uncooperative native threads may still hang. No benchmark or
universal race/deadlock-freedom claim is made.
