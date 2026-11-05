"""Minimal read-only XMRig API adapter for AI Qween."""
from __future__ import annotations
import json
import urllib.request
from typing import Optional
from .engine import AIQween, QweenConfig


class XMRigQweenBridge:
    def __init__(self, host="127.0.0.1", port=18088, token="",
                 qween: Optional[AIQween] = None, timeout=2.0):
        if host not in ("127.0.0.1", "localhost", "::1"):
            raise ValueError("Per sicurezza AI Qween accetta solo API XMRig locali.")
        self.host = host
        self.port = int(port)
        self.token = token
        self.timeout = float(timeout)
        self.qween = qween or AIQween(QweenConfig())

    def poll(self, cpu_temperature_c=None, gpu_temperature_c=None,
             cpu_load_percent=None):
        url = f"http://{self.host}:{self.port}/1/summary"
        headers = {"Accept": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=self.timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
        sample = self.qween.from_xmrig_summary(
            payload,
            cpu_temperature_c=cpu_temperature_c,
            gpu_temperature_c=gpu_temperature_c,
            cpu_load_percent=cpu_load_percent,
        )
        self.qween.ingest(sample)
        return sample
