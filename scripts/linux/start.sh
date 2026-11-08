#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
CONFIG="${ROOT}/config.json"
LOGS="${ROOT}/logs"
API="http://127.0.0.1:8000"
if [[ ! -f "${CONFIG}" ]]; then echo "config.json assente: copia config.example.json e configura wallet/pool." >&2; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "Python 3 non trovato." >&2; exit 1; }
VENV="${ROOT}/.venv"
PYTHON="${VENV}/bin/python"
if [[ ! -x "${PYTHON}" ]]; then
    python3 -m venv "${VENV}" || { echo "Creazione .venv non riuscita: installa python3-venv." >&2; exit 1; }
fi
if ! "${PYTHON}" -c 'import fastapi, uvicorn, pydantic' >/dev/null 2>&1; then
    "${PYTHON}" -m pip install -r "${ROOT}/requirements.txt" || { echo "Installazione dipendenze backend non riuscita." >&2; exit 1; }
fi
"${PYTHON}" -c 'import fastapi, uvicorn, pydantic' || { echo "Dipendenze backend non disponibili nella .venv." >&2; exit 1; }
mkdir -p "${LOGS}"
health() { "${PYTHON}" -c 'import urllib.request; r=urllib.request.urlopen("http://127.0.0.1:8000/api/health", timeout=1); raise SystemExit(0 if b"ok" in r.read() else 1)' >/dev/null 2>&1; }
if ! health; then
  nohup "${PYTHON}" -m uvicorn backend.src.main:app --host 127.0.0.1 --port 8000 >"${LOGS}/backend.stdout.log" 2>"${LOGS}/backend.stderr.log" &
  BACKEND_PID=$!
  printf '%s\n' "${BACKEND_PID}" > "${LOGS}/backend.pid"
  READY=0
  for _ in $(seq 1 50); do
    if ! kill -0 "${BACKEND_PID}" 2>/dev/null; then echo "Backend terminato. Controlla logs/backend.stderr.log." >&2; exit 1; fi
    if health; then READY=1; break; fi
    sleep 0.2
  done
  if [[ "${READY}" -ne 1 ]]; then echo "Backend non raggiungibile su ${API}." >&2; exit 1; fi
fi
"${PYTHON}" -c 'import urllib.request; r=urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:8000/api/miner/start", method="POST"), timeout=10); print(r.read().decode())'
