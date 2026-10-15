#!/usr/bin/env python3
"""Reproduce actual DFS, seeded-fault discovery, CLI replay and native baseline measurements."""
import argparse
import datetime
import hashlib
import json
import math
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = ('lost-update', 'lost-update-rmw', 'mutex-deadlock', 'mutex-ordered')
SEEDS = {'lost-update': ('split-atomic-increment', 'assertion_failed'),
         'mutex-deadlock': ('opposite-lock-order', 'deadlock')}

def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError('must be positive')
    return number

def execute(arguments, timeout=30):
    start = datetime.datetime.now(datetime.timezone.utc)
    try:
        result = subprocess.run([str(a) for a in arguments], cwd=ROOT, capture_output=True,
                                text=True, timeout=timeout)
        return {'returncode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr,
                'timeout': False, 'started_at': start.isoformat()}
    except subprocess.TimeoutExpired as error:
        def decode(value):
            return value.decode('utf-8', 'replace') if isinstance(value, bytes) else value or ''
        return {'returncode': None, 'stdout': decode(error.stdout), 'stderr': decode(error.stderr),
                'timeout': True, 'started_at': start.isoformat()}

def distribution(values):
    if not values:
        return None
    ordered = sorted(values)
    return {'samples': len(values), 'median': statistics.median(values),
            'min': min(values), 'max': max(values),
            'p95_nearest_rank': ordered[math.ceil(.95 * len(values))-1]}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def cache_value(cache, key):
    for line in cache.splitlines():
        if line.startswith(key + ':'):
            return line.split('=', 1)[1]
    return None

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-release')
    parser.add_argument('--output', type=Path, default=ROOT / 'artifacts/benchmark.json')
    parser.add_argument('--trials', type=positive, default=10)
    parser.add_argument('--replay-trials', type=positive, default=5)
    parser.add_argument('--baseline-samples', type=positive, default=100)
    parser.add_argument('--max-schedules', type=positive, default=100)
    parser.add_argument('--max-steps', type=positive, default=8)
    parser.add_argument('--timeout-ms', type=positive, default=5000)
    args = parser.parse_args()
    if args.trials > 100 or args.replay_trials > 100 or args.baseline_samples > 10000 or args.max_schedules > 10000 or args.max_steps > 1000000 or args.timeout_ms > 60000:
        parser.error('options exceed documented engine/harness bounds')
    build = args.build_dir.resolve()
    executable = build / 'threadscope_benchmark'
    cli = build / 'threadscope'
    if not executable.is_file() or not cli.is_file():
        parser.error('build threadscope and threadscope_benchmark first')
    cache = (build / 'CMakeCache.txt').read_text()
    compiler = cache_value(cache, 'CMAKE_CXX_COMPILER')
    source_paths = [ROOT / 'CMakeLists.txt', ROOT / 'third_party/nlohmann/json.hpp']
    for directory in ['include', 'src', 'examples', 'tools']:
        source_paths.extend(path for path in (ROOT / directory).rglob('*') if path.is_file())
    source_hashes = {str(path.relative_to(ROOT)): digest(path) for path in sorted(source_paths)}
    git_status = execute(['git', 'status', '--porcelain'])
    report = {
        'schema_version': 1,
        'created_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'environment': {'system': platform.system(), 'release': platform.release(), 'machine': platform.machine(),
                        'python': platform.python_version(), 'compiler': execute([compiler, '--version'])['stdout'].splitlines()[0],
                        'build_type': cache_value(cache, 'CMAKE_BUILD_TYPE'),
                        'sanitizer': cache_value(cache, 'THREADSCOPE_SANITIZER'),
                        'warnings_as_errors': cache_value(cache, 'THREADSCOPE_WARNINGS_AS_ERRORS')},
        'identity': {'git_head': execute(['git', 'rev-parse', 'HEAD'])['stdout'].strip(),
                     'working_tree_dirty': bool(git_status['stdout']), 'source_sha256': source_hashes,
                     'benchmark_binary_sha256': digest(executable), 'cli_binary_sha256': digest(cli)},
        'methodology': {'exploration_clock': 'in-process C++ steady_clock nanoseconds',
                        'exploration_includes': 'DFS, fresh resources, native worker creation, controlled operations, snapshots, joining and in-memory histories',
                        'exploration_excludes': 'JSON encoding, file writes, Python and process startup',
                        'seed_definition': 'one deliberately injected logical fault per broken program; multiple failing schedules are not distinct bugs',
                        'replay': 'independent CLI invocation for each failed trace and repeated trial, exact marker AND expected exit code required',
                        'overhead_baseline': 'one native corrected atomic counter with two increment workers and a checker joining both; same three-worker/final-value behavior',
                        'overhead_difference': 'controlled run adds scheduling, snapshots, logical join and event recording; baseline has no exploration or tracing',
                        'overhead_order': 'alternating native/controlled order after five warmups per mode; times are single executions, not whole searches'},
        'settings': {key: getattr(args, key) for key in ['trials', 'replay_trials', 'baseline_samples', 'max_schedules', 'max_steps', 'timeout_ms']},
    }
    success = False
    try:
        success = True
        with tempfile.TemporaryDirectory(prefix='threadscope-benchmark-') as temporary:
            traces_root = Path(temporary)
            execution = execute([executable, '--trials', args.trials, '--max-schedules', args.max_schedules,
                                 '--max-steps', args.max_steps, '--timeout-ms', args.timeout_ms,
                                 '--baseline-samples', args.baseline_samples, '--out', traces_root], timeout=60)
            if execution['returncode'] != 0:
                report.update(verification_passed=False, execution_error=execution)
                success = False
            else:
                raw = json.loads(execution['stdout'])
                report['raw_measurements'] = raw
                discoveries, replay_results, rows = set(), [], []
                for name in SCENARIOS:
                    records = [r for r in raw['records'] if r['scenario'] == name]
                    if len(records) != args.trials:
                        raise ValueError('benchmark did not produce every requested trial')
                    hashes, faults = [], set()
                    for record in records:
                        trial_hashes, assertions, deadlocks = [], 0, 0
                        if record['schedules'] != len(record['traces']):
                            raise ValueError('trace count contradicts measurement')
                        for relative in record['traces']:
                            path = traces_root / relative
                            data = json.loads(path.read_text())
                            if data['program'] != name + '/v1' or data['limits'] != {'max_steps': args.max_steps, 'timeout_ms': args.timeout_ms}:
                                raise ValueError('recorded program or limits contradict measurement')
                            trial_hashes.append(digest(path))
                            status = data['outcome']['status']
                            assertions += int(status == 'assertion_failed')
                            deadlocks += int(status == 'deadlock')
                            if name in SEEDS and status == SEEDS[name][1]:
                                faults.add(SEEDS[name][0])
                            if status in ('assertion_failed', 'deadlock'):
                                for repeat in range(args.replay_trials):
                                    replay = execute([cli, 'replay', path], timeout=10)
                                    verified = replay['returncode'] == 1 and 'Replay verified: every event and final outcome match.' in replay['stdout'].splitlines()
                                    replay_results.append({'scenario': name, 'exploration_trial': record['trial'],
                                                           'schedule': relative, 'replay_trial': repeat,
                                                           'verified': verified, **replay})
                                    success = success and verified
                        if assertions != record['assertions'] or deadlocks != record['deadlocks']:
                            raise ValueError('failure counts disagree with actual trace outcomes')
                        hashes.append(trial_hashes)
                        success = success and record['errors'] == 0
                    reproducible = all(h == hashes[0] for h in hashes)
                    success = success and reproducible
                    discoveries.update(faults)
                    rows.append({'scenario': name,
                                 'schedules_per_trial': [r['schedules'] for r in records],
                                 'total_schedules': sum(r['schedules'] for r in records),
                                 'explore_ns': distribution([r['explore_ns'] for r in records]),
                                 'assertion_failures_per_trial': [r['assertions'] for r in records],
                                 'deadlocks_per_trial': [r['deadlocks'] for r in records],
                                 'distinct_seeded_faults': sorted(faults), 'reproducible_trace_sets': reproducible,
                                 'incomplete_trials': sum(not r['frontier_exhausted'] or r['step_bound_reached'] or r['errors'] != 0 for r in records)})
                native = distribution(raw['overhead']['native_ns'])
                controlled = distribution(raw['overhead']['controlled_ns'])
                ratio = controlled['median'] / native['median'] if native and native['median'] > 0 else None
                report.update(scenarios=rows, distinct_seeded_faults=sorted(discoveries),
                              total_schedules=sum(r['total_schedules'] for r in rows),
                              replay={'attempted': len(replay_results), 'verified': sum(r['verified'] for r in replay_results), 'trials': replay_results},
                              overhead={'native_ns': native, 'controlled_ns': controlled, 'runtime_ratio': ratio,
                                        'percent_overhead': (ratio - 1) * 100 if ratio is not None else None}, verification_passed=success)
    except (OSError, ValueError, KeyError, TypeError) as error:
        report.update(verification_passed=False, measurement_error=str(error))
        success = False
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    if success:
        print('Schedules:', report['total_schedules'], '| distinct seeded faults:', len(report['distinct_seeded_faults']),
              '| replays:', str(report['replay']['verified']) + '/' + str(report['replay']['attempted']))
        for row in report['scenarios']:
            print(row['scenario'], 'median explore ms:', round(row['explore_ns']['median'] / 1000000, 3))
        print('Controlled/native single-run median ratio:', report['overhead']['runtime_ratio'])
    else:
        print('Benchmark verification failed; inspect recorded errors, not a success claim.')
    print('Report:', args.output)
    return 0 if success else 1

if __name__ == '__main__':
    sys.exit(main())
