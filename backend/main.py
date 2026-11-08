from __future__ import annotations

import ipaddress
import json
import os
import re
import secrets
import shutil
import subprocess
import threading
import time
import urllib.error
import urllib.request
from collections import deque
from pathlib import Path
from typing import Any, Literal
from urllib.parse import urlsplit

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field, field_validator, model_validator


ROOT = Path(__file__).resolve().parent.parent
CONFIG_PATH = ROOT / "config.json"
DIST_DIR = ROOT / "dist"
DEFAULTS: dict[str, Any] = {
    "autosave": True,
    "background": False,
    "colors": True,
    "cpu": {"enabled": True, "huge-pages": True, "priority": 2},
    "cuda": False,
    "opencl": False,
    "http": {
        "enabled": True,
        "host": "127.0.0.1",
        "port": 18088,
        "access-token": "",
        "restricted": True,
    },
    "pools": [],
}


class MinerConfigUpdate(BaseModel):
    pool: str = ""
    wallet: str = ""
    worker: str = "rig01"
    password: str | None = None
    threads: int = Field(default=0, ge=0, le=256)
    tls: bool = False
    tls_fingerprint: str = ""
    msr_mode: Literal["auto", "off", "fast", "custom"] = "auto"
    api_port: int = Field(default=18088, ge=1024, le=65535)

    @field_validator("pool")
    @classmethod
    def valid_pool(cls, value: str) -> str:
        value = value.strip()
        if not value:
            return value
        if any(character in value for character in ("/", "?", "#", "@", " ")):
            raise ValueError("Inserisci host:porta senza schema o credenziali")
        try:
            parsed = urlsplit(f"//{value}")
            if not parsed.hostname or not parsed.port:
                raise ValueError
        except ValueError as exc:
            raise ValueError("Pool non valido: usa host:porta") from exc
        return value

    @field_validator("wallet", "worker")
    @classmethod
    def trim_text(cls, value: str) -> str:
        value = value.strip()
        if "\x00" in value:
            raise ValueError("Valore non valido")
        return value

    @field_validator("tls_fingerprint")
    @classmethod
    def valid_tls_fingerprint(cls, value: str) -> str:
        value = value.strip().replace(":", "")
        if not value:
            return ""
        if not re.fullmatch(r"[0-9a-fA-F]{64}", value):
            raise ValueError("Il fingerprint TLS deve contenere 64 caratteri esadecimali SHA-256")
        return value.lower()

    @model_validator(mode="after")
    def fingerprint_requires_tls(self):
        if self.tls_fingerprint and not self.tls:
            raise ValueError("Abilita TLS quando configuri un fingerprint del certificato")
        return self


def load_config() -> dict[str, Any]:
    config = json.loads(json.dumps(DEFAULTS))
    if CONFIG_PATH.exists():
        try:
            saved = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            if isinstance(saved, dict):
                config.update(saved)
                config["cpu"] = {**DEFAULTS["cpu"], **(saved.get("cpu") or {})}
                config["http"] = {**DEFAULTS["http"], **(saved.get("http") or {})}
        except (OSError, json.JSONDecodeError):
            pass
    if not config["http"].get("access-token"):
        config["http"]["access-token"] = secrets.token_urlsafe(32)
        save_config(config)
    return config


def save_config(config: dict[str, Any]) -> None:
    CONFIG_PATH.write_text(json.dumps(config, indent=2), encoding="utf-8")
    if os.name != "nt":
        CONFIG_PATH.chmod(0o600)


def safe_number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
        return number if number >= 0 and number < float("inf") else None
    except (TypeError, ValueError):
        return None


def normalize_summary(summary: dict[str, Any]) -> dict[str, Any]:
    hashrate = summary.get("hashrate") or {}
    total = hashrate.get("total") or []
    results = summary.get("results") or {}
    connection = summary.get("connection") or {}

    def rate(index: int) -> float | None:
        try:
            return safe_number(total[index])
        except (IndexError, TypeError):
            return None

    accepted = safe_number(results.get("shares_good"))
    total_shares = safe_number(results.get("shares_total"))
    rejected = (
        max(0, int(total_shares - accepted))
        if accepted is not None and total_shares is not None
        else None
    )
    uptime = safe_number(connection.get("uptime"))
    return {
        "hashrate": {"10s": rate(0), "60s": rate(1), "15m": rate(2)},
        "shares": {
            "accepted": int(accepted) if accepted is not None else None,
            "rejected": rejected,
            "total": int(total_shares) if total_shares is not None else None,
        },
        "uptime_seconds": int(uptime) if uptime is not None else None,
        "pool": connection.get("pool"),
        "connected": connection.get("connected"),
    }


def is_loopback(host: str) -> bool:
    host = host.strip("[]").lower()
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def is_local_api_connection(peer_host: str, request_host: str) -> bool:
    return is_loopback(peer_host) and is_loopback(request_host)


def msr_override_enabled(randomx: dict[str, Any]) -> bool:
    """Return True unless MSR is explicitly disabled.

    The native engine does not implement MSR yet, so automatic, fast and
    custom modes must be routed through XMRig instead of silently mining
    without the requested CPU register preset.
    """
    value = randomx.get("wrmsr")
    if value is False:
        return False
    if isinstance(value, int) and not isinstance(value, bool) and value < 0:
        return False
    return True


class MinerController:
    def __init__(self) -> None:
        self.process: subprocess.Popen[str] | None = None
        self.engine_kind: str | None = None
        self.native_status: dict[str, Any] | None = None
        self.lock = threading.RLock()
        self.log_lock = threading.Lock()
        self.logs: deque[dict[str, Any]] = deque(maxlen=400)
        self.log_sequence = 0

    def _log(self, message: str) -> None:
        with self.log_lock:
            self.log_sequence += 1
            self.logs.append({"id": self.log_sequence, "time": time.time(), "message": message})

    def is_running(self) -> bool:
        with self.lock:
            return self.process is not None and self.process.poll() is None

    def read_summary(self, config: dict[str, Any]) -> dict[str, Any]:
        request = urllib.request.Request(
            f"http://127.0.0.1:{int(config['http']['port'])}/1/summary",
            headers={"Authorization": f"Bearer {config['http']['access-token']}"},
        )
        with urllib.request.urlopen(request, timeout=1.5) as response:
            payload = json.loads(response.read().decode("utf-8"))
        return normalize_summary(payload)

    def status(self) -> dict[str, Any]:
        managed = self.is_running()
        if managed and self.engine_kind == "unique-rx":
            with self.lock:
                if self.native_status is not None:
                    return {**self.native_status, "managed": True}
            return {
                "state": "AVVIO",
                "managed": True,
                "error": None,
                "hashrate": {"10s": None, "60s": None, "15m": None},
                "shares": {"accepted": 0, "rejected": 0, "total": 0},
                "uptime_seconds": None,
                "pool": None,
                "connected": False,
            }
        try:
            summary = self.read_summary(load_config())
            return {"state": "ATTIVO", "managed": managed, "error": None, **summary}
        except (OSError, ValueError, urllib.error.URLError, TimeoutError) as exc:
            return {
                "state": "AVVIO" if managed else "OFFLINE",
                "managed": managed,
                "error": str(exc) if managed else None,
                "hashrate": {"10s": None, "60s": None, "15m": None},
                "shares": {"accepted": None, "rejected": None, "total": None},
                "uptime_seconds": None,
                "pool": None,
                "connected": None,
            }

    def start(self) -> None:
        config = load_config()
        pools = config.get("pools") or []
        pool = pools[0] if pools and isinstance(pools[0], dict) else {}
        if not str(pool.get("user", "")).strip():
            raise HTTPException(status_code=400, detail="Inserisci il wallet prima di avviare")
        if not str(pool.get("url", "")).strip():
            raise HTTPException(status_code=400, detail="Configura il pool prima di avviare")
        msr_requires_xmrig = msr_override_enabled(config.get("randomx") or {})
        native_candidates = [
            shutil.which("unique-rx"),
            str(ROOT / "engine" / "build" / "unique-rx"),
        ]
        native_path = next(
            (path for path in native_candidates if path and Path(path).is_file()),
            None,
        )
        if native_path and not msr_requires_xmrig:
            executable_path = native_path
            command = [native_path, "--mine", str(CONFIG_PATH)]
            engine_kind = "unique-rx"
        else:
            executable_path = shutil.which("xmrig") or shutil.which("xmrig.exe")
            if not executable_path:
                for candidate in (ROOT / "xmrig", ROOT / "xmrig.exe"):
                    if candidate.is_file():
                        executable_path = str(candidate)
                        break
            if not executable_path:
                detail = "Compila engine/unique-rx o installa XMRig"
                if msr_requires_xmrig:
                    detail = "MSR automatico, Fast o personalizzato richiede XMRig finché il motore nativo non implementa MSR"
                raise HTTPException(status_code=400, detail=detail)
            command = [str(executable_path), "-c", str(CONFIG_PATH)]
            engine_kind = "xmrig"
        executable = Path(executable_path).resolve()
        if self.is_running():
            raise HTTPException(status_code=409, detail="XMRig è già gestito da questa dashboard")
        try:
            self.read_summary(config)
            raise HTTPException(status_code=409, detail="Un miner risponde già all'API locale")
        except HTTPException:
            raise
        except (OSError, ValueError, urllib.error.URLError, TimeoutError):
            pass

        try:
            with self.lock:
                self.process = subprocess.Popen(
                    command,
                    cwd=executable.parent,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    errors="replace",
                    bufsize=1,
                )
                self.engine_kind = engine_kind
                self.native_status = None
                process = self.process
            if engine_kind == "xmrig" and msr_requires_xmrig:
                mode = public_config(config)["msr_mode"]
                self._log(
                    f"Avvio XMRig: modalità MSR {mode}; il motore nativo non ha ancora "
                    "il supporto MSR completo."
                )
            else:
                self._log(f"Avvio {engine_kind} richiesto dall'utente.")
            threading.Thread(target=self._read_output, args=(process,), daemon=True).start()
        except OSError as exc:
            self.process = None
            raise HTTPException(status_code=500, detail=f"Impossibile avviare XMRig: {exc}") from exc

    def _read_output(self, process: subprocess.Popen[str]) -> None:
        if process.stdout:
            for line in process.stdout:
                line = line.rstrip()
                if line.startswith("UNIQUE_RX_STATUS "):
                    try:
                        payload = json.loads(line[len("UNIQUE_RX_STATUS "):])
                    except json.JSONDecodeError:
                        self._log("Riga telemetria UNIQUE RX non valida.")
                    else:
                        if isinstance(payload, dict) and "hashrate" in payload:
                            with self.lock:
                                self.native_status = payload
                        else:
                            self._log("Schema telemetria UNIQUE RX non valido.")
                else:
                    self._log(line)
        self._log(f"{self.engine_kind or 'Miner'} terminato (codice {process.poll()}).")

    def stop(self) -> None:
        with self.lock:
            process = self.process
            if process is None or process.poll() is not None:
                self.process = None
                raise HTTPException(status_code=409, detail="Nessun miner avviato da questa dashboard")
            process.terminate()
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
        self._log("Arresto miner richiesto dall'utente.")

    def recent_logs(self, after: int = 0) -> list[dict[str, Any]]:
        with self.log_lock:
            return [entry for entry in self.logs if entry["id"] > after]


controller = MinerController()
app = FastAPI(title="UNIQUE RX Local Dashboard", version="0.1.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=[
        "http://127.0.0.1:5173",
        "http://localhost:5173",
        "http://127.0.0.1:5174",
        "http://localhost:5174",
        "http://127.0.0.1:8000",
        "http://localhost:8000",
    ],
    allow_methods=["GET", "POST", "PUT", "DELETE"],
    allow_headers=["Content-Type"],
)


@app.middleware("http")
async def local_only(request, call_next):
    if not request.url.path.startswith("/api/"):
        return await call_next(request)
    host = request.url.hostname or ""
    peer_host = request.client.host if request.client else ""
    origin = request.headers.get("origin")
    if not is_local_api_connection(peer_host, host):
        return JSONResponse({"detail": "Questa API è accessibile solo da loopback"}, status_code=403)
    allowed_origins = {
        "http://127.0.0.1:5173",
        "http://localhost:5173",
        "http://127.0.0.1:5174",
        "http://localhost:5174",
        "http://127.0.0.1:8000",
        "http://localhost:8000",
    }
    if origin and origin.rstrip("/") not in allowed_origins:
        return JSONResponse({"detail": "Origine non consentita"}, status_code=403)
    return await call_next(request)


@app.get("/api/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


def public_config(config: dict[str, Any]) -> dict[str, Any]:
    pools = config.get("pools") or []
    pool = pools[0] if pools else {}
    cpu = config.get("cpu") or {}
    randomx = config.get("randomx") or {}
    wrmsr = randomx.get("wrmsr")
    if wrmsr is None:
        msr_mode = "auto"
    elif wrmsr is False or wrmsr == -1:
        msr_mode = "off"
    elif wrmsr is True:
        msr_mode = "fast"
    else:
        msr_mode = "custom"
    thread_hint = safe_number(cpu.get("max-threads-hint"))
    threads = round((os.cpu_count() or 1) * thread_hint / 100) if thread_hint else 0
    return {
        "pool": pool.get("url", ""),
        "wallet": pool.get("user", ""),
        "worker": pool.get("rig-id", "rig01"),
        "password_set": bool(pool.get("pass", "")),
        "threads": threads,
        "tls": bool(pool.get("tls", False)),
        "tls_fingerprint": pool.get("tls-fingerprint") or "",
        "msr_mode": msr_mode,
        "api_port": config["http"]["port"],
    }


def apply_config_update(current: dict[str, Any], update: MinerConfigUpdate) -> dict[str, Any]:
    result = json.loads(json.dumps(current))
    pools = result.get("pools") or [{}]
    pool = dict(pools[0]) if isinstance(pools[0], dict) else {}
    pool.update({
        "url": update.pool,
        "user": update.wallet,
        "rig-id": update.worker,
        "keepalive": True,
        "tls": update.tls,
        "tls-fingerprint": update.tls_fingerprint or None,
        "enabled": True,
    })
    if update.password is not None:
        pool["pass"] = update.password
    cpu = dict(result.get("cpu") or {})
    cpu["enabled"] = True
    if update.threads > 0:
        cpu["max-threads-hint"] = max(
            1,
            min(100, round(100 * update.threads / max(1, os.cpu_count() or 1))),
        )
    else:
        cpu.pop("max-threads-hint", None)
    http = dict(result.get("http") or {})
    http.update({
        "enabled": True,
        "host": "127.0.0.1",
        "port": update.api_port,
        "restricted": True,
    })
    randomx = dict(result.get("randomx") or {})
    if update.msr_mode == "fast":
        randomx.update({"wrmsr": True, "rdmsr": True})
    elif update.msr_mode == "off":
        randomx.update({"wrmsr": False, "rdmsr": True})
    elif update.msr_mode == "auto":
        randomx.pop("wrmsr", None)
        randomx["rdmsr"] = True
    elif randomx.get("wrmsr") is None:
        raise ValueError("Nessun preset MSR custom da preservare")
    result.update({"cpu": cpu, "http": http, "pools": [pool], "randomx": randomx})
    return result


@app.get("/api/config")
def get_config() -> dict[str, Any]:
    return public_config(load_config())


@app.put("/api/config")
def update_config(update: MinerConfigUpdate) -> dict[str, Any]:
    if controller.is_running():
        raise HTTPException(status_code=409, detail="Ferma XMRig prima di modificare la configurazione")
    current = load_config()
    try:
        controller.read_summary(current)
    except (OSError, ValueError, urllib.error.URLError, TimeoutError):
        pass
    else:
        raise HTTPException(status_code=409, detail="Ferma il miner collegato prima di modificare config.json")
    try:
        result = apply_config_update(current, update)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    save_config(result)
    return public_config(result)


@app.get("/api/status")
def get_status() -> dict[str, Any]:
    return controller.status()


@app.get("/api/logs")
def get_logs(after: int = 0) -> dict[str, Any]:
    return {"items": controller.recent_logs(max(0, after))}


@app.post("/api/miner/start", status_code=202)
def start_miner() -> dict[str, str]:
    controller.start()
    return {"status": "start_requested"}


@app.post("/api/miner/stop")
def stop_miner() -> dict[str, str]:
    controller.stop()
    return {"status": "stop_requested"}


if DIST_DIR.is_dir():
    app.mount("/", StaticFiles(directory=DIST_DIR, html=True), name="dashboard")