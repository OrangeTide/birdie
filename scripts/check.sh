#!/bin/sh
# Run the same checks CI runs, locally during development.
#
# Mirrors .github/workflows/ci.yml: builds the app, runs the headless GUI
# test, and builds the dist bundle. Stops at the first failure.
#
# Usage:
#   scripts/check.sh            # incremental (fast)
#   scripts/check.sh --clean    # from scratch, like a fresh CI checkout
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)

set -e

cd "$(dirname "$0")/.."

if [ "${1:-}" = "--clean" ]; then
    echo "==> make clean"
    make clean
elif [ -n "${1:-}" ]; then
    echo "unknown argument: $1" >&2
    echo "usage: $0 [--clean]" >&2
    exit 2
fi

echo "==> make (build)"
make

echo "==> make test (headless GUI test)"
make test

echo "==> make dist (bundle)"
make dist

echo "==> all checks passed"
