from __future__ import annotations

import logging
from logging.handlers import RotatingFileHandler
from pathlib import Path


class ShellLog:
    """Append shell output to a bounded, UTF-8 rotating log file."""

    def __init__(
        self,
        path: Path,
        *,
        max_bytes: int = 5 * 1024 * 1024,
        backup_count: int = 5,
    ) -> None:
        if max_bytes < 1:
            raise ValueError("max_bytes must be positive")
        if backup_count < 0:
            raise ValueError("backup_count cannot be negative")

        path.parent.mkdir(parents=True, exist_ok=True)
        self._path = path
        self._backup_count = backup_count
        self._logger = logging.Logger(f"unique_rx.shell.{id(self)}", logging.INFO)
        self._logger.propagate = False
        handler = RotatingFileHandler(
            path,
            maxBytes=max_bytes,
            backupCount=backup_count,
            encoding="utf-8",
            delay=True,
        )
        handler.setFormatter(logging.Formatter(
            "%(asctime)s %(levelname)s %(message)s",
            datefmt="%Y-%m-%d %H:%M:%S",
        ))
        self._handler = handler
        self._logger.addHandler(handler)

    def recent_lines(self, limit: int = 200) -> list[str]:
        limit = max(1, min(int(limit), 1000))
        paths = [
            self._path.with_name(f"{self._path.name}.{index}")
            for index in range(self._backup_count, 0, -1)
        ]
        paths.append(self._path)
        self._handler.acquire()
        try:
            lines: list[str] = []
            for path in paths:
                try:
                    lines.extend(path.read_text(encoding="utf-8", errors="replace").splitlines())
                except FileNotFoundError:
                    continue
            return lines[-limit:]
        finally:
            self._handler.release()

    def write(self, message: str, *, level: int = logging.INFO) -> None:
        self._logger.log(level, "%s", message)

    def close(self) -> None:
        for handler in self._logger.handlers[:]:
            handler.flush()
            handler.close()
            self._logger.removeHandler(handler)
