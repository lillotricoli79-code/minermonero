from __future__ import annotations

import ipaddress
import json
import os
import re
import secrets
import shutil
import signal
import subprocess
import sys
import threading
import time
import tempfile
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

from .shell_log import ShellLog


ROOT = Path(__file__).resolve().parents[2]
CONFIG_PATH = ROOT / "config.json"
DIST_DIR = ROOT / "dashboard" / "dist"
DEFAULTS: dict[str, Any] = {
    "autosave": True,
    "background": False,
    "colors": True,
    "cpu": {"enabled": True, "huge-pages": True, "jit-huge-pages": False, "prefetch-mode": 1, "priority": 2},
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
    "randomx": {"full-memory": True, "wrmsr": False, "rdmsr": True},
}


class MinerConfigUpdate(BaseModel):
    pool: str = ""
    wallet: str = ""
    worker: str = "rig01"
    password: str | None = None
    threads: int = Field(default=0, ge=0, le=256)
    priority: int = Field(default=2, ge=-1, le=5, strict=True)
    huge_pages: bool = False
    jit_huge_pages: bool = False
    prefetch_mode: int = Field(default=1, ge=0, le=3)
    tls: bool = False
    tls_fingerprint: str = ""
    msr_mode: Literal["auto", "off", "fast", "custom"] = "off"
    api_port: int = Field(default=18088, ge=1024, le=65535, strict=True)

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

    @field_validator("api_port")
    @classmethod
    def api_port_not_backend_port(cls, value: int) -> int:
        if value == 8000:
            raise ValueError("La porta 8000 è riservata al backend locale")
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
        except (OSError, json.JSONDecodeError) as exc:
            # Never replace a user's damaged/unreadable configuration with
            # defaults: that can silently erase wallet and pool settings.
            raise ValueError(f"Impossibile leggere config.json senza rischiare di sovrascriverlo: {exc}") from exc
        if not isinstance(saved, dict):
            raise ValueError("config.json deve contenere un oggetto JSON")
        for section in ("cpu", "http", "randomx"):
            if section in saved and not isinstance(saved[section], dict):
                raise ValueError(f"config.json: la sezione {section} deve essere un oggetto")
        if "pools" in saved and (
            not isinstance(saved["pools"], list)
            or any(not isinstance(pool, dict) for pool in saved["pools"])
        ):
            raise ValueError("config.json: pools deve essere un elenco di oggetti")
        config.update(saved)
        config["cpu"] = {**DEFAULTS["cpu"], **(saved.get("cpu") or {})}
        config["http"] = {**DEFAULTS["http"], **(saved.get("http") or {})}
    configured_api_port = config["http"].get("port", DEFAULTS["http"]["port"])
    if (
        isinstance(configured_api_port, bool)
        or not isinstance(configured_api_port, int)
        or not 1024 <= configured_api_port <= 65535
    ):
        raise ValueError("config.json: la porta API deve essere un intero tra 1024 e 65535")
    if configured_api_port == 8000:
        raise ValueError("config.json: la porta 8000 è riservata al backend locale")
    if not config["http"].get("access-token"):
        config["http"]["access-token"] = secrets.token_urlsafe(32)
        save_config(config)
    return config


def save_config(config: dict[str, Any]) -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=CONFIG_PATH.parent,
            prefix=f".{CONFIG_PATH.name}.",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary_path = Path(temporary.name)
            temporary.write(json.dumps(config, indent=2))
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_path, CONFIG_PATH)
        if os.name != "nt":
            CONFIG_PATH.chmod(0o600)
    finally:
        if temporary_path is not None and temporary_path.exists():
            temporary_path.unlink()


def safe_number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
        return number if number >= 0 and number < float("inf") else None
    except (TypeError, ValueError):
        return None


def first_enabled_pool(config: dict[str, Any]) -> dict[str, Any]:
    pools = config.get("pools") or []
    for pool in pools:
        if isinstance(pool, dict) and pool.get("enabled", True):
            return pool
    return {}


def find_native_engine(root: Path = ROOT) -> Path | None:
    # Several historical build directories may coexist. Select the most
    # recently produced project binary instead of silently launching an
    # older engine just because its directory has higher search priority.
    candidates = [
        root / "unique-rx",
        root / "unique-rx.exe",
        root / "engine" / "build" / "unique-rx",
        root / "engine" / "build" / "unique-rx.exe",
        root / "engine" / "build-mingw" / "unique-rx.exe",
        root / "engine" / "build-mingw-ninja" / "unique-rx.exe",
        root / "engine" / "build-mingw-make" / "unique-rx.exe",
        root / "engine" / "build-msvc" / "unique-rx.exe",
        root / "engine" / "build-msvc" / "Release" / "unique-rx.exe",
    ]
    existing = [candidate for candidate in candidates if candidate.is_file()]
    if existing:
        return max(existing, key=lambda candidate: candidate.stat().st_mtime_ns)

    path_executable = shutil.which("unique-rx") or shutil.which("unique-rx.exe")
    return Path(path_executable) if path_executable else None


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
            "stale": None,
            "pending": None,
            "expired": None,
            "dropped": None,
        },
        "uptime_seconds": int(uptime) if uptime is not None else None,
        "algorithm": None,
        "job_id": None,
        "height": None,
        "threads": None,
        "cpu": None,
        "msr_active": None,
        "full_memory": None,
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


def is_intel_cpu() -> bool:
    """Detect Intel CPU vendor without assuming a model-name string."""
    if os.name == "nt":
        command = (
            "$cpu = Get-CimInstance Win32_Processor -ErrorAction Stop | "
            "Select-Object -First 1; $cpu.Manufacturer"
        )
        try:
            result = subprocess.run(
                ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
                capture_output=True,
                text=True,
                timeout=4,
                check=False,
            )
            if result.returncode == 0:
                vendor = result.stdout.strip().lower()
                if vendor:
                    return "intel" in vendor or "genuineintel" in vendor
        except (OSError, subprocess.TimeoutExpired):
            return False
        return False

    cpuinfo = Path("/proc/cpuinfo")
    try:
        for line in cpuinfo.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.lower().startswith("vendor_id"):
                return line.split(":", 1)[1].strip() == "GenuineIntel"
    except OSError:
        return False
    return False


def native_msr_available(executable_path: str | None) -> bool:
    """Check that the native MSR provider and the signed Intel module are present."""
    if sys.platform == "win32":
        program_files = Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
        libraries = [program_files / "PawnIO" / "PawnIOLib.dll"]
        try:
            import winreg

            with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE,
                r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO",
            ) as key:
                install_location, _ = winreg.QueryValueEx(key, "InstallLocation")
                if install_location:
                    libraries.append(Path(install_location) / "PawnIOLib.dll")
        except (ImportError, OSError):
            pass

        modules = [ROOT / "engine" / "msr" / "IntelMSR.bin"]
        if executable_path:
            executable_dir = Path(executable_path).resolve().parent
            libraries.append(executable_dir / "PawnIOLib.dll")
            modules.extend([
                executable_dir / "msr" / "IntelMSR.bin",
                executable_dir / "IntelMSR.bin",
            ])
        return any(path.is_file() for path in libraries) and any(
            path.is_file() for path in modules
        )

    if sys.platform.startswith("linux"):
        online_file = Path("/sys/devices/system/cpu/online")
        try:
            online = online_file.read_text(encoding="ascii").strip()
            cpu_ids: list[int] = []
            for item in online.split(","):
                if "-" in item:
                    first, last = (int(value) for value in item.split("-", 1))
                    cpu_ids.extend(range(first, last + 1))
                elif item:
                    cpu_ids.append(int(item))
        except (OSError, ValueError):
            cpu_ids = list(range(os.cpu_count() or 1))

        # Match the CPUs the current process is actually allowed to use.
        # In containers/cgroups, checking every online CPU can incorrectly
        # disable MSR support when device nodes exist only for the allowed set.
        affinity_getter = getattr(os, "sched_getaffinity", None)
        if affinity_getter is not None:
            try:
                allowed_cpu_ids = affinity_getter(0)
            except OSError:
                allowed_cpu_ids = None
            if allowed_cpu_ids is not None:
                cpu_ids = [cpu for cpu in cpu_ids if cpu in allowed_cpu_ids]

        return bool(cpu_ids) and all(
            (device := Path("/dev/cpu") / str(cpu) / "msr").is_file()
            and os.access(device, os.R_OK | os.W_OK)
            for cpu in cpu_ids
        )
    return False

def msr_override_enabled(randomx: dict[str, Any]) -> bool:
    """Return True only for custom MSR payloads not supported by the native engine.

    Automatic and fast presets are selected by the native engine for the detected
    CPU vendor/family/model. Unsupported hardware or missing MSR permissions are
    handled as an optional optimization by the native engine, not by switching
    the whole miner to a different implementation.
    """
    value = randomx.get("wrmsr")
    if value is None or isinstance(value, bool):
        return False
    if isinstance(value, int) and value < 0:
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
        self.shell_log = ShellLog(ROOT / "logs" / "shell" / "shell.log")

    def _log(self, message: str, engine: str | None = None) -> None:
        # Keep the original miner shell line intact and identify its source.
        # The engine field is additive, so existing dashboard/API clients remain compatible.
        with self.log_lock:
            self.log_sequence += 1
            source = engine or self.engine_kind or "dashboard"
            self.logs.append({
                "id": self.log_sequence,
                "time": time.time(),
                "engine": source,
                "message": message,
            })
            try:
                self.shell_log.write(f"[{source}] {message}")
            except OSError:
                # A full or unavailable disk must not stop the miner process.
                pass

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
        # Read process identity and its telemetry as one locked snapshot. The
        # output-reader thread may clear/reassign these fields during shutdown
        # or an immediate restart.
        with self.lock:
            process = self.process
            managed = process is not None and process.poll() is None
            engine_kind = self.engine_kind
            native_status = self.native_status
        if managed and engine_kind == "unique-rx":
            if native_status is not None:
                return {**native_status, "managed": True}
            return {
                "state": "AVVIO",
                "managed": True,
                "error": None,
                "hashrate": {"10s": None, "60s": None, "15m": None},
                "shares": {
                    "accepted": 0, "rejected": 0, "total": 0,
                    "stale": 0, "pending": 0, "expired": 0, "dropped": 0,
                },
                "uptime_seconds": None,
                "algorithm": None,
                "job_id": None,
                "height": None,
                "threads": None,
                "msr_active": None,
                "full_memory": None,
                "cpu": None,
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
                "shares": {
                    "accepted": None, "rejected": None, "total": None,
                    "stale": None, "pending": None, "expired": None, "dropped": None,
                },
                "uptime_seconds": None,
                "algorithm": None,
                "job_id": None,
                "height": None,
                "threads": None,
                "msr_active": None,
                "full_memory": None,
                "cpu": None,
                "pool": None,
                "connected": None,
            }

    def start(self, thread_override: int | None = None) -> None:
        config = load_config()
        pool = first_enabled_pool(config)
        if not str(pool.get("user", "")).strip():
            raise HTTPException(status_code=400, detail="Inserisci il wallet prima di avviare")
        if not str(pool.get("url", "")).strip():
            raise HTTPException(status_code=400, detail="Configura il pool prima di avviare")
        msr_requires_xmrig = msr_override_enabled(config.get("randomx") or {})
        native_executable = find_native_engine()
        native_path = str(native_executable) if native_executable else None
        if native_path and not msr_requires_xmrig:
            executable_path = native_path
            command = [native_path, "--mine", str(CONFIG_PATH)]
            if thread_override is not None:
                if isinstance(thread_override, bool) or not 1 <= thread_override <= 256:
                    raise HTTPException(status_code=400, detail="Il numero di thread deve essere tra 1 e 256")
                command.extend(["--threads", str(thread_override)])
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
                    detail = "MSR nativo non disponibile: installa PawnIO e IntelMSR.bin per Intel, oppure installa XMRig per preset personalizzati o CPU non Intel"
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
            popen_options: dict[str, Any] = {
                "cwd": executable.parent,
                "stdout": subprocess.PIPE,
                "stderr": subprocess.STDOUT,
                "text": True,
                "errors": "replace",
                "bufsize": 1,
            }
            if os.name == "nt":
                # CTRL_BREAK_EVENT lets XMRig run its shutdown path and restore MSR.
                popen_options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
            with self.lock:
                # Recheck while holding the same lock used to publish the
                # process. Two simultaneous API start requests must never
                # launch two miners and orphan the first child process.
                if self.process is not None and self.process.poll() is None:
                    raise HTTPException(
                        status_code=409,
                        detail="Un miner è già gestito da questa dashboard",
                    )
                process = subprocess.Popen(command, **popen_options)
                self.process = process
                self.engine_kind = engine_kind
                self.native_status = None
            if engine_kind == "xmrig" and msr_requires_xmrig:
                mode = public_config(config)["msr_mode"]
                self._log(
                    f"Avvio XMRig: modalità MSR {mode}; il motore nativo supporta "
                    "solo il preset automatico Intel reversibile."
                )
            else:
                self._log(f"Avvio {engine_kind} richiesto dall'utente.")
            self._start_output_reader(process, engine_kind)
        except OSError as exc:
            # Do not clear a process that a concurrent start request may
            # already have published after this Popen attempt failed.
            with self.lock:
                if self.process is not None and self.process.poll() is not None:
                    self.process = None
                    self.engine_kind = None
                    self.native_status = None
            raise HTTPException(status_code=500, detail=f"Impossibile avviare XMRig: {exc}") from exc

    def _start_output_reader(
        self, process: subprocess.Popen[str], engine_kind: str
    ) -> None:
        try:
            threading.Thread(
                target=self._read_output,
                args=(process, engine_kind),
                daemon=True,
            ).start()
        except RuntimeError as exc:
            # A child must never be left mining without its output/status reader:
            # the dashboard would lose process lifecycle and shutdown telemetry.
            self._log(f"Impossibile avviare il lettore output: {exc}; arresto miner.")
            try:
                if process.poll() is None:
                    if os.name == "nt":
                        process.send_signal(signal.CTRL_BREAK_EVENT)
                    else:
                        process.terminate()
            except (AttributeError, OSError, ValueError):
                if process.poll() is None:
                    try:
                        process.terminate()
                    except OSError:
                        pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            finally:
                with self.lock:
                    if self.process is process:
                        self.process = None
                        self.engine_kind = None
                        self.native_status = None
            raise HTTPException(
                status_code=500,
                detail="Impossibile avviare il lettore output; il miner è stato arrestato",
            ) from exc

    def _read_output(
        self, process: subprocess.Popen[str], engine_kind: str
    ) -> None:
        if process.stdout:
            for line in process.stdout:
                # Remove only the line terminator: do not trim or rewrite miner output.
                line = line.rstrip("\r\n")
                if line.startswith("UNIQUE_RX_STATUS "):
                    try:
                        payload = json.loads(line[len("UNIQUE_RX_STATUS "):])
                    except json.JSONDecodeError:
                        self._log(line, engine_kind)
                    else:
                        if isinstance(payload, dict) and "hashrate" in payload:
                            # Machine telemetry is consumed by the API/dashboard,
                            # not copied into the operator shell as noisy JSON.
                            with self.lock:
                                if self.process is process:
                                    self.native_status = payload
                        else:
                            self._log(line, engine_kind)
                else:
                    self._log(line, engine_kind)
        self._log(f"{engine_kind} terminato (codice {process.poll()}).", engine_kind)
        # Clear only this exact process: an immediately restarted miner may
        # already have replaced self.process while this reader is unwinding.
        with self.lock:
            if self.process is process:
                self.process = None
                self.engine_kind = None
                self.native_status = None

    def stop(self) -> None:
        with self.lock:
            process = self.process
            if process is None or process.poll() is not None:
                self.process = None
                raise HTTPException(status_code=409, detail="Nessun miner avviato da questa dashboard")
        try:
            if os.name == "nt":
                process.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                process.terminate()
        except (AttributeError, OSError, ValueError):
            # The child can exit between poll() and signal delivery. If it is
            # still alive, terminate() bypasses the miner's graceful MSR
            # restore path, so make that loss of guarantee explicit.
            if process.poll() is None:
                self._log(
                    "Segnale di arresto graduale non disponibile: "
                    "terminazione forzata; il ripristino MSR non è garantito."
                )
                try:
                    process.terminate()
                except OSError:
                    pass
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self._log(
                "Il miner non ha chiuso entro 30 secondi: terminazione forzata; "
                "il ripristino MSR potrebbe non essere stato eseguito."
            )
            try:
                process.kill()
            except OSError:
                pass
            process.wait(timeout=5)
        self._log("Arresto miner completato.")

    def recent_logs(self, after: int = 0) -> list[dict[str, Any]]:
        with self.log_lock:
            return [entry for entry in self.logs if entry["id"] > after]


    def execute_shell_command(self, raw_command: str) -> dict[str, Any]:
        """Execute operator commands without writing to config.json."""
        command = raw_command.strip()
        if not command:
            raise HTTPException(status_code=400, detail="Inserisci un comando. Digita help per l'elenco.")
        if len(command) > 120:
            raise HTTPException(status_code=400, detail="Comando troppo lungo.")
        parts = command.split()
        verb = parts[0].lower()
        status = self.status()

        if verb in {"help", "?"} and len(parts) == 1:
            return {"command": command, "restart": False, "lines": [
                "help                 Elenco comandi",
                "start                Avvia il miner",
                "stop                 Arresto controllato",
                "diagnose             Diagnostica CPU, memoria, MSR e stato JIT esposto",
                "auto                 Alias di threads auto",
                "status               Stato miner, pool e CPU",
                "threads              Thread attivi",
                "threads N            Riavvio controllato con N worker (config invariato)",
                "threads auto         Riavvio con selezione automatica del config",
                "workers              Dettaglio worker reali, se disponibile",
                "shares               Accepted / rejected / stale / pending",
                "graph                Hashrate 10s / 60s / 15m",
                "pool                 Pool e connessione",
                "logs [N]             Ultime N righe (default 20)",
            ]}

        if verb == "auto" and len(parts) == 1:
            return self.execute_shell_command("threads auto")

        if verb == "start" and len(parts) == 1:
            self.start()
            return {"command": command, "restart": False, "lines": ["Richiesta di avvio inviata al miner nativo/backend."]}

        if verb == "stop" and len(parts) == 1:
            self.stop()
            return {"command": command, "restart": False, "lines": ["Arresto controllato completato."]}

        if verb == "diagnose" and len(parts) == 1:
            cpu = status.get("cpu") or {}
            features = cpu.get("features") or {}
            jit = status.get("jit_status") or status.get("randomx_jit")
            jit_text = str(jit) if jit is not None else "non pubblicato dalla telemetria (non dedurre JIT/interprete)"
            return {"command": command, "restart": False, "lines": [
                "CPU: " + str(cpu.get("brand") or "non rilevata"),
                "Vendor: " + str(cpu.get("vendor") or "n/d") + " · family/model: " + str(cpu.get("family", "n/d")) + "/" + str(cpu.get("model", "n/d")),
                "Core: " + str(cpu.get("physical_cores", "n/d")) + " fisici / " + str(cpu.get("logical_threads", "n/d")) + " logici",
                "Cache L3: " + str(cpu.get("l3_cache_bytes", "n/d")) + " byte · NUMA: " + str(cpu.get("numa_nodes", "n/d")),
                "AES: " + str(features.get("aes", "n/d")) + " · AVX2: " + str(features.get("avx2", "n/d")) + " · BMI2: " + str(features.get("bmi2", "n/d")),
                "Huge pages: richieste=" + str(status.get("large_pages_requested", "n/d")) + " · attive=" + str(status.get("large_pages_active", "n/d")),
                "MSR: " + str(status.get("msr_status") or status.get("msr_active") or "n/d"),
                "JIT: " + jit_text,
                "Errore miner: " + str(status.get("error") or "nessuno"),
            ]}

        if verb == "threads":
            if len(parts) == 1:
                return {"command": command, "restart": False, "lines": [
                    "Thread attivi: " + str(status.get("threads") if status.get("threads") is not None else "non disponibili"),
                    "Uso: threads N oppure threads auto",
                ]}
            if len(parts) != 2:
                raise HTTPException(status_code=400, detail="Uso: threads N oppure threads auto")
            if not status.get("managed") or not self.is_running():
                raise HTTPException(status_code=409, detail="Avvia prima il miner dalla dashboard.")
            with self.lock:
                engine_kind = self.engine_kind
            if engine_kind != "unique-rx":
                raise HTTPException(status_code=409, detail="Regolazione thread disponibile solo con il motore nativo UNIQUE RX.")
            value = parts[1].lower()
            if value == "auto":
                override = None
                label = "automatici secondo config.json"
            else:
                try:
                    override = int(value, 10)
                except ValueError as exc:
                    raise HTTPException(status_code=400, detail="Usa un intero tra 1 e 256 oppure auto.") from exc
                if not 1 <= override <= 256:
                    raise HTTPException(status_code=400, detail="Il numero di thread deve essere tra 1 e 256.")
                label = str(override)
            self._log("Shell: cambio thread richiesto; riavvio controllato del motore.")
            self.stop()
            self.start(override)
            return {"command": command, "restart": True, "lines": [
                "Riavvio richiesto con thread " + label + ".",
                "config.json non è stato modificato.",
                "Il numero effettivo sarà confermato dalla telemetria del motore.",
            ]}

        if verb == "status" and len(parts) == 1:
            cpu = status.get("cpu") or {}
            return {"command": command, "restart": False, "lines": [
                "Miner: " + str(status.get("state", "SCONOSCIUTO")) + " · gestito: " + ("sì" if status.get("managed") else "no"),
                "Pool: " + str(status.get("pool") or "n/d") + " · " + ("connesso" if status.get("connected") else "non connesso/n.d."),
                "Hashrate: 10s " + str((status.get("hashrate") or {}).get("10s")) + " H/s · 60s " + str((status.get("hashrate") or {}).get("60s")) + " H/s · 15m " + str((status.get("hashrate") or {}).get("15m")) + " H/s",
                "Thread: " + str(status.get("threads") if status.get("threads") is not None else "n/d") + " · uptime: " + str(status.get("uptime_seconds") if status.get("uptime_seconds") is not None else "n/d") + " s",
                "CPU: " + str(cpu.get("brand") or "n/d") + " · core fisici: " + str(cpu.get("physical_cores", "n/d")) + " · logici: " + str(cpu.get("logical_threads", "n/d")),
                "Errore: " + str(status.get("error") or "nessuno"),
            ]}

        if verb == "graph" and len(parts) == 1:
            rates = status.get("hashrate") or {}
            numeric = [v for v in rates.values() if isinstance(v, (int, float)) and v >= 0]
            maximum = max(numeric, default=1) or 1
            lines = ["Hashrate reale riportato dal motore:"]
            for label, key in (("10 secondi", "10s"), ("60 secondi", "60s"), ("15 minuti", "15m")):
                value = rates.get(key)
                if isinstance(value, (int, float)) and value >= 0:
                    bar = "#" * min(40, int(value / maximum * 40))
                    lines.append(f"{label:12} {value:,.2f} H/s |{bar}")
                else:
                    lines.append(f"{label:12} non disponibile")
            return {"command": command, "restart": False, "lines": lines}

        if verb == "workers" and len(parts) == 1:
            workers = status.get("workers")
            if not isinstance(workers, list) or not workers:
                return {"command": command, "restart": False, "lines": ["Dettaglio worker non pubblicato dal motore; nessun dato viene simulato."]}
            lines = [f"Worker #{w.get('id')}: {'attivo' if w.get('active') else 'inattivo'} · {w.get('hashrate_hps', 0):,.2f} H/s · {w.get('hashes', 0)} hash" for w in workers if isinstance(w, dict)]
            return {"command": command, "restart": False, "lines": lines or ["Nessun worker disponibile."]}

        if verb == "shares" and len(parts) == 1:
            shares = status.get("shares") or {}
            return {"command": command, "restart": False, "lines": [f"{key}: {shares.get(key) if shares.get(key) is not None else 'n/d'}" for key in ("accepted", "rejected", "stale", "pending")]}

        if verb == "pool" and len(parts) == 1:
            return {"command": command, "restart": False, "lines": [
                "Pool: " + str(status.get("pool") or "n/d"),
                "Connessione: " + ("connesso" if status.get("connected") else "non connesso/n.d."),
                "Latenza: " + str(status.get("pool_latency_ms") if status.get("pool_latency_ms") is not None else "n/d") + " ms",
                "Riconnessioni: " + str(status.get("reconnects") if status.get("reconnects") is not None else "n/d"),
            ]}

        if verb == "logs" and len(parts) <= 2:
            try:
                limit = int(parts[1], 10) if len(parts) == 2 else 20
            except ValueError as exc:
                raise HTTPException(status_code=400, detail="Uso: logs [1..100]") from exc
            if not 1 <= limit <= 100:
                raise HTTPException(status_code=400, detail="Il limite logs deve essere tra 1 e 100.")
            return {"command": command, "restart": False, "lines": self.shell_log.recent_lines(limit)}

        raise HTTPException(status_code=400, detail="Comando sconosciuto o argomenti non validi. Digita help.")


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
    pool = first_enabled_pool(config)
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
    explicit_threads = cpu.get("threads", 0)
    if isinstance(explicit_threads, bool) or not isinstance(explicit_threads, int):
        explicit_threads = 0
    thread_hint = safe_number(cpu.get("max-threads-hint"))
    threads = (
        explicit_threads
        if explicit_threads > 0
        else round((os.cpu_count() or 1) * thread_hint / 100) if thread_hint else 0
    )
    priority = cpu.get("priority", 2)
    if isinstance(priority, bool) or not isinstance(priority, int) or not -1 <= priority <= 5:
        priority = 2
    huge_pages = cpu.get("huge-pages", True)
    if not isinstance(huge_pages, bool):
        huge_pages = False
    jit_huge_pages = cpu.get("jit-huge-pages", False)
    if not isinstance(jit_huge_pages, bool):
        jit_huge_pages = False
    prefetch_mode = cpu.get("prefetch-mode", 1)
    if type(prefetch_mode) is not int or not 0 <= prefetch_mode <= 3:
        prefetch_mode = 1
    return {
        "pool": pool.get("url", ""),
        "wallet": pool.get("user", ""),
        "worker": pool.get("rig-id", "rig01"),
        "password_set": bool(pool.get("pass", "")),
        "threads": threads,
        "priority": priority,
        "huge_pages": huge_pages,
        "jit_huge_pages": jit_huge_pages,
        "prefetch_mode": prefetch_mode,
        "tls": bool(pool.get("tls", False)),
        "tls_fingerprint": pool.get("tls-fingerprint") or "",
        "msr_mode": msr_mode,
        "api_port": config["http"]["port"],
    }


def apply_config_update(current: dict[str, Any], update: MinerConfigUpdate) -> dict[str, Any]:
    result = json.loads(json.dumps(current))
    pools = [
        dict(pool) for pool in (result.get("pools") or [])
        if isinstance(pool, dict)
    ]
    if not pools:
        pools = [{}]
    primary_index = next(
        (index for index, pool in enumerate(pools) if pool.get("enabled", True)),
        0,
    )
    pool = dict(pools[primary_index])
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
    cpu["priority"] = update.priority
    if update.threads > 0:
        # The native engine clamps this exact request to CPUs available to the process
        # and to container quotas; do not approximate it as a percentage.
        cpu["threads"] = update.threads
        cpu.pop("max-threads-hint", None)
    else:
        cpu["threads"] = 0
        cpu.pop("max-threads-hint", None)
    cpu["huge-pages"] = update.huge_pages
    cpu["jit-huge-pages"] = update.jit_huge_pages
    cpu["prefetch-mode"] = update.prefetch_mode
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
    pools[primary_index] = pool
    result.update({"cpu": cpu, "http": http, "pools": pools, "randomx": randomx})
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


@app.get("/api/logs/history")
def get_log_history(limit: int = 200) -> dict[str, Any]:
    try:
        return {"lines": controller.shell_log.recent_lines(limit)}
    except OSError as exc:
        raise HTTPException(status_code=500, detail="Impossibile leggere lo storico shell") from exc


class ShellCommand(BaseModel):
    command: str = Field(min_length=1, max_length=120)


@app.post("/api/shell/command")
def shell_command(request: ShellCommand) -> dict[str, Any]:
    return controller.execute_shell_command(request.command)


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