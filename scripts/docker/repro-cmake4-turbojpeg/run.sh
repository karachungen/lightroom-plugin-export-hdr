#!/usr/bin/env bash
# Reproduce the Windows CI turbojpeg CMake 4 policy failure on macOS/Linux via Docker.
# This validates the regression locally; it does not build uhdr_repack or MSVC binaries.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> Building repro image"
docker build -t uhdr-repro-cmake4-turbojpeg "$SCRIPT_DIR"

echo "==> Running repro (should exit 0 when failure is reproduced)"
docker run --rm uhdr-repro-cmake4-turbojpeg

echo "Done. The failure is the missing CMAKE_POLICY_VERSION_MINIMUM=3.5 that build_plugin.sh exports before configure."
