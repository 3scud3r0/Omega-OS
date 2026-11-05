"""Compatibility entry point for the V3 hardware acceptance suite."""
import runpy
runpy.run_path("tests/v3_validation.py", run_name="__main__")
