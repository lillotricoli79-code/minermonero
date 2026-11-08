#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY'
import urllib.request
url = "http://127.0.0.1:8000/api/miner/stop"
request = urllib.request.Request(url, data=b"{}", method="POST",
                                headers={"Content-Type": "application/json"})
with urllib.request.urlopen(request, timeout=15) as response:
    print("Arresto miner richiesto:", response.read().decode())
PY
