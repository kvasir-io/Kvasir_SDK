#!/usr/bin/env python3
"""Fixture generator of test_chip_package.cmake: writes a fixed text to --out; fails when GEN_SVD_FAIL is set."""
import argparse
import os
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--out", required=True)
args = parser.parse_args()
if os.environ.get("GEN_SVD_FAIL"):
    print("asked to fail", file=sys.stderr)
    sys.exit(3)
with open(args.out, "w") as f:
    f.write("<device>generated</device>\n")
