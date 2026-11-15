"""Shared QMP client over a private Unix domain socket."""

from __future__ import annotations

from collections import deque
import json
import socket
import sys
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from qemutests.harness.ppm import Surface, parse_ppm


class QmpClient:
    """QMP client with monotonic shared deadlines, transcript logging, and screendump."""

    def __init__(self, path: str, timeout: float = 15.0):
        self.path = path
        self.timeout = timeout
        self.sock: Optional[socket.socket] = None
        self._buffer = b""
        self._events: deque[dict] = deque(maxlen=64)
        self.transcript: Optional[Path] = None
        self._transcript_fp = None

    def connect(self, *, deadline: float | None = None) -> None:
        if deadline is None:
            deadline = time.monotonic() + self.timeout
        err = None
        while time.monotonic() < deadline:
            attempt = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                attempt.settimeout(self._remaining(deadline))
                attempt.connect(self.path)
                self.sock = attempt
                break
            except OSError as exc:
                attempt.close()
                err = exc
                time.sleep(min(0.1, max(0, deadline - time.monotonic())))
        if self.sock is None:
            raise RuntimeError(f"QMP connect failed: {err}")
        try:
            greeting = self._read_message(deadline)
            if "QMP" not in greeting:
                raise RuntimeError("QMP greeting missing")
            self.execute("qmp_capabilities", deadline=deadline)
        except BaseException:
            self.close()
            raise

    def _remaining(self, deadline: float) -> float:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("QMP total deadline expired")
        return remaining

    def _next_json_message(self, deadline: float) -> dict:
        if self.sock is None:
            raise RuntimeError("QMP not connected")
        while True:
            self.sock.settimeout(self._remaining(deadline))
            if b"\n" not in self._buffer:
                data = self.sock.recv(4096)
                if not data:
                    raise RuntimeError("QMP connection closed")
                self._buffer += data
                if len(self._buffer) > 1024 * 1024:
                    raise RuntimeError("QMP message too large")
                continue
            line, self._buffer = self._buffer.split(b"\n", 1)
            return json.loads(line)

    def _read_message(self, deadline: float) -> dict:
        while True:
            msg = self._next_json_message(deadline)
            if "event" not in msg:
                return msg
            self._events.append(msg)

    def wait_event(
        self,
        name: str,
        match: Optional[Callable[[dict], bool]] = None,
        *,
        deadline: float | None = None,
    ) -> dict:
        """Wait for a QMP async event matching ``name`` and optional ``match(event)``."""
        if deadline is None:
            deadline = time.monotonic() + self.timeout
        for idx, ev in enumerate(self._events):
            if ev.get("event") == name and (match is None or match(ev)):
                del self._events[idx]
                return ev
        while True:
            msg = self._next_json_message(deadline)
            if "event" in msg:
                if msg.get("event") == name and (match is None or match(msg)):
                    return msg
                self._events.append(msg)

    def execute(self, cmd: str, args: Optional[dict] = None, *,
                deadline: float | None = None) -> dict:
        if self.sock is None:
            raise RuntimeError("QMP not connected")
        if deadline is None:
            deadline = time.monotonic() + self.timeout
        request: Dict[str, Any] = {"execute": cmd}
        if args is not None:
            request["arguments"] = args
        self.sock.settimeout(self._remaining(deadline))
        self.sock.sendall((json.dumps(request) + "\n").encode())
        msg = self._read_message(deadline)
        if "error" in msg:
            raise RuntimeError(f"QMP error {cmd}: {msg['error']}")
        if "return" not in msg:
            raise RuntimeError(f"QMP response missing return: {msg}")
        return msg["return"]

    def cmd(self, name: str, args: Optional[dict] = None, *,
            deadline: float | None = None) -> dict:
        """Alias for ``execute``."""
        return self.execute(name, args, deadline=deadline)

    def screendump(self, path: Path) -> Surface:
        self.execute("screendump", {"filename": str(path)})
        data = Path(path).read_bytes()
        return parse_ppm(data)

    def input_events(
        self, events: List[dict], *, deadline: float | None = None
    ) -> None:
        self._record_transcript(events)
        if deadline is not None:
            self.execute("input-send-event", {"events": events}, deadline=deadline)
        else:
            self.execute("input-send-event", {"events": events})

    def set_transcript(self, path: Path) -> None:
        self.close_transcript()
        self.transcript = Path(path)
        try:
            self.transcript.parent.mkdir(parents=True, exist_ok=True)
            self._transcript_fp = self.transcript.open("a", encoding="utf-8")
        except OSError as exc:
            print(
                f"[qmp] warning: cannot write the QMP transcript to "
                f"{self.transcript}: {exc}",
                file=sys.stderr,
            )
            self._transcript_fp = None

    def _record_transcript(self, events: List[dict]) -> None:
        if self._transcript_fp is None:
            if self.transcript is None:
                return
            self.set_transcript(self.transcript)
            if self._transcript_fp is None:
                return
        line = json.dumps(
            {"command": "input-send-event", "events": events}, sort_keys=True
        )
        try:
            self._transcript_fp.write(line + "\n")
            self._transcript_fp.flush()
        except OSError as exc:
            print(f"[qmp] warning: cannot write the QMP transcript: {exc}",
                  file=sys.stderr)

    def close_transcript(self) -> None:
        if self._transcript_fp is not None:
            try:
                self._transcript_fp.close()
            except OSError:
                pass
            self._transcript_fp = None

    def close(self) -> None:
        self.close_transcript()
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            finally:
                self.sock = None
