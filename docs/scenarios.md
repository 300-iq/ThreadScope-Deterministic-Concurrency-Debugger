# Instrumented scenario library

All four factories create fresh resources, workers, and a scheduler per run.
Workers execute real C++ threads, but shared-state access and logical lock
transitions pass through the scheduler. The counter uses atomics, including
in the intentionally incorrect split-increment program: no undefined C++ data
race is introduced. The library and CLI use the same registered factories.

## Observed bounded results

| Scenario | Max schedules | Max steps | Explored | Completed | Assertions | Deadlocks |
| --- | --- | --- | --- | --- | --- | --- |
| `lost-update` | 6 | 6 | 6 | 2 | 4 | 0 |
| `lost-update-rmw` | 2 | 4 | 2 | 2 | 0 | 0 |
| `mutex-deadlock` | 6 | 8 | 6 | 4 | 0 | 2 |
| `mutex-ordered` | 2 | 8 | 2 | 2 | 0 | 0 |

The search exhausts the modeled tree at these bounds for these exact finite
programs. No partial-order reduction is applied. Passing bounded exploration
does not prove the corrected programs universally race-free, deadlock-free,
or correct under arbitrary native synchronization or weak-memory execution.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/threadscope explore lost-update --max-schedules 6 --max-steps 6 --out artifacts/scenarios/counter-failures
./build/threadscope explore lost-update-rmw --max-schedules 2 --max-steps 4
./build/threadscope explore mutex-deadlock --max-schedules 6 --max-steps 8 --out artifacts/scenarios/lock-failures
./build/threadscope explore mutex-ordered --max-schedules 2 --max-steps 8
```

Execute commands separately: finding an assertion or deadlock returns exit code
1 intentionally; a cap, invalid input, environmental error, or divergence returns
2. Successful corrected exploration returns 0.

## Counter schedules

Thread 0 and thread 1 each load the counter, then store their local value + 1.
Thread 2 joins them and asserts counter == 2. Schedule `0,1,0,1,2,2` reads 0
twice and stores 1 twice, so the assertion fails with counter=1. Schedule
`0,0,1,1,2,2` finishes the first increment before the second load and passes
with counter=2. The CLI accepts the first four choices as a prefix and appends
the checker's only valid choices.

```sh
./build/threadscope run lost-update --schedule 0,1,0,1 --trace artifacts/scenarios/lost-update.json
./build/threadscope replay artifacts/scenarios/lost-update.json
./build/threadscope run lost-update --schedule 0,0,1,1 --trace artifacts/scenarios/counter-pass.json
./build/threadscope replay artifacts/scenarios/counter-pass.json
```

The RMW variant replaces both split operations with `fetch_add(counter, 1)`.
Its complete schedules are `0,1,2,2` and `1,0,2,2`, both passing with counter=2.

## Lock schedules and waiting

The incorrect program acquires `first` then `second` in thread 0, and `second`
then `first` in thread 1. Both release in reverse order after acquiring both.
Schedule `0,1` grants each worker its first acquisition; their next requests
are both blocked. `first` belongs to thread 0 and `second` to thread 1, forming:

```text
T0 waits for T1 holding lock second
T1 waits for T0 holding lock first
Modeled wait-for cycle detected.
```

No blocked acquisition is recorded as executed. Both workers park through the
scheduler's condition variable before receiving a permit. Readiness is checked
under the scheduler monitor, and an enabled acquisition only writes logical
ownership. There is no blocking native application mutex acquisition inside a
permit. A separate test verifies a third runnable worker can finish three yields
while these two requests remain blocked.

The incorrect program also has passing schedule `0,0,0,0,1,1,1,1`. In the
corrected program both workers acquire `first` then `second`; its only complete
schedules are `0,0,0,0,1,1,1,1` and `1,1,1,1,0,0,0,0`. The other thread waits
on `first` while the owner proceeds, and becomes runnable when it is released.

```sh
./build/threadscope run mutex-deadlock --schedule 0,1 --trace artifacts/scenarios/deadlock.json
./build/threadscope replay artifacts/scenarios/deadlock.json
./build/threadscope run mutex-deadlock --schedule 0,0,0,0 --trace artifacts/scenarios/locks-pass.json
./build/threadscope run mutex-ordered --schedule 1 --trace artifacts/scenarios/ordered.json
./build/threadscope replay artifacts/scenarios/ordered.json
```

Global deadlock detection requires no enabled operation and at least one
unfinished worker. A graph cycle alone does not stop a run while another worker
can progress. Conversely, a terminated worker retaining a needed lock causes
deadlock without a cycle. Recursive acquisition on the non-recursive model
produces a self-cycle. Those edge cases are regression tested.

## Replay evidence and limitations

The scenario suite compares five repeated searches of each registered program
against the original full results and byte-identical JSON, then replays every
result three times in each repetition. The concrete two-lock failure is also
replayed 20 times with full-result and JSON equality. CLI integration replays
the deadlock and corrected lock-order files five times each, checks that changed
dependencies stop before the next grant, rejects invalid event categories, and
checks legacy-v1 compatibility and step-bounded replay.

Replay compares logical IDs, runnable/state tables, pending operation metadata,
recorded pre-operation dependencies/snapshots, actual observations/after-state,
and the final outcome/dependencies/ownership. These checks provide reproducibility
evidence for the tested instrumented model; they do not prove all OS executions.
Timeout progress is environmental, untracked memory cannot be certified, and
native waits or infinite local work can prevent cooperative joining. The model
does not explore C++ hardware weak-memory reorderings, arbitrary binaries, or
fairness. See [architecture.md](architecture.md) and [validation.md](validation.md).
