#!/usr/bin/env python3
"""Deterministic HTTP/1.1 mock for the SystemOne transport tests."""

from __future__ import annotations

import argparse
import json
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit


class State:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.epoch = 0
        self.reset()

    def reset(self) -> None:
        with self.lock:
            self.epoch += 1
            self.requests = 0
            self.active = 0
            self.max_active = 0
            self.connections: set[str] = set()
            self.reuse_close_triggered = 0
            self.states: dict[str, int] = {}
            self.completed_states: list[str] = []
            self.connection_headers: list[str] = []
            self.overlap_gates: dict[str, dict[str, threading.Event]] = {}

    def begin(self, client: tuple[str, int], state: str, connection: str) -> tuple[int, bool]:
        with self.lock:
            self.requests += 1
            self.active += 1
            self.max_active = max(self.max_active, self.active)
            connection_id = f"{client[0]}:{client[1]}"
            reused = connection_id in self.connections
            self.connections.add(connection_id)
            self.states[state] = self.states.get(state, 0) + 1
            self.connection_headers.append(connection.lower())
            gate = self._overlap_gate_parts(state)
            if gate is not None:
                group, role = gate
                events = self.overlap_gates.setdefault(
                    group,
                    {
                        "slow_started": threading.Event(),
                        "fast_completed": threading.Event(),
                    },
                )
                if role == "slow":
                    events["slow_started"].set()
            return self.epoch, reused

    @staticmethod
    def _overlap_gate_parts(state: str) -> tuple[str, str] | None:
        parts = state.split(":", 2)
        if len(parts) == 3 and parts[0] == "gate" and parts[2] in {"slow", "fast"}:
            return parts[1], parts[2]
        return None

    def wait_for_overlap_peer(self, state: str) -> None:
        gate = self._overlap_gate_parts(state)
        if gate is None:
            return
        group, role = gate
        with self.lock:
            events = self.overlap_gates.get(group)
        if events is None:
            return
        # Bound the rendezvous so a broken caller cannot stall the mock server.
        if role == "fast":
            events["slow_started"].wait(timeout=5)
        else:
            events["fast_completed"].wait(timeout=5)

    def record_reuse_close(self, epoch: int) -> None:
        with self.lock:
            if epoch == self.epoch:
                self.reuse_close_triggered += 1

    def end(self, epoch: int, state: str) -> None:
        with self.lock:
            if epoch == self.epoch:
                self.active -= 1
                self.completed_states.append(state)
                gate = self._overlap_gate_parts(state)
                if gate is not None and gate[1] == "fast":
                    events = self.overlap_gates.get(gate[0])
                    if events is not None:
                        events["fast_completed"].set()

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            return {
                "requests": self.requests,
                "active": self.active,
                "max_active": self.max_active,
                "connection_count": len(self.connections),
                "connections": sorted(self.connections),
                "reuse_close_triggered": self.reuse_close_triggered,
                "states": dict(self.states),
                "completed_states": list(self.completed_states),
                "connection_headers": list(self.connection_headers),
            }


def response_for(body: dict[str, Any], state: str) -> tuple[int, bytes, str, int]:
    delay_ms = 0
    response_state = state
    if state.startswith("delay:"):
        parts = state.split(":", 2)
        try:
            delay_ms = max(0, min(int(parts[1]), 10000))
            response_state = parts[2]
        except (IndexError, ValueError):
            return 400, b'{"error":"invalid delay state"}', "application/json", 0
    if response_state == "missing-model":
        return 404, b'{"error":"model not found: tev1:0.8b"}', "application/json", delay_ms
    if response_state == "unsupported-route":
        return 404, b'{"error":"route not found"}', "application/json", delay_ms
    if response_state == "http-500":
        return 500, b'{"error":"test server failure"}', "application/json", delay_ms
    if response_state == "malformed":
        return 200, b'{"model":', "application/json", delay_ms
    if response_state == "oversize":
        result = valid_response(body)
        result["test_padding"] = "x" * 32768
        return 200, json.dumps(result, separators=(",", ":")).encode(), "application/json", delay_ms
    response = valid_response(body)
    return 200, json.dumps(response, separators=(",", ":")).encode(), "application/json", delay_ms


def valid_response(body: dict[str, Any]) -> dict[str, Any]:
    answers: dict[str, Any] = {}
    questions = body.get("questions", {})
    if not isinstance(questions, dict):
        questions = {}
    for name, question in questions.items():
        kind = question.get("type") if isinstance(question, dict) else None
        if kind == "choice":
            criteria = question.get("criteria", {})
            labels = list(criteria) if isinstance(criteria, dict) else []
            selected = labels[0] if labels else "other"
            answers[name] = {
                "type": "choice",
                "choice": selected,
                "probabilities": {label: 1.0 if label == selected else 0.0 for label in labels},
                "confidence": 1.0,
            }
        elif kind == "score":
            criteria = question.get("criteria", [])
            labels = list(criteria) if isinstance(criteria, list) else []
            legend = {str(index): label for index, label in enumerate(labels)}
            probabilities = {str(index): 1.0 if index == 0 else 0.0 for index in range(len(labels))}
            answers[name] = {
                "type": "score",
                "score": 0,
                "legend": legend,
                "probabilities": probabilities,
                "confidence": 1.0,
            }
        else:
            answers[name] = {"type": "noul", "noul": 0.75}
    return {
        "model": "tev1:0.8b",
        "answers": answers,
        "usage": {"input_tokens": 5, "output_tokens": 3},
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "SystemOneMock/1.0"

    @property
    def state(self) -> State:
        return self.server.mock_state  # type: ignore[attr-defined]

    def log_message(self, _format: str, *_args: Any) -> None:
        # Keep test output deterministic and avoid logging request bodies.
        return

    def _send(self, status: int, payload: bytes, content_type: str = "application/json") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            # Expected for timeout, oversize, and cancellation scenarios.
            pass

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = urlsplit(self.path).path
        if path == "/__stats":
            payload = json.dumps(self.state.snapshot(), separators=(",", ":")).encode()
            self._send(200, payload)
        elif path == "/__reset":
            self.state.reset()
            self._send(200, b"{}")
        else:
            self._send(404, b'{"error":"route not found"}')

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = urlsplit(self.path).path
        try:
            length = int(self.headers.get("Content-Length", "0"))
            raw_body = self.rfile.read(length)
            body = json.loads(raw_body)
        except (ValueError, json.JSONDecodeError):
            self._send(400, b'{"error":"invalid request JSON"}')
            return

        state_name = body.get("state", "") if isinstance(body, dict) else ""
        state_name = str(state_name)
        epoch, reused_connection = self.state.begin(
            self.client_address, state_name, self.headers.get("Connection", "")
        )
        try:
            if state_name == "reuse-close" and reused_connection:
                self.state.record_reuse_close(epoch)
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                self.connection.close()
                return
            if path != "/v1/systemone":
                status, payload, content_type, delay_ms = (
                    404,
                    b'{"error":"route not found"}',
                    "application/json",
                    0,
                )
            else:
                status, payload, content_type, delay_ms = response_for(body, state_name)
                self.state.wait_for_overlap_peer(state_name)
            if delay_ms:
                time.sleep(delay_ms / 1000.0)
            self._send(status, payload, content_type)
        finally:
            self.state.end(epoch, state_name)


class Server(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int]):
        super().__init__(address, Handler)
        self.mock_state = State()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--ready-file", type=Path, required=True)
    args = parser.parse_args()

    server = Server((args.host, args.port))
    host, port = server.server_address[:2]
    ready = {"host": host, "port": port, "url": f"http://{host}:{port}/v1/systemone"}
    args.ready_file.write_text(json.dumps(ready), encoding="utf-8")
    try:
        server.serve_forever(poll_interval=0.05)
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
