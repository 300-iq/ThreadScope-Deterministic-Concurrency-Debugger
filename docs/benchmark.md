# Benchmarks

Run from a fresh Release build, with sanitizers disabled:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DTHREADSCOPE_WARNINGS_AS_ERRORS=ON
cmake --build build-release --parallel 4
python3 scripts/benchmark.py --build-dir build-release --output artifacts/benchmark.json --trials 10 --replay-trials 5 --baseline-samples 100 --max-schedules 100 --max-steps 8 --timeout-ms 5000
```

`tools/benchmark.cpp` executes the actual scenario factories and DFS engine.
`scripts/benchmark.py` collects the measurements, checks saved outcomes against
counts, compares trace hashes across runs, and starts independent CLI replay
processes. It exits nonzero if executions fail, counts disagree, traces change
between trials, or any replay lacks the exact verification marker and exit code.
Bounded, unfinished runs remain explicitly reported as incomplete. A successful
report can describe a bounded search; it is not a correctness proof.

The CTest `benchmark_smoke` uses one real trial for full bounds and another with
one step/one schedule. It requires 16 full executions, two seeded fault classes,
12 verified replay attempts, and accurate incomplete reporting for all four capped
programs. It deliberately has no timing threshold. Python and the default-on
benchmark harness are required for registration.

## Definitions

- **Schedule:** one complete, failing, deadlocked, or step-bounded execution from
  fresh state. The count is executions, not DFS nodes or thread-switches.
- **Exploration time:** nanoseconds from C++ `steady_clock` around `explore()`.
  Includes DFS, resource/task creation, native worker startup/joining, requests,
  operations, snapshots and in-memory histories. Excludes JSON encoding, filesystem
  writes, Python, subprocess startup and later replay. One warmup search per scenario
  is excluded; ten actual searches per scenario are measured in fixed scenario order.
- **Distinct seeded failures:** two deliberately injected fault classes, assigned
  to the known broken implementations: split-atomic increment and opposite lock
  order. Four failing increment schedules are one seeded fault, and two deadlock
  schedules are another. This is discovery of known seeds, not general bug deduplication.
- **Replay success:** every failing trace from each measured search is replayed
  five times in separate CLI processes. Expected exit 1 and the exact standalone
  verification line are both required. Replaying the bug is success for replay but
  still a failure for the tested program. Replay trials are counted separately
  from explored schedules.
- **Reproducibility:** SHA-256 lists of saved trace bytes, including path order,
  must match between trials. Wall-clock time is not embedded in these stable traces.
- **Overhead baseline:** one corrected atomic counter execution with two native
  increment threads and one native checker thread that joins them and observes 2.
  The controlled run uses the compiled corrected scenario with the same three-worker
  and final-value behavior. It additionally uses permits, logical joins, snapshots
  and event recording. Both include resource/thread creation and joining. This
  baseline measures a single execution, not DFS, tracing IO or an arbitrary
  application. Five warmups per mode precede 100 samples each; order alternates.
- **Overhead:** median controlled duration divided by median native duration;
  percent overhead is `(ratio - 1) * 100`. No improvement claim is made.

Reports include raw durations, min/max/median and nearest-rank p95, actual schedule
and failure counts for every trial, replay output/exit codes, environment, settings,
source SHA-256s, executable SHA-256s, commit ID and dirty-worktree flag. Timed search
results are written outside the timer into a temporary directory; those traces
are replayed and hashed before cleanup. Replay paths in the report identify those
temporary measurements and are not persistent trace downloads. Regenerate traces
with the CLI for later interactive inspection.

## Executed local result

[Raw JSON report](benchmarks/macos-arm64.json), timestamp
2026-10-13T01:49:45.278693Z. Darwin 25.6.0 arm64, AppleClang 17.0.0,
Python 3.9.6, CMake Release, no sanitizer, warnings-as-errors enabled.
The source tree was uncommitted; source/binary hashes identify this measurement
more precisely than its Git HEAD.

| Program | Schedules per trial | Assertions | Deadlocks | Median / min / max search (ms) |
| --- | ---: | ---: | ---: | --- |
| lost-update | 6 | 4 | 0 | 0.733688 / 0.692500 / 1.069292 |
| lost-update-rmw | 2 | 0 | 0 | 0.173854 / 0.159791 / 0.251250 |
| mutex-deadlock | 6 | 0 | 2 | 0.597646 / 0.587167 / 0.633583 |
| mutex-ordered | 2 | 0 | 0 | 0.223354 / 0.208500 / 0.257084 |

Counts are per trial, with ten trials per program: 160 explored schedules,
two distinct seeds found, 300/300 independent failing-trace replay attempts verified,
and no incomplete trials at these bounds. All per-program trace lists matched.

| Single corrected-counter execution | Samples | Median | Min | Max | Nearest-rank p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Native baseline | 100 | 40.375 µs | 34.834 µs | 53.042 µs | 43.917 µs |
| Controlled + history | 100 | 78.7495 µs | 68.750 µs | 118.291 µs | 95.084 µs |

Median ratio: 1.950452; overhead: 95.0452%. The different instrumentation is the
intended measured cost; operation/checker implementations are not instruction-for-
instruction identical. Tiny trees and OS thread-startup costs dominate. One machine,
fixed scenario order, ten search samples and uncontrolled background load limit
generalization. No confidence interval, cross-machine comparison, regression trend,
speedup, or production-workload extrapolation is claimed.

To check measurement identity against a current checkout, hash the named files and
binaries and compare `identity.source_sha256`, `benchmark_binary_sha256`, and
`cli_binary_sha256`. The script assumes the specified build corresponds to the
working tree; it does not automatically rebuild stale artifacts. Always run the
build commands first. A sanitizer or Debug report is valid as its own measurement
but is not comparable to this Release result without accounting for configuration.
