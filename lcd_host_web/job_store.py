"""Thread-safe temporary download registry used by the media API."""

from __future__ import annotations

import os
import threading
import time
import uuid
from pathlib import Path
from threading import Timer
from dataclasses import dataclass, field


class DownloadStore:
    def __init__(self, root: Path, ttl_seconds: int = 300):
        self.root = Path(root)
        self.ttl_seconds = ttl_seconds
        self.entries: dict[str, dict] = {}
        self._lock = threading.RLock()
        self._timer: Timer | None = None

    def register(self, download_id: str, **metadata) -> None:
        with self._lock:
            metadata.setdefault("mtime", time.time())
            self.entries[download_id] = metadata
            self.schedule()

    def touch(self, download_id: str) -> dict | None:
        with self._lock:
            entry = self.entries.get(download_id)
            if entry:
                entry["mtime"] = time.time()
            return entry

    def schedule(self) -> None:
        with self._lock:
            if self._timer:
                self._timer.cancel()
            self._timer = Timer(self.ttl_seconds + 10, self.cleanup)
            self._timer.daemon = True
            self._timer.start()

    def cleanup(self) -> None:
        with self._lock:
            now = time.time()
            dead = [key for key, value in self.entries.items()
                    if now - value.get("mtime", now) > self.ttl_seconds]
            for key in dead:
                entry = self.entries.pop(key, None)
                if entry:
                    try:
                        os.unlink(entry["path"])
                    except OSError:
                        pass
            if self.entries:
                self.schedule()


@dataclass
class ConversionContext:
    job_id: str
    cancel_event: threading.Event = field(default_factory=threading.Event)
    progress: float = 0.0
    phase: str = "queued"
    detail: str = "排队中"
    total_frames: int = 0
    processed_frames: int = 0
    _processes: set = field(default_factory=set)
    _lock: threading.RLock = field(default_factory=threading.RLock)

    @property
    def cancelled(self) -> bool:
        return self.cancel_event.is_set()

    def register_process(self, process) -> None:
        with self._lock:
            self._processes.add(process)
            if self.cancelled:
                try:
                    process.kill()
                except OSError:
                    pass

    def report(self, progress: float, processed_frames: int | None = None,
               phase: str | None = None, detail: str | None = None) -> None:
        with self._lock:
            # Polling can observe updates while the worker transitions stages.
            self.progress = max(self.progress, max(0.0, min(100.0, float(progress))))
            if processed_frames is not None:
                self.processed_frames = max(0, int(processed_frames))
            if phase is not None:
                self.phase = phase
            if detail is not None:
                self.detail = detail

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "progress": self.progress,
                "phase": self.phase,
                "detail": self.detail,
                "processed_frames": self.processed_frames,
                "total_frames": self.total_frames,
            }

    def cancel(self) -> None:
        self.cancel_event.set()
        with self._lock:
            for process in tuple(self._processes):
                try:
                    process.kill()
                except OSError:
                    pass


class ConversionJobStore:
    """Small in-process job manager for the single-user desktop host."""

    def __init__(self, max_active: int = 2):
        self._jobs: dict[str, dict] = {}
        self._lock = threading.RLock()
        self.max_active = max_active

    def submit(self, worker) -> str:
        job_id = uuid.uuid4().hex
        context = ConversionContext(job_id)
        record = {"id": job_id, "status": "queued", "progress": 0.0,
                  "phase": "queued", "detail": "排队中",
                  "processed_frames": 0, "total_frames": 0,
                  "result": None, "error": None, "context": context}
        with self._lock:
            active = sum(value["status"] in {"queued", "running", "cancelling"}
                         for value in self._jobs.values())
            if active >= self.max_active:
                raise RuntimeError("Too many conversion jobs")
            self._jobs[job_id] = record

        def run():
            with self._lock:
                record["status"] = "running"
                record["phase"] = "preparing"
                record["detail"] = "准备转换..."
            try:
                result = worker(context)
                with self._lock:
                    if context.cancelled:
                        record["status"] = "cancelled"
                        record["phase"] = "cancelled"
                        record["detail"] = "已取消"
                    else:
                        record["status"] = "completed"
                        record["progress"] = 100.0
                        record["phase"] = "completed"
                        record["detail"] = "转换完成"
                        record["result"] = result
            except Exception as exc:
                with self._lock:
                    record["status"] = "cancelled" if context.cancelled else "failed"
                    record["phase"] = record["status"]
                    record["detail"] = str(exc)
                    record["error"] = str(exc)

        threading.Thread(target=run, name=f"convert-{job_id[:8]}", daemon=True).start()
        return job_id

    def get(self, job_id: str) -> dict | None:
        with self._lock:
            record = self._jobs.get(job_id)
            if not record:
                return None
            context = record["context"]
            snapshot = context.snapshot()
            if record["status"] == "completed":
                snapshot["progress"] = 100.0
                snapshot["phase"] = "completed"
                snapshot["detail"] = "转换完成"
            record.update(snapshot)
            return {key: value for key, value in record.items() if key != "context"}

    def cancel(self, job_id: str) -> bool:
        with self._lock:
            record = self._jobs.get(job_id)
            if not record or record["status"] in {"completed", "failed", "cancelled"}:
                return False
            record["context"].cancel()
            record["status"] = "cancelling"
            return True
