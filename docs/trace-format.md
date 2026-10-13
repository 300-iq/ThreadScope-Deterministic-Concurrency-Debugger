# Trace format, schema version 1

The JSON document is both a human-readable execution history and the input for
strict replay. See `artifacts/lost-update.json` after running the README demo for
a complete generated trace.

| Field | Meaning |
| --- | --- |
| `schema_version` | Integer 1; other versions rejected |
| `program` | Versioned, built-in scenario identifier |
| `limits.max_steps` | Positive controlled-operation bound |
| `limits.timeout_ms` | Positive cooperative deadline duration |
| `schedule` | Selected logical thread IDs, in execution order |
| `events` | One event per controlled operation |
| `events[].type` | Derived event category; optional on legacy input |
| `events[].wait_for` | Directed wait dependencies at the pre-selection checkpoint |
| `events[].shared_state_before` | Sorted registered resource snapshots before the grant (optional for legacy v1) |
| `events[].shared_state_after` | Same resources after execution (optional for legacy v1) |
| `outcome.status` | Completion, failure, or stop reason |
| `outcome.message` | Assertion label or diagnostic message |
| `outcome.threads` | State table at the stop decision, before cleanup |
| `outcome.shared_state` | Registered resource snapshots at the stop decision (optional for legacy v1) |
| `outcome.wait_for` | Directed wait dependencies at the stop decision (optional for legacy v1) |

An actual first load event from a lost-update trace has this structure:

```json
{
  "step": 0,
  "thread": 0,
  "runnable": [0, 1],
  "threads": [
    { "id": 0, "state": "runnable" },
    { "id": 1, "state": "runnable" },
    { "id": 2, "state": "blocked" }
  ],
  "operation": "load",
  "type": "shared_state",
  "resource": "counter",
  "label": "read counter",
  "wait_for": [
    { "thread": 2, "waits_for": 0, "kind": "join", "resource": "" },
    { "thread": 2, "waits_for": 1, "kind": "join", "resource": "" }
  ],
  "shared_state_before": [{ "name": "counter", "kind": "atomic_int", "value": 0 }],
  "shared_state_after": [{ "name": "counter", "kind": "atomic_int", "value": 0 }],
  "observation": {
    "before": 0,
    "after": 0,
    "expected": null,
    "assertion_passed": null,
    "error": null
  }
}
```

`before`/`after` refer to the named resource. A load has identical values; a
store/fetch-add records its actual change. Lock ownership uses logical IDs, with
null denoting no owner. Join/yield have null values. An assertion has identical
before/after values and a separate expected value, so failure does not imply a
state mutation. The corresponding final failed assertion records before=1,
after=1, expected=2, assertion_passed=false.

Shared state is an array of `{name, kind, value}` records, sorted by unique
nonempty resource name. `kind` is `atomic_int` or `mutex`; `value` is an int for
atomics, and an owner logical thread ID or null for mutexes. Each event has both
before/after snapshots, with the same names/kinds as `outcome.shared_state`.
This is an additive schema-v1 extension: existing field names remain unchanged.
Older traces omit all three snapshot fields and remain replayable with the
original operation-observation checks. Snapshots are not inferred for missing
legacy fields. Mixed/partial snapshots, changed layouts, and invalid owner IDs
are rejected. Full snapshots require explicit `Scheduler::track()` registration;
all built-in CLI scenarios register every relevant resource.

Operation kinds: `load`, `store`, `fetch_add`, `assert_equal`, `yield`, `lock`,
`unlock`, `join`. Thread states: `running`, `runnable`, `blocked`, `terminated`.
Scheduling events are quiescent and cannot contain `running`; timeout outcomes
can include a worker still running at the stop decision.

Event categories (`type`): `shared_state` for load/store/fetch-add,
`lock_acquire` for lock, `lock_release` for unlock, `assertion` for assert-equal,
and `join`/`yield` for their corresponding operations. A supplied category must
match the operation. It is inferred for old inputs and emitted on new output;
the original operation names remain unchanged.

New scheduler results always include `wait_for` arrays on every event and the
outcome, even when empty. A graph is sorted by waiting thread and target, with
unique edges. Each edge is `{thread, waits_for, kind, resource}`. `kind` is
`mutex` or `join`; `resource` names a mutex and is empty for a join. Sources must
be blocked, targets must be registered logical IDs, and a recorded mutex owner
must agree with the edge when that resource is included in shared snapshots.
Join targets must be unfinished and distinct from the waiting thread. Multiple
join dependencies are allowed; a mutex waiter has exactly one owner when blocked.
Partial graph presence, duplicate/unsorted edges, unknown kinds, invalid IDs,
and inconsistent ownership are rejected. Old traces omit all graph fields;
replay compares the fields that were recorded, without inventing dependencies.

For the opposite-order deadlock with schedule `0,1`, the final graph is:

```json
"wait_for": [
  { "thread": 0, "waits_for": 1, "kind": "mutex", "resource": "second" },
  { "thread": 1, "waits_for": 0, "kind": "mutex", "resource": "first" }
]
```

Event graphs describe the same **before-selection** checkpoint as their thread
table, not the after-state snapshot. The next pending request becomes known at
the next barrier; if no next operation is enabled, its dependencies appear in
the final outcome. The final graph above therefore appears in `outcome` after
the two recorded acquisitions, rather than as a fictitious executed lock event.
Timeout state/graph snapshots are diagnostics and may reflect stale eligibility
while a worker is still progressing; they are outside deterministic replay.

Statuses: `completed`, `assertion_failed`, `deadlock`, `step_limit`, `timeout`,
`replay_mismatch`, `worker_error`. The CLI replays completed, assertion-failure,
deadlock, and step-limit outcomes for registered scenarios. Reproducing a bug
retains its failure exit code, even though replay verified successfully.

The decoder validates types, integer ranges, contiguous IDs/steps, schedule/event
agreement, runnable-set consistency, operation/state names, and required fields.
It limits input to 8 MiB, 64 logical threads, 1,000,000 steps, and 60,000 ms.
Extra fields are tolerated for metadata; unknown schema versions are rejected.
JSON object keys must be unique, including keys with equivalent escaped spellings,
and nesting is limited to 32 containers. The UI/API uses the same ambiguity/depth
policy. In stable outcomes, no worker can be running and no operation can carry
an error. Completed requires all workers terminated; assertion failure requires
exactly one failed assertion as the final event; deadlock requires blocked unfinished
workers with none runnable; step-limit requires the configured event count and
runnable unfinished work. Assertion observations must agree with their observed
and expected values. Other stable outcomes cannot contain a failed assertion.
Saving validates these constraints before opening/truncating a file, although
filesystem writes are not transactional against interruption or disk errors.
Structural validation does not certify a trace's behavior: replay compares it
with execution, including the final outcome. Runnable sets, request metadata,
and shared state before execution are checked before a grant; observation/after
state divergence is checked afterward and cannot be rolled back.
Editing only `schedule` is rejected
if it no longer agrees with `events`; use `run --schedule ... --trace ...` to
generate a new trace for a modified schedule.
