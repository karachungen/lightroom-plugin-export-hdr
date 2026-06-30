#!/usr/bin/env bash
set -euo pipefail

echo "==> CMake version: $(cmake --version | head -1)"
echo "==> Configuring libjpeg-turbo 3.0.1 (expect failure on CMake 4 without policy workaround)"

set +e
cmake -S /work/turbo -B /work/turbo/build
status=$?
set -e

if [[ $status -eq 0 ]]; then
	echo "UNEXPECTED: configure succeeded (this repro expects CMake 4 policy failure)"
	exit 1
fi

echo "OK: reproduced expected CMake 4 configure failure for libjpeg-turbo 3.0.1"
exit 0
