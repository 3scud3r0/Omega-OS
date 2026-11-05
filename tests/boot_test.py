"""Compatibility entry point for the V2 hardware acceptance suite."""
import runpy
runpy.run_path("tests/v2_validation.py", run_name="__main__")
