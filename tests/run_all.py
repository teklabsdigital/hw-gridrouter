"""Run every test file (each prints its own summary).  python3 tests/run_all.py

GRIDROUTER_TEST_SCALE=0.2 makes the random suites five times smaller.
test_kicad_emit.py is skipped unless run with KiCad's Python.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = ["test_api.py", "test_search.py", "test_ripup.py", "test_kicad_emit.py"]

failed = []
for f in FILES:
    print("=" * 70, "\n", f, flush=True)
    if subprocess.call([sys.executable, os.path.join(HERE, f)]) != 0:
        failed.append(f)
print("=" * 70)
print("FAILED: " + ", ".join(failed) if failed else "all test files passed")
sys.exit(1 if failed else 0)
