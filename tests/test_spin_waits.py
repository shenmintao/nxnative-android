"""Run the upstream lock-poll rejection cases in the Android regression suite."""
import importlib.util
from pathlib import Path

path = Path(__file__).resolve().parents[1] / 'nxnative/dm/tools/tests/test_spin_waits.py'
spec = importlib.util.spec_from_file_location('upstream_spin_wait_tests', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
SpinWaits = module.SpinWaits
