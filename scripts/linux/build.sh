#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
ENGINE_DIR="${REPO_ROOT}/engine"
BUILD_DIR="${ENGINE_DIR}/build"

if command -v npm >/dev/null 2>&1; then
    printf '%s\n' "Building local dashboard..."
    npm ci --prefix "${REPO_ROOT}/dashboard"
    npm run build --prefix "${REPO_ROOT}/dashboard"
else
    printf '%s\n' "Node.js/npm not found: dashboard build skipped (Node.js 20.19+ or 22.12+ required)." >&2
fi

command -v cmake >/dev/null 2>&1 || {
    printf '%s\n' "CMake is required." >&2
    exit 1
}
command -v ctest >/dev/null 2>&1 || {
    printf '%s\n' "CTest is required." >&2
    exit 1
}

if command -v ninja >/dev/null 2>&1; then
    cmake -S "${ENGINE_DIR}" -B "${BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
else
    cmake -S "${ENGINE_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
fi

cmake --build "${BUILD_DIR}" --parallel 2
ctest --test-dir "${BUILD_DIR}" --output-on-failure
