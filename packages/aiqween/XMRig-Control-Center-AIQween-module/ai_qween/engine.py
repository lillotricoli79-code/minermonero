"""
AI Qween - controller-side advisor for official XMRig.
This module does NOT mine, fabricate metrics, or change a running XMRig process.
It evaluates real XMRig API samples and returns conservative recommendations.
Python 3.10+, standard library only.
"""
from __future__ import annotations

from dataclasses import dataclass, asdict
from collections import deque
from datetime import datetime, timezone
from typing import Optional, Any
import math
import time


@dataclass
class TelemetrySample:
    timestamp: float
    hashrate_10s: Optional[float]
    hashrate_60s: Optional[float]
    hashrate_15m: Optional[float]
    accepted: Optional[int]
    rejected: Optional[int]
    uptime_seconds: Optional[int]
    cpu_temperature_c: Optional[float] = None
    gpu_temperature_c: Optional[float] = None
    cpu_load_percent: Optional[float] = None
    source: str = "xmrig-api"


@dataclass
class QweenDecision:
    mode: str
    action: str
    suggested_threads: Optional[int]
    confidence: float
    reason: str
    safety_state: str
    created_utc: str

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass
class QweenConfig:
    min_threads: int = 1
    max_threads: int = 16
    max_cpu_temperature_c: float = 82.0
    resume_temperature_c: float = 75.0
    max_cpu_load_percent: float = 95.0
    minimum_samples: int = 4
    minimum_gain_percent: float = 2.0
    temperature_required_for_auto: bool = True
    auto_apply: bool = False  # intentionally unsupported by this advisor
    sample_window: int = 12


class AIQween:
    """
    Safe heuristic advisor.

    - Uses actual XMRig summary values only.
    - Missing sensor values stay None; no invented defaults.
    - Never applies changes to a running process.
    - Requires several samples before suggesting tuning.
    - Temperature guard can recommend reducing threads, but only if a real
      temperature reading is supplied by a trusted hardware provider.
    """

    def __init__(self, config: Optional[QweenConfig] = None):
        self.config = config or QweenConfig()
        if self.config.sample_window < 1:
            raise ValueError("sample_window deve essere almeno 1")
        if self.config.minimum_samples < 1:
            raise ValueError("minimum_samples deve essere almeno 1")
        if self.config.min_threads < 1 or self.config.max_threads < self.config.min_threads:
            raise ValueError("Intervallo thread non valido")
        if self.config.resume_temperature_c >= self.config.max_cpu_temperature_c:
            raise ValueError("resume_temperature_c deve essere inferiore a max_cpu_temperature_c")
        if not 0 <= self.config.max_cpu_load_percent <= 100:
            raise ValueError("max_cpu_load_percent deve essere tra 0 e 100")
        self.samples: deque[TelemetrySample] = deque(maxlen=self.config.sample_window)
        self._thermal_latched = False
        self._last_hot_temperature: Optional[float] = None
        self._last_decision: Optional[QweenDecision] = None

    @staticmethod
    def _number(value: Any) -> Optional[float]:
        if isinstance(value, bool) or value is None:
            return None
        try:
            v = float(value)
            return v if math.isfinite(v) and v >= 0 else None
        except (TypeError, ValueError):
            return None

    @classmethod
    def from_xmrig_summary(cls, summary: dict[str, Any],
                           cpu_temperature_c: Optional[float] = None,
                           gpu_temperature_c: Optional[float] = None,
                           cpu_load_percent: Optional[float] = None) -> TelemetrySample:
        hr = summary.get("hashrate") or {}
        total = hr.get("total") or []
        results = summary.get("results") or {}
        connection = summary.get("connection") or {}

        def arr(i: int) -> Optional[float]:
            try:
                return cls._number(total[i])
            except (IndexError, TypeError):
                return None

        good = cls._number(results.get("shares_good"))
        total_shares = cls._number(results.get("shares_total"))
        rejected = None
        if good is not None and total_shares is not None and total_shares >= good:
            rejected = int(total_shares - good)

        return TelemetrySample(
            timestamp=time.time(),
            hashrate_10s=arr(0),
            hashrate_60s=arr(1),
            hashrate_15m=arr(2),
            accepted=int(good) if good is not None else None,
            rejected=rejected,
            uptime_seconds=int(cls._number(connection.get("uptime")))
                if cls._number(connection.get("uptime")) is not None else None,
            cpu_temperature_c=cls._number(cpu_temperature_c),
            gpu_temperature_c=cls._number(gpu_temperature_c),
            cpu_load_percent=cls._number(cpu_load_percent),
        )

    def ingest(self, sample: TelemetrySample) -> None:
        self.samples.append(sample)

    def decide(self, current_threads: int, logical_cpus: int) -> QweenDecision:
        now = datetime.now(timezone.utc).isoformat()
        cfg = self.config
        current_threads = max(1, int(current_threads))
        logical_cpus = max(1, int(logical_cpus))
        max_threads = max(cfg.min_threads, min(cfg.max_threads, logical_cpus))

        if not self.samples:
            return self._decision("WAIT", None, 0.0,
                                  "In attesa delle statistiche reali di XMRig.",
                                  "UNKNOWN", now)

        latest = self.samples[-1]
        temp = latest.cpu_temperature_c
        load = latest.cpu_load_percent

        # Thermal guard: no inferred/default temperature.
        if temp is not None:
            if temp >= cfg.max_cpu_temperature_c:
                self._thermal_latched = True
                self._last_hot_temperature = temp
            elif temp <= cfg.resume_temperature_c:
                self._thermal_latched = False
                self._last_hot_temperature = None

        if self._thermal_latched:
            target = max(cfg.min_threads, current_threads - 1)
            if temp is None:
                reason = (
                    "Protezione termica ancora attiva: il sensore CPU non è "
                    "disponibile dopo il superamento della soglia. Verificare "
                    "manualmente il raffreddamento prima di riprendere."
                )
            else:
                reason = (
                    f"Temperatura CPU reale {temp:.1f} °C: ridurre di un "
                    "thread e verificare il raffreddamento."
                )
            return self._decision("PROTECT", target, 0.95, reason, "HOT", now)

        if load is not None and load >= cfg.max_cpu_load_percent:
            target = max(cfg.min_threads, current_threads - 1)
            return self._decision("PROTECT", target, 0.85,
                                  f"Carico CPU reale {load:.1f}% oltre il limite configurato.",
                                  "HIGH_LOAD", now)

        if cfg.temperature_required_for_auto and temp is None:
            return self._decision("OBSERVE", None, 0.0,
                                  "Sensore CPU non disponibile: nessuna ottimizzazione automatica senza temperatura verificata.",
                                  "SENSOR_MISSING", now)

        if len(self.samples) < cfg.minimum_samples:
            return self._decision("LEARN", None, 0.0,
                                  f"Campioni insufficienti ({len(self.samples)}/{cfg.minimum_samples}); raccolgo dati prima di consigliare modifiche.",
                                  "NORMAL", now)

        stable_rates = [
            s.hashrate_60s for s in self.samples
            if s.hashrate_60s is not None and s.hashrate_60s > 0
        ]
        if len(stable_rates) < cfg.minimum_samples:
            return self._decision("LEARN", None, 0.0,
                                  "Hashrate 60 secondi non ancora disponibile in modo sufficiente.",
                                  "NORMAL", now)

        # A conservative recommendation only: the advisor never assumes that
        # more threads necessarily means more hashrate.
        avg = sum(stable_rates) / len(stable_rates)
        spread = (max(stable_rates) - min(stable_rates)) / avg if avg else 1.0
        if spread > 0.25:
            return self._decision("OBSERVE", None, 0.25,
                                  "Hashrate instabile: non modificare i thread; controllare pool, sistema e temperatura.",
                                  "UNSTABLE", now)

        if current_threads < max_threads and (load is None or load < 85.0):
            target = min(max_threads, current_threads + 1)
            return self._decision("TEST_ONE_STEP", target, 0.55,
                                  f"Prova controllata suggerita: {target} thread. Confrontare hashrate 60s e temperatura dopo la modifica manuale.",
                                  "NORMAL", now)

        if current_threads > cfg.min_threads and load is not None and load > 80.0:
            target = max(cfg.min_threads, current_threads - 1)
            return self._decision("TEST_ONE_STEP", target, 0.55,
                                  f"Carico elevato ({load:.1f}%): prova a {target} thread e confronta i risultati.",
                                  "NORMAL", now)

        return self._decision("HOLD", current_threads, 0.4,
                              "Configurazione attuale da mantenere: non emerge un miglioramento dimostrato.",
                              "NORMAL", now)

    def _decision(self, mode: str, threads: Optional[int], confidence: float,
                  reason: str, safety: str, now: str) -> QweenDecision:
        d = QweenDecision(
            mode=mode,
            action="MANUAL_REVIEW" if threads is not None else "NO_CHANGE",
            suggested_threads=threads,
            confidence=confidence,
            reason=reason,
            safety_state=safety,
            created_utc=now,
        )
        self._last_decision = d
        return d

    def export_state(self) -> dict[str, Any]:
        return {
            "schema": 1,
            "thermal_latched": self._thermal_latched,
            "samples": [asdict(s) for s in self.samples],
            "last_decision": self._last_decision.to_dict() if self._last_decision else None,
        }
