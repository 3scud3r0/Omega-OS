"""Compatibility entry point for the V1 hardware acceptance suite."""
import runpy
runpy.run_path("tests/v1_validation.py", run_name="__main__")
