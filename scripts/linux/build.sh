#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
ENGINE_DIR="${REPO_ROOT}/engine"
BUILD_DIR="${ENGINE_DIR}/build"
DASHBOARD_DIR="${REPO_ROOT}/dashboard"
VENV="${REPO_ROOT}/.venv"
PYTHON="${VENV}/bin/python"

command -v node >/dev/null 2>&1 || { echo "Node.js 20.19+ o 22.12+ è obbligatorio per la dashboard." >&2; exit 1; }
command -v npm >/dev/null 2>&1 || { echo "npm non trovato." >&2; exit 1; }
node -e 'const [a,b]=process.versions.node.split(".").map(Number); if (!((a===20&&b>=19)||(a>=22&&!(a===22&&b<12)))) process.exit(1)' || {
    echo "Versione Node.js non supportata: usare 20.19+ o 22.12+." >&2
    exit 1
}
command -v python3 >/dev/null 2>&1 || { echo "Python 3 è obbligatorio per backend e test." >&2; exit 1; }
if [[ ! -x "${PYTHON}" ]]; then
    python3 -m venv "${VENV}" || { echo "Creazione .venv non riuscita: installa python3-venv." >&2; exit 1; }
fi
if ! "${PYTHON}" -c 'import fastapi, uvicorn, pydantic' >/dev/null 2>&1; then
    "${PYTHON}" -m pip install -r "${REPO_ROOT}/requirements.txt"
fi
"${PYTHON}" -c 'import fastapi, uvicorn, pydantic'

echo "=== BUILD DASHBOARD ==="
npm ci --prefix "${DASHBOARD_DIR}"
npm run build --prefix "${DASHBOARD_DIR}"
test -f "${DASHBOARD_DIR}/dist/index.html" || { echo "Dashboard dist/index.html non generato." >&2; exit 1; }

command -v cmake >/dev/null 2>&1 || { echo "CMake è obbligatorio." >&2; exit 1; }
command -v ctest >/dev/null 2>&1 || { echo "CTest è obbligatorio." >&2; exit 1; }
if command -v ninja >/dev/null 2>&1; then
    cmake -S "${ENGINE_DIR}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
else
    cmake -S "${ENGINE_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
fi

echo "=== BUILD NATIVA RELEASE ==="
cmake --build "${BUILD_DIR}" --parallel 2

echo "=== TEST BACKEND ==="
(
    cd "${REPO_ROOT}"
    "${PYTHON}" -m unittest backend.tests.test_main
)

echo "=== TEST NATIVI ==="
ctest --test-dir "${BUILD_DIR}" --output-on-failure

test -x "${BUILD_DIR}/unique-rx" || { echo "Eseguibile unique-rx non trovato." >&2; exit 1; }
echo "[OK] Dashboard, backend, motore e test completati. Il mining non è stato avviato."
