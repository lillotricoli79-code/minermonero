from __future__ import annotations

import logging
import logging.handlers
import queue
import re
import threading
import time
from pathlib import Path
from typing import TextIO


# Never persist pool credentials, API bearer tokens, or a Monero address in logs.
_SECRET_PATTERNS = (
    (re.compile(r"(?i)(\\b(?:password|passwd|pass|access[-_ ]?token|authorization)\\b\\s*[:=]\\s*)([^\\s,;]+)"), r"\\1<redacted>"),
    (re.compile(r"(?i)(\\bbearer\\s+)[A-Za-z0-9._~+/=-]+"), r"\\1<redacted>"),
    (re.compile(r"(?<![A-Za-z0-9])[48][A-Za-z0-9]{94}(?![A-Za-z0-9])"), "<wallet-redacted>"),
)


def _redact(value: str) -> str:
    result = str(value)
    for pattern, replacement in _SECRET_PATTERNS:
        result = pattern.sub(replacement, result)
    return result


class _BoundedQueueHandler(logging.handlers.QueueHandler):
    """Non-blocking producer: log I/O never runs on the caller's thread."""

    def __init__(self, log_queue: queue.Queue[logging.LogRecord], owner: "ShellLog") -> None:
        super().__init__(log_queue)
        self._owner = owner

    def enqueue(self, record: logging.LogRecord) -> None:
        try:
            self.queue.put_nowait(record)
        except queue.Full:
            self._owner._dropped_messages += 1


class _DrainingQueueListener(logging.handlers.QueueListener):
    """Use a blocking sentinel insert so close drains a full queue safely."""

    def enqueue_sentinel(self) -> None:
        self.queue.put(self._sentinel)


class ShellLog:
    """Thread-safe asynchronous shell log with rotation, retention and redaction.

    Producers only enqueue records. A dedicated listener performs disk I/O,
    so a slow or temporarily unavailable disk cannot stall mining workers.
    """

    def __init__(
        self,
        path: Path,
        *,
        max_bytes: int = 8 * 1024 * 1024,
        backup_count: int = 3,
        queue_capacity: int = 8192,
        level: int = logging.INFO,
    ) -> None:
        if max_bytes < 1:
            raise ValueError("max_bytes must be positive")
        if backup_count < 0:
            raise ValueError("backup_count cannot be negative")
        if queue_capacity < 64:
            raise ValueError("queue_capacity must be at least 64")

        self._path = Path(path)
        self._backup_count = backup_count
        self._queue: queue.Queue[logging.LogRecord] = queue.Queue(maxsize=queue_capacity)
        self._state_lock = threading.RLock()
        self._dropped_messages = 0
        self._closed = False

        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._file_handler = logging.handlers.RotatingFileHandler(
            self._path,
            maxBytes=max_bytes,
            backupCount=backup_count,
            encoding="utf-8",
            delay=True,
        )
        self._file_handler.setLevel(level)
        self._file_handler.setFormatter(logging.Formatter(
            "%(asctime)s %(levelname)-8s [%(threadName)s:%(thread)d] %(message)s",
            datefmt="%Y-%m-%d %H:%M:%S",
        ))

        self._queue_handler = _BoundedQueueHandler(self._queue, self)
        self._queue_handler.setLevel(level)
        self._listener = _DrainingQueueListener(
            self._queue, self._file_handler, respect_handler_level=True
        )
        self._logger = logging.Logger(f"unique_rx.shell.{id(self)}", level)
        self._logger.propagate = False
        self._logger.addHandler(self._queue_handler)
        self._listener.start()

    @property
    def dropped_messages(self) -> int:
        with self._state_lock:
            return self._dropped_messages

    def recent_lines(self, limit: int = 200) -> list[str]:
        limit = max(1, min(int(limit), 1000))
        paths = [
            self._path.with_name(f"{self._path.name}.{index}")
            for index in range(self._backup_count, 0, -1)
        ]
        paths.append(self._path)
        # RotatingFileHandler uses this same lock while appending/rotating.
        self._file_handler.acquire()
        try:
            lines: list[str] = []
            for path in paths:
                try:
                    lines.extend(path.read_text(encoding="utf-8", errors="replace").splitlines())
                except FileNotFoundError:
                    continue
            return lines[-limit:]
        finally:
            self._file_handler.release()

    def write(
        self,
        message: str,
        *,
        level: int = logging.INFO,
        exc_info: bool | tuple[type[BaseException], BaseException, object] = False,
    ) -> None:
        safe_message = _redact(message)
        with self._state_lock:
            if self._closed:
                return
            self._logger.log(level, "%s", safe_message, exc_info=exc_info)

    def exception(self, message: str) -> None:
        self.write(message, level=logging.ERROR, exc_info=True)

    def flush(self) -> None:
        # Queue.join is deliberately avoided: QueueListener does not expose
        # task_done accounting. The handler lock waits for any active write.
        while not self._queue.empty():
            time.sleep(0.005)
        self._file_handler.acquire()
        try:
            self._file_handler.flush()
        finally:
            self._file_handler.release()

    def close(self) -> None:
        with self._state_lock:
            if self._closed:
                return
            self._closed = True
            self._logger.removeHandler(self._queue_handler)
        # The listener consumes every queued record before receiving its
        # sentinel. Joining it guarantees an ordered, complete shutdown.
        self._listener.stop()
        self._file_handler.flush()
        self._file_handler.close()
        self._queue_handler.close()

    def __enter__(self) -> "ShellLog":
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        if exc_type is not None:
            self.write(
                f"Shell logger context exited with {exc_type.__name__}: {exc_value}",
                level=logging.ERROR,
            )
        self.close()
