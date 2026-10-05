"""Behavioral checks for the SystemOne transport benchmark output."""

from __future__ import annotations

import json
import importlib.util
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "test" / "benchmark" / "run_benchmark.py"
SPEC = importlib.util.spec_from_file_location("systemone_benchmark", RUNNER)
assert SPEC is not None and SPEC.loader is not None
BENCHMARK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCHMARK)


def test_benchmark_reports_transport_io_thread_count() -> None:
    """Keep the measured transport configuration visible with its results."""
    completed = subprocess.run(
        [
            sys.executable,
            str(RUNNER),
            "--rows",
            "2",
            "--questions",
            "1",
            "--max-inflight",
            "1",
            "--queue-capacity",
            "1",
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
        timeout=30,
    )
    report = json.loads(completed.stdout)
    assert report["configuration"]["transport_io_threads"] == 2


def test_cold_ollama_stop_command_has_a_timeout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """A stuck Ollama CLI must not bypass the benchmark's cold-start bounds."""
    binary_dir = tmp_path / "bin"
    binary_dir.mkdir()
    fake_ollama = binary_dir / "ollama"
    fake_ollama.write_text("#!/usr/bin/env python3\nimport time\ntime.sleep(2)\n", encoding="utf-8")
    fake_ollama.chmod(0o755)
    monkeypatch.setenv("PATH", f"{binary_dir}{os.pathsep}{os.environ.get('PATH', '')}")
    monkeypatch.setattr(BENCHMARK, "OLLAMA_STOP_TIMEOUT_SECONDS", 0.1, raising=False)
    monkeypatch.setattr(BENCHMARK, "_stats", lambda _url: pytest.fail("cold stop did not time out"))

    started = time.monotonic()
    with pytest.raises(TimeoutError, match="ollama stop"):
        BENCHMARK._prepare_cold_ollama("http://localhost:11434/v1/systemone")
    assert time.monotonic() - started < 1.5
