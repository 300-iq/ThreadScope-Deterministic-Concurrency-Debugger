# Complete repository audit

This audit covers the current scheduler, DFS explorer, trace codec/replay, four
scenarios, CLI, React debugger, local API, tests, warning configuration, and
measurement pipeline. Verification was performed locally on macOS arm64 with
AppleClang 17. The changes remain local; no GitHub Actions result is claimed.

## Reproduced defects and corrections

The first four regressions below were run against the existing implementation
before fixing it and failed as described. The final strict Debug/Release and
standalone UBSan suites passed with the regressions enabled.

| Issue | Reproduction and consequence | Correction and coverage |
| --- | --- | --- |
| Native-worker identity was not enforced | A task passed its live `Context` to an extra native thread. That thread's `yield()` was accepted and recorded as the owner's logical ID. A schedule could hide which native thread really executed the operation. | Record native worker IDs under the monitor. `perform()` rejects a foreign caller before posting a request or taking a permit. `context_rejects_foreign_native_thread` requires rejection, zero events, and clean termination. |
| Exploration could accept a reduced runnable set | A two-yield factory returned a real result but removed an enabled alternative from its recorded runnable array. DFS returned one path and falsely reported exhaustion. This affects custom factory metadata; the built-in scheduler already emits complete sets. | Require each runnable array to equal the IDs whose recorded states are runnable, with valid thread tables and quiescence. `factories_cannot_hide_runnable_choices` reproduces the rejected factory. |
| Trace terminal status could contradict events | Changing a failed trace to `completed`, or marking a complete trace as `deadlock`/`assertion_failed`, was accepted by the C++ codec and UI parser. UI import could display an impossible outcome. Strict actual replay already compared terminal outcomes; this defect did not establish a passing replay of a changed outcome. | Both validators enforce completed/assertion/deadlock/step-limit semantics, stable final states, and assertion-value consistency. C++ `trace_outcomes_cannot_contradict_events`, six frontend parser cases, and real API rejection cover these inputs. A separate valid-source/faulty-factory test retains runtime final-outcome divergence coverage. |
| Invalid saves could destroy an existing valid trace | An in-memory event with an invalid step was encoded and saved over a valid file. Loading then failed; the original evidence was lost. | Validate the encoded trace before opening/truncating the destination. `invalid_trace_save_preserves_file` requires the previous contents to survive the rejected save. This does not make filesystem writes transactional. |

Additional input/output hardening and coverage:

- C++ and TypeScript JSON readers reject duplicate object keys, including escaped
  spellings, and more than 32 nested containers. C++ duplicate/deep-input tests
  and three dedicated frontend tests pass. Previously duplicate keys were accepted
  by the standard parsers; no parser stack-crash reproduction is claimed.
- Assertion observations must contain equal before/after values, an expected
  value, and a boolean consistent with the comparison. Non-assert operations
  cannot carry assertion results. Changed ordinary observations remain eligible
  for execution-time replay divergence checks.
- Request-validation errors are HTTP 400; corrupt/missing engine trace output is
  HTTP 502; unexpected server failures are HTTP 500 with an explicit lack of a
  verified result. Frontend HTML/malformed-JSON replies now produce an actionable
  error. Mocked fetch cases verify HTTP errors and ambiguous replies; real API
  cases reject contradictory traces and duplicate options before execution.
- Subprocess lifecycle unit tests require SIGKILL on watchdog, abort, or excessive
  output, and require child `close` before rejecting a killed job. Spawn failure
  cannot become a successful result. These tests model lifecycle signals; they do
  not claim a real 30-second hang or mid-execution socket-disconnect test.
- Strict project warnings now apply to examples, CLI, all tests, and the benchmark
  as well as the core. Vendored JSON headers are system includes. Warnings-as-errors
  is enabled by default and can be explicitly disabled by the build option.
- The CLI scheduling-prefix size check now includes the final token. The benchmark
  joins already-created native workers if later worker construction fails.

## Scheduler, synchronization, and shutdown reasoning

`Scheduler::Impl::monitor` protects the request slots, logical states, readiness
metadata, permits, failure flags, and event history. Task registration and resource
registration precede worker creation; both tables are fixed during execution.
Registration and `run()` are single-owner APIs, not concurrent management APIs.

The driver chooses only after all slots are non-running. Unselected workers are
parked on the condition variable; terminated workers published that state under
the monitor. A selected worker remains logically running after its controlled
action until it posts its next request or actually returns. The next choice
therefore cannot depend on which worker happens to arrive first. Tests vary
startup and between-operation delays and require identical histories. Deadlock
means unfinished modeled requests with no enabled operation. A wait cycle alone
does not stop another runnable worker; a terminated lock owner can create a
deadlock without a cycle.

Built-in operation actions and snapshots execute under the monitor. Relaxed atomic
loads/stores/fetch-adds are race-free; monitor handoffs order their observations.
They do not model weak-memory reorderings. Modeled ownership is atomic and is
changed only by controlled nonblocking actions. Logical joins query monitor-protected
termination states while the monitor is held; they do not call native joins inside
the permit. Native worker joins occur during cleanup after releasing the monitor.

Exceptions from task code are handled outside the monitor, including user-defined
`what()`. Per-worker allocation-free error flags preserve failure if copying a
diagnostic throws. The driver selects startup errors in stable ID order at the
barrier. Existing isolated exception-monitor and allocation-injection regressions
pass. Cleanup marks stopping, wakes framework waiters, and joins every launched
worker on normal or exceptional exits. Snapshots describe the stop decision before
cancellation changes live worker states.

A deadline is checked after the quiescence wait even when its predicate became
true while the driver was descheduled. Deadlines are environmental, not deterministic
replay inputs. Native thread creation failures, scheduling delays and allocation
failures remain outside the stable-program guarantee.

The ownership fix checks live-context use. It cannot make an escaped/dangling
context or resource reference safe. Tasks, exception handlers and task destructors
must finish bounded local work; clients must not suppress cancellation by catching
every exception and continuing forever. Resources must outlive the run and be
owned by one execution. No exhaustive proof of all possible library callers is
claimed, and TSan runtime validation remains unavailable here.

## DFS completeness, bounds, and state isolation

DFS stores a spine referencing prior immutable decisions. At each leaf it advances
the deepest remaining alternative in ascending runnable-ID order, pops exhausted
frames, and supplies the resulting prefix to a fresh run. The default suffix must
always select the lowest runnable ID; otherwise lower alternatives could be
silently skipped. Existing guards reject changed forced prefixes, a non-minimum
default suffix, excess executed steps, or invalid decisions. This audit additionally
checks that runnable choices agree with the full state table.

`bounded_dfs_matches_independent_state_model` uses a separately implemented,
sequential state interpreter for three workers: a flag-dependent variable-length
thread, a flag setter, and a two-yield thread. It recursively enumerates its valid
choice tree and compares exact path order and uniqueness against the real engine
for step bounds 1 through 6 and schedule caps around the number of leaves. It also
compares exhaustion and schedule-bound reporting. This supplements the exact six
lost-update paths, three-way choices, early termination, dead ends and empty
program tests; it is evidence for these models, not a general completeness proof.

Each built-in factory constructs fresh scheduler, resources and tasks. Repeated
searches require equal initial snapshots and full encoded histories. Custom
factories are responsible for all hidden local/global state reset. Prefix checks
detect observed drift but cannot certify hidden deterministic state or undo a
custom factory's side effects. Environmental errors stop exploration and do not
produce an exhausted frontier.

Reaching the scheduling cap on the final leaf reports exhaustion; encountering a
cap with a remaining alternative reports a schedule bound. A step-bound leaf is
an unfinished execution. Exhausting all such bounded leaves still gives
`step_bound_reached=true` and incomplete exploration. Full histories are retained:
there is no partial-order reduction or global library memory/time-budget guarantee.

## Replay and UI/API consistency

Replay structurally validates the entire trace before invoking the factory. It
checks choices, runnable sets, thread states, operation/resource/label, available
before snapshots and wait dependencies before granting a permit. Observations and
after snapshots are compared afterward. The final status, message, thread states,
resources and dependencies must match as well. There is no fallback choice.
Post-operation divergence cannot roll back the operation.

Schema-v1 legacy traces may omit all snapshots/dependencies; replay checks only
the available fields. That intentionally provides less evidence than a current
full trace. Duplicate-key/depth/outcome validation is now aligned across the codec
and shared UI parser. The parser is structural validation, not proof of execution;
only an actual C++ replay can produce the verified badge. Timeout/worker-error
traces are diagnostic and cannot be marked replay-verified.

The local API checks engine summary counts, trace program IDs, bounds, outcomes
and exit codes. It parses actual saved files instead of deriving hypothetical
results. Broken/Corrected searches hold one job guard with identical settings but
independent scenario executions. The broken trace is never relabeled for the
corrected implementation. Branches validate the original by replay, reproduce the
common prefix/checkpoint before permits, generate a separate result, and replay it
independently. Missing checkpoints in a shorter history are shown as absent.

Requests cannot provide an executable or shell command. The binary and working
directory are fixed; spawn uses an argument array and `shell:false`. The server
binds to localhost, checks Host/Origin, requires JSON for POSTs, limits bodies to
8 MiB, rejects unknown options, and sets HTTP timeouts. Jobs use unique temporary
directories, capped output, a 30-second child watchdog, cancellation, and cleanup.
Frontend exploration is capped at 100 schedules / 256 steps / 5000 ms; replay has
its own 1000-step cap. Trace files cap at 8 MiB and job trace output at 16 MiB.
This boundary assumes a trusted local checkout and compiled built-in programs.

Browser verification against the rebuilt UI/API repeated lost-update exploration,
failure-to-pass What-If with final counter 1 versus 2, verified original/modified
replays, counter Patch & Verify (6/4 assertions versus 2/0), and broken replay.
Screenshots in `docs/images` are from those executions. Earlier milestone browser
checks cover locks and mobile layouts; this audit does not claim a new automated
browser E2E suite or a fresh full mobile pass.

## Verification performed

| Configuration | Observed result |
| --- | --- |
| Debug, all strict warnings + Werror | 8/8 CTest suites passed |
| Release, all strict warnings + Werror | 8/8 CTest suites passed |
| Standalone nonrecovering UBSan | 8/8 CTest suites passed |
| React/TypeScript/Vite | Production build passed; 69/69 tests passed |
| ASan + UBSan | Compiled; independent smoke and engine-help probes each timed out after 10 seconds, empty output; full suite not run |
| Separate TSan | Compiled; both probes returned -11 (SIGSEGV), empty output; full suite not run |
| Reproducible Release benchmark | 160 schedules; 2 seeded fault classes; 300/300 replay trials verified |

ASan/TSan probes were also attempted outside sandbox restrictions. The independent
smoke program contains only an atomic, a native thread and a join; it does not link
the ThreadScope core. Its flushed `main reached` marker was not observed. This
suggests a local runtime/toolchain startup problem, but its root cause has not been
established. No clean ASan/TSan scan or scheduler sanitizer finding is claimed.
Separate Linux CI sanitizer jobs are configured but were not executed locally.
Raw probe/suite outputs are preserved in [ASan](verification/asan.json),
[TSan](verification/tsan.json), and [UBSan](verification/ubsan.json) reports.
Repository paths are normalized to `<repository>` for readability; exit codes,
timeouts and captured diagnostics are preserved.

## Exact verification commands

From the repository root:

```sh
cmake -S . -B build-audit-strict -DCMAKE_BUILD_TYPE=Debug -DTHREADSCOPE_WARNINGS_AS_ERRORS=ON
cmake --build build-audit-strict --parallel 4
ctest --test-dir build-audit-strict --output-on-failure

cmake -S . -B build-audit-release -DCMAKE_BUILD_TYPE=Release -DTHREADSCOPE_WARNINGS_AS_ERRORS=ON
cmake --build build-audit-release --parallel 4
ctest --test-dir build-audit-release --output-on-failure

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTHREADSCOPE_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cd web
npm run build
npm test
cd ..

cmake -S . -B build-audit-ubsan -DCMAKE_BUILD_TYPE=Debug -DTHREADSCOPE_SANITIZER=undefined
cmake --build build-audit-ubsan --parallel 4
python3 scripts/verify_sanitizer.py --build-dir build-audit-ubsan --report artifacts/audit/ubsan.json

cmake -S . -B build-audit-asan -DCMAKE_BUILD_TYPE=Debug -DTHREADSCOPE_SANITIZER=address
cmake --build build-audit-asan --parallel 4
python3 scripts/verify_sanitizer.py --build-dir build-audit-asan --report artifacts/audit/asan.json --probe-timeout 10

cmake -S . -B build-audit-tsan -DCMAKE_BUILD_TYPE=Debug -DTHREADSCOPE_SANITIZER=thread
cmake --build build-audit-tsan --parallel 4
python3 scripts/verify_sanitizer.py --build-dir build-audit-tsan --report artifacts/audit/tsan.json --probe-timeout 10

python3 scripts/benchmark.py --build-dir build-audit-release --output artifacts/benchmark.json --trials 10 --replay-trials 5 --baseline-samples 100
```

The sanitizer script first runs bounded startup probes, records raw diagnostics,
then runs CTest only if both probes succeed. Failed probes return nonzero. The
benchmark smoke regression runs real full and capped searches and requires counts
and replay markers, never timing thresholds. Full methodology and raw measurements
are in [benchmark.md](benchmark.md) and [benchmarks/macos-arm64.json](benchmarks/macos-arm64.json).

## Remaining issues and unverified behavior

1. **ASan/TSan scans unavailable:** resolve runtime startup or run on a supported
   host before claiming address/race-sanitizer coverage. Linux/macOS GitHub CI and
   Windows/MSVC have not been executed in this audit.
2. **Uncooperative C++ tasks:** an in-process scheduler cannot safely kill native
   workers. Infinite local work, native blocking, a blocking destructor or a
   swallowed cancellation can prevent joining. Current API fixed scenarios have
   bounded tasks and an external process watchdog; arbitrary custom programs are
   outside that service boundary.
3. **Client contracts and trace identity:** custom factories must isolate and reset
   state; contexts/resources must remain alive and exclusive to a run. Program IDs
   are versioned names, not executable fingerprints. Current replay detects recorded
   divergence but cannot certify unobserved code or stripped legacy metadata.
4. **Persistence and resource cost:** trace writes validate before overwriting but
   are not atomic against disk-full, interruption or concurrent writers. DFS retains
   full histories and can consume substantial memory. CLI/library bounds do not
   provide a global process memory or exploration-time limit.
5. **Lifecycle and browser coverage:** deterministic unit tests cover child watchdog,
   kill/close and abort; real API tests cover pre-abort/job cleanup. Forced real
   watchdog overrun, an actual mid-execution client disconnect, and an automated
   browser regression suite remain unverified. Browser screenshots are manual
   automation evidence, not exhaustive frontend accessibility/security coverage.
6. **Measurements:** one local machine, tiny finite scenario trees, small sample
   counts, no controlled background load, and thread creation in both overhead
   modes. No performance improvement, broad speed claim or universal correctness
   result follows from these numbers. Repository work has not been committed/pushed
   by this audit; configured CI therefore is not evidence of a remote pass.

## Three strongest demonstrated accomplishments

1. Real native workers execute through a centrally controlled cooperative scheduler
   with arrival-independent decisions and strict, reproducible operation-level replay.
2. Bounded DFS finds two seeded logical fault classes without undefined data races;
   an independent variable-length state model checks enumeration and cap behavior.
3. The debugger connects actual exploration, What-If executions, and Broken/Corrected
   comparisons to inspectable JSON evidence and explicit C++ replay verification.
