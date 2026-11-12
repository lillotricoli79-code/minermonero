#!/usr/bin/env bash
set -u
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
echo "UNIQUE RX - Diagnostica locale"
echo "Repository: ${ROOT}"
if [[ -f "${ROOT}/config.json" ]]; then
  python3 - "${ROOT}/config.json" <<'PY'
import json, sys
try:
    config = json.load(open(sys.argv[1], encoding="utf-8"))
    pools = [p for p in config.get("pools", []) if p.get("enabled", True)]
    print("[OK] config.json leggibile" if pools and pools[0].get("url") and pools[0].get("user") else "[ATTENZIONE] configura pool abilitato e wallet")
except Exception as error:
    print("[ERRORE] config.json:", error)
PY
else echo "[ATTENZIONE] config.json assente: copia config.example.json e configura wallet/pool."; fi
PYTHON="${ROOT}/.venv/bin/python"
if [[ ! -x "${PYTHON}" ]] && command -v python3 >/dev/null 2>&1; then PYTHON="$(command -v python3)"; fi
if [[ -x "${PYTHON}" ]]; then
  "${PYTHON}" --version
  if "${PYTHON}" -c 'import fastapi, uvicorn, pydantic'; then
    echo "[OK] FastAPI, Uvicorn e Pydantic disponibili"
  else
    echo "[ATTENZIONE] dipendenze mancanti: scripts/linux/start.sh le installa nella .venv"
  fi
else echo "[ATTENZIONE] Python 3 non trovato."; fi
if command -v cmake >/dev/null 2>&1; then cmake --version | head -n 1; else echo "[ATTENZIONE] CMake non trovato."; fi
MINER=""
for candidate in "${ROOT}/unique-rx" "${ROOT}/engine/build/unique-rx"; do
  # Linux diagnostics must never report a Windows .exe as a usable native miner.
  if [[ -f "${candidate}" && -x "${candidate}" ]]; then MINER="${candidate}"; break; fi
done
if [[ -n "${MINER}" ]]; then echo "[OK] Motore nativo: ${MINER}"; else echo "[ATTENZIONE] eseguibile assente. Compila con scripts/linux/build.sh."; fi
python3 - <<'PY'
import json, urllib.request
try:
    with urllib.request.urlopen("http://127.0.0.1:8000/api/status", timeout=2) as response:
        print("[OK] Backend:", json.dumps(json.loads(response.read()), ensure_ascii=False))
except Exception:
    print("Backend locale non avviato.")
PY
