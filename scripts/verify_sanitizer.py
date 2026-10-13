#!/usr/bin/env python3
"""Bounded runtime probes first; never call a startup failure a passing scan."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

def invoke(arguments, timeout):
    try:
        result = subprocess.run(arguments, capture_output=True, text=True, timeout=timeout)
        return {"command": arguments, "returncode": result.returncode, "timeout": False,
                "stdout": result.stdout, "stderr": result.stderr}
    except subprocess.TimeoutExpired as error:
        def decode(value):
            return value.decode("utf-8", "replace") if isinstance(value, bytes) else value or ""
        return {"command": arguments, "returncode": None, "timeout": True,
                "stdout": decode(error.stdout), "stderr": decode(error.stderr)}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--probe-timeout", type=int, default=10)
    args = parser.parse_args()
    if args.probe_timeout < 1 or args.probe_timeout > 60:
        parser.error("probe timeout must be 1..60 seconds")
    directory = args.build_dir.resolve()
    probes = [invoke([str(directory / "sanitizer_smoke")], args.probe_timeout),
              invoke([str(directory / "threadscope"), "--help"], args.probe_timeout)]
    supported = all(p["returncode"] == 0 and not p["timeout"] for p in probes)
    suite = invoke(["ctest", "--test-dir", str(directory), "--output-on-failure"], 180) if supported else None
    passed = supported and suite["returncode"] == 0 and not suite["timeout"]
    report = {"build_dir": str(directory), "probes": probes, "suite": suite,
              "status": "passed" if passed else "suite_failed" if supported else "runtime_probe_failed"}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(report["status"], "report:", args.report)
    if not supported:
        print("Full suite was not run: inspect both independent-runtime and engine probe diagnostics.")
    elif not passed:
        print(suite["stdout"], suite["stderr"])
    return 0 if passed else 1

if __name__ == "__main__":
    sys.exit(main())
