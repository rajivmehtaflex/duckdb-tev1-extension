"""End-to-end checks for the pure C++ SystemOne HTTP dispatcher."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
from typing import Any

import pytest


ROOT = Path(__file__).resolve().parents[2]
SERVER_SCRIPT = ROOT / "test" / "mock" / "server.py"


def _driver_path() -> Path:
    configured = os.environ.get("SYSTEMONE_TRANSPORT_TEST_DRIVER")
    candidates = [Path(configured)] if configured else []
    candidates.extend(
        [
            ROOT / "build" / "release" / "extension" / "ollama_systemone" / "systemone_transport_test_driver",
            ROOT / "build" / "debug" / "extension" / "ollama_systemone" / "systemone_transport_test_driver",
            ROOT / "build" / "systemone_transport_test_driver",
        ]
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    searched = "\n  ".join(str(candidate) for candidate in candidates)
    raise AssertionError(
        "transport test driver was not found; build the systemone_transport_test_driver target first. "
        f"Searched:\n  {searched}"
    )


def _get_json(url: str) -> dict[str, Any]:
    with urllib.request.urlopen(url, timeout=3) as response:
        return json.loads(response.read())


@pytest.fixture(scope="module")
def mock_server(tmp_path_factory: pytest.TempPathFactory) -> dict[str, Any]:
    ready_file = tmp_path_factory.mktemp("systemone-mock") / "ready.json"
    process = subprocess.Popen(
        [sys.executable, str(SERVER_SCRIPT), "--host", "127.0.0.1", "--port", "0", "--ready-file", str(ready_file)],
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
        server["reset_url"] = f"http://{server['host']}:{server['port']}/__reset"
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


@pytest.fixture(autouse=True)
def reset_mock(mock_server: dict[str, Any]) -> None:
    _get_json(mock_server["reset_url"])


@pytest.fixture(scope="session")
def driver() -> Path:
    return _driver_path()


def run_driver(driver: Path, mode: str, endpoint: str, timeout: float = 12) -> dict[str, Any]:
    completed = subprocess.run(
        [str(driver), mode, endpoint],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    assert completed.returncode == 0, (
        f"transport_test_driver {mode} exited {completed.returncode}\n"
        f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
    )
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise AssertionError(f"driver did not print JSON: {completed.stdout!r}\n{completed.stderr}") from error


def _results(output: dict[str, Any]) -> dict[int, dict[str, Any]]:
    return {int(result["slot"]): result for result in output["results"]}


def test_bounds_active_queued_and_unconsumed_results(driver: Path, mock_server: dict[str, Any]) -> None:
    output = run_driver(driver, "concurrency", mock_server["url"])
    observations = output["observations"]
    mid = observations["mid_stats"]
    pending = observations["completed_pending_stats"]

    assert observations["returned_before_consume"] == 3
    assert mid["active"] == 2
    assert mid["queued"] == 1
    assert observations["returned_with_unconsumed_result"] == 3
    assert pending["completed_pending"] >= 1
    assert observations["released_after_result_consumed"] is True
    assert sorted(result["slot"] for result in output["results"]) == [1, 2, 3, 4]
    assert all(result["success"] for result in output["results"])
    assert output["stats"]["completed"] == 4
    assert output["stats"]["active"] == 0
    assert output["stats"]["queued"] == 0
    assert output["stats"]["completed_pending"] == 0

    server_stats = _get_json(mock_server["stats_url"])
    assert server_stats["requests"] == 4
    assert server_stats["max_active"] == 2
    assert server_stats["active"] == 0


def test_keep_alive_reuses_http_connections(driver: Path, mock_server: dict[str, Any]) -> None:
    output = run_driver(driver, "keepalive", mock_server["url"])
    server_stats = _get_json(mock_server["stats_url"])

    assert len(output["results"]) == 6
    assert all(result["success"] for result in output["results"])
    assert server_stats["requests"] == 6
    assert server_stats["connection_count"] <= 2
    assert all("keep-alive" in header for header in server_stats["connection_headers"])


def test_results_keep_their_original_slots_when_completions_arrive_out_of_order(
    driver: Path, mock_server: dict[str, Any]
) -> None:
    output = run_driver(driver, "out_of_order", mock_server["url"])
    results = _results(output)

    assert output["completion_order"] == [20, 10]
    assert set(results) == {10, 20}
    assert results[10]["success"] is True
    assert results[20]["success"] is True


def test_404_errors_distinguish_missing_model_from_unsupported_route(
    driver: Path, mock_server: dict[str, Any]
) -> None:
    output = run_driver(driver, "not_found", mock_server["url"])
    results = _results(output)

    assert results[1]["error"] == "MODEL_NOT_FOUND"
    assert results[2]["error"] == "UNSUPPORTED_ROUTE"
    assert _get_json(mock_server["stats_url"])["requests"] == 2


def test_http_status_errors_are_not_retried(driver: Path, mock_server: dict[str, Any]) -> None:
    output = run_driver(driver, "http_error", mock_server["url"])
    result = output["results"][0]

    assert result["success"] is False
    assert result["error"] == "HTTP_STATUS"
    assert result["http_status"] == 500
    assert result["retries"] == 0
    assert output["stats"]["retry_count"] == 0
    assert _get_json(mock_server["stats_url"])["requests"] == 1


def test_malformed_response_is_rejected(driver: Path, mock_server: dict[str, Any]) -> None:
    output = run_driver(driver, "malformed", mock_server["url"])

    assert output["results"][0]["error"] == "INVALID_RESPONSE"
    assert output["results"][0]["success"] is False


def test_oversized_response_is_stopped_at_the_configured_limit(driver: Path, mock_server: dict[str, Any]) -> None:
    output = run_driver(driver, "oversize", mock_server["url"])

    assert output["results"][0]["error"] == "RESPONSE_TOO_LARGE"
    assert output["results"][0]["success"] is False
    assert output["stats"]["response_bytes"] > 0


def test_request_timeout_is_categorized(driver: Path, mock_server: dict[str, Any]) -> None:
    output = run_driver(driver, "timeout", mock_server["url"])

    assert output["results"][0]["error"] == "TIMEOUT"
    assert output["stats"]["timeout_count"] == 1


def test_connection_refusal_is_retried_only_before_upload(driver: Path) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
        endpoint = f"http://127.0.0.1:{port}/v1/systemone"
        output = run_driver(driver, "retry_refused", endpoint)
    result = output["results"][0]

    assert result["error"] == "CONNECTION_REFUSED"
    assert result["retries"] == 1
    assert output["stats"]["retry_count"] == 1


def test_cancel_aborts_active_and_queued_work_and_tears_down(driver: Path, mock_server: dict[str, Any]) -> None:
    started = time.monotonic()
    output = run_driver(driver, "cancel", mock_server["url"], timeout=6)
    elapsed = time.monotonic() - started

    assert elapsed < 4
    assert len(output["results"]) == 3
    assert all(result["error"] == "CANCELLED" for result in output["results"])
    assert output["stats"]["cancelled_count"] == 3
    assert output["stats"]["active"] == 0
    assert output["stats"]["queued"] == 0
