"""Shared SQL CLI and mock-server fixtures for scalar integration tests."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
from typing import Any, Callable

import pytest


ROOT = Path(__file__).resolve().parents[2]
MOCK_SERVER_SCRIPT = ROOT / "test" / "mock" / "server.py"


def get_json(url: str) -> dict[str, Any]:
    with urllib.request.urlopen(url, timeout=3) as response:
        return json.loads(response.read())


@pytest.fixture(scope="session")
def duckdb_cli() -> Path:
    configured = os.environ.get("SYSTEMONE_DUCKDB_CLI")
    candidates = [Path(configured)] if configured else []
    candidates.extend([ROOT / "build" / "release" / "duckdb", ROOT / "build" / "debug" / "duckdb"])
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise AssertionError("matching DuckDB CLI is missing; build the v1.5.6 shell first")


@pytest.fixture(scope="session")
def systemone_extension() -> Path:
    configured = os.environ.get("SYSTEMONE_EXTENSION")
    candidates = [Path(configured)] if configured else []
    candidates.extend(
        [
            ROOT / "build" / "release" / "extension" / "ollama_systemone" / "ollama_systemone.duckdb_extension",
            ROOT / "build" / "release" / "repository" / "v1.5.6" / "linux_amd64" / "ollama_systemone.duckdb_extension",
            ROOT / "build" / "debug" / "extension" / "ollama_systemone" / "ollama_systemone.duckdb_extension",
        ]
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise AssertionError("loadable SystemOne extension is missing; build the extension first")


@pytest.fixture
def run_systemone_sql(duckdb_cli: Path, systemone_extension: Path) -> Callable[..., subprocess.CompletedProcess[str]]:
    def run(sql: str, *, endpoint: str | None = None, timeout: float = 30) -> subprocess.CompletedProcess[str]:
        setup = f"LOAD '{systemone_extension.as_posix()}';"
        if endpoint is not None:
            escaped_endpoint = endpoint.replace("'", "''")
            setup += f" SET ollama_endpoint = '{escaped_endpoint}';"
        environment = os.environ.copy()
        for name in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"):
            environment.pop(name, None)
        environment["NO_PROXY"] = "*"
        environment["no_proxy"] = "*"
        return subprocess.run(
            [str(duckdb_cli), "-unsigned", "-json", "-no-init", "-c", setup + " " + sql],
            cwd=ROOT,
            env=environment,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )

    return run


@pytest.fixture
def systemone_mock_server(tmp_path: Path) -> dict[str, Any]:
    ready_file = tmp_path / "ready.json"
    process = subprocess.Popen(
        [sys.executable, str(MOCK_SERVER_SCRIPT), "--host", "127.0.0.1", "--port", "0", "--ready-file", str(ready_file)],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not ready_file.exists():
            if process.poll() is not None:
                stderr = process.stderr.read() if process.stderr else ""
                raise AssertionError(f"mock server exited before startup: {stderr}")
            time.sleep(0.02)
        assert ready_file.exists(), "mock server did not publish its ready file"
        server = json.loads(ready_file.read_text(encoding="utf-8"))
        server["stats_url"] = f"http://{server['host']}:{server['port']}/__stats"
        server["process"] = process
        yield server
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
        if process.stderr:
            process.stderr.close()
