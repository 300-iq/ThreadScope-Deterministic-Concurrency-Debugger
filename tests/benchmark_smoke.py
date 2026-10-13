"""Verify the real measurement pipeline; assert counts, never timing thresholds."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='threadscope-benchmark-test-') as temporary:
    for steps, cap in [(8, 100), (1, 1)]:
        output = Path(temporary) / ('report-' + str(steps) + '.json')
        result = subprocess.run([sys.executable, str(root / 'scripts/benchmark.py'),
            '--build-dir', sys.argv[1], '--output', str(output), '--trials', '1',
            '--replay-trials', '2', '--baseline-samples', '4',
            '--max-steps', str(steps), '--max-schedules', str(cap)],
            capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        report = json.loads(output.read_text())
        assert report['verification_passed'] is True
        assert len(report['scenarios']) == 4
        assert all(row['explore_ns']['median'] >= 0 for row in report['scenarios'])
        assert all(row['reproducible_trace_sets'] for row in report['scenarios'])
        if steps == 8:
            assert report['total_schedules'] == 16
            assert len(report['distinct_seeded_faults']) == 2
            assert report['replay']['verified'] == report['replay']['attempted'] == 12
            assert all(row['incomplete_trials'] == 0 for row in report['scenarios'])
            assert report['overhead']['native_ns']['samples'] == 4
        else:
            assert report['total_schedules'] == 4
            assert report['distinct_seeded_faults'] == []
            assert report['replay']['attempted'] == 0
            assert all(row['incomplete_trials'] == 1 for row in report['scenarios'])
            assert report['overhead']['runtime_ratio'] is None
print('Real benchmark counts, bounded searches, replay markers and reports verified.')
