"""SQL-level concurrency and cancellation checks for SystemOne."""

from __future__ import annotations

import json
import os
import signal
import subprocess
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any, Callable

import pytest


ROOT = Path(__file__).resolve().parents[2]
CONFIG = (
    '{"questions":{"sentiment":{"type":"noul",'
    '"instructions":"Return a probability."}}}'
)


def _read_stats(server: dict[str, Any]) -> dict[str, Any]:
    with urllib.request.urlopen(server["stats_url"], timeout=3) as response:
        return json.loads(response.read())


def _row_query(*, rows: int, prefix: str, delay_ms: int) -> str:
    return f"""
SET ollama_max_inflight = 2;
SET ollama_queue_capacity = 2;
SELECT count(result) AS result_count
FROM (
    SELECT ollama_systemone(
        'delay:{delay_ms}:{prefix}-' || CAST(i AS VARCHAR),
        '{CONFIG}'
    ) AS result
    FROM range({rows}) AS input(i)
) AS evaluated;
"""


def _wait_for_stats(
    server: dict[str, Any],
    predicate: Callable[[dict[str, Any]], bool],
    *,
    timeout: float,
    process: subprocess.Popen[str] | None = None,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last = _read_stats(server)
    while not predicate(last) and time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            break
        time.sleep(0.02)
        last = _read_stats(server)
    return last


def test_rows_are_dispatched_with_the_configured_per_expression_limit(
    run_systemone_sql: Callable[..., subprocess.CompletedProcess[str]],
    systemone_mock_server: dict[str, Any],
) -> None:
    row_count = 8
    completed = run_systemone_sql(
        _row_query(rows=row_count, prefix="bounded", delay_ms=400),
        endpoint=systemone_mock_server["url"],
    )

    assert completed.returncode == 0, f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
    stats = _read_stats(systemone_mock_server)
    assert stats["requests"] == row_count
    assert stats["active"] == 0
    assert stats["max_active"] == 2
    assert stats["states"] == {f"delay:400:bounded-{i}": 1 for i in range(row_count)}


def test_independent_sql_queries_can_dispatch_at_the_same_time(
    run_systemone_sql: Callable[..., subprocess.CompletedProcess[str]],
    systemone_mock_server: dict[str, Any],
) -> None:
    start_together = threading.Barrier(2)

    def run_one(prefix: str) -> subprocess.CompletedProcess[str]:
        sql = f"""
SET ollama_max_inflight = 1;
SET ollama_queue_capacity = 1;
SELECT count(ollama_systemone(
    'delay:2500:{prefix}', '{CONFIG}'
)) AS result_count
FROM range(1);
"""
        start_together.wait(timeout=5)
        return run_systemone_sql(sql, endpoint=systemone_mock_server["url"], timeout=15)

    with ThreadPoolExecutor(max_workers=2) as executor:
        first = executor.submit(run_one, "query-a")
        second = executor.submit(run_one, "query-b")
        results = [first.result(), second.result()]

    assert all(result.returncode == 0 for result in results), "\n".join(
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}" for result in results
    )
    stats = _read_stats(systemone_mock_server)
    assert stats["requests"] == 2
    assert stats["active"] == 0
    assert stats["max_active"] == 2
    assert stats["states"] == {"delay:2500:query-a": 1, "delay:2500:query-b": 1}


def test_request_failure_reports_its_input_slot(
    run_systemone_sql: Callable[..., subprocess.CompletedProcess[str]],
    systemone_mock_server: dict[str, Any],
) -> None:
    completed = run_systemone_sql(
        f"""
SET ollama_max_inflight = 1;
SET ollama_queue_capacity = 1;
SELECT ollama_systemone(
    CASE WHEN i = 1 THEN 'delay:250:missing-model' ELSE 'success-' || CAST(i AS VARCHAR) END,
    '{CONFIG}'
)
FROM range(100) AS input(i);
""",
        endpoint=systemone_mock_server["url"],
    )

    diagnostic = completed.stdout + completed.stderr
    assert completed.returncode != 0, diagnostic
    assert "slot 2" in diagnostic.lower() or "row 2" in diagnostic.lower(), diagnostic
    stats = _read_stats(systemone_mock_server)
    assert stats["states"].get("delay:250:missing-model") == 1
    assert stats["requests"] == 2, stats
    assert stats["active"] == 0


def test_failure_order_uses_the_lowest_slot_when_a_later_slot_fails_first(
    run_systemone_sql: Callable[..., subprocess.CompletedProcess[str]],
    systemone_mock_server: dict[str, Any],
) -> None:
    completed = run_systemone_sql(
        f"""
SET ollama_max_inflight = 2;
SET ollama_queue_capacity = 0;
SELECT ollama_systemone(state, '{CONFIG}')
FROM (VALUES
    (1, 'delay:250:missing-model'),
    (2, 'missing-model'),
    (3, 'must_not_be_submitted')
) AS input(slot, state)
ORDER BY slot;
""",
        endpoint=systemone_mock_server["url"],
    )

    diagnostic = completed.stdout + completed.stderr
    assert completed.returncode != 0, diagnostic
    assert "slot 1" in diagnostic.lower() or "row 1" in diagnostic.lower(), diagnostic
    stats = _read_stats(systemone_mock_server)
    assert stats["requests"] == 2, stats
    assert stats["states"] == {"delay:250:missing-model": 1, "missing-model": 1}
    assert stats["completed_states"] == ["missing-model", "delay:250:missing-model"]


def test_prepared_plan_reads_new_settings_for_each_query(
    run_systemone_sql: Callable[..., subprocess.CompletedProcess[str]],
    systemone_mock_server: dict[str, Any],
) -> None:
    completed = run_systemone_sql(
        f"""
PREPARE systemone_settings AS
    SELECT count(result) AS result_count
    FROM (
        SELECT ollama_systemone(
            'delay:120:prepared-' || CAST(i AS VARCHAR), '{CONFIG}'
        ) AS result
        FROM range(2) AS input(i)
    ) AS evaluated;
SET ollama_max_inflight = 1;
SET ollama_queue_capacity = 1;
EXECUTE systemone_settings;
SET ollama_max_inflight = 2;
SET ollama_queue_capacity = 0;
EXECUTE systemone_settings;
""",
        endpoint=systemone_mock_server["url"],
    )

    assert completed.returncode == 0, f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
    stats = _read_stats(systemone_mock_server)
    assert stats["requests"] == 4
    assert stats["max_active"] == 2
    assert stats["states"] == {"delay:120:prepared-0": 2, "delay:120:prepared-1": 2}


def test_sigint_cancels_active_sql_work_and_cleans_up(
    duckdb_cli: Path,
    systemone_extension: Path,
    systemone_mock_server: dict[str, Any],
) -> None:
    setup = (
        f"LOAD '{systemone_extension.as_posix()}'; "
        f"SET ollama_endpoint = '{systemone_mock_server['url']}'; "
        "SET ollama_max_inflight = 2; "
        "SET ollama_queue_capacity = 2; "
        "SET ollama_request_timeout_ms = 10000; "
    )
    query = f"""
SELECT ollama_systemone(
    'delay:1000:cancel-' || CAST(i AS VARCHAR), '{CONFIG}'
)
FROM range(100) AS input(i);
"""
    environment = os.environ.copy()
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"):
        environment.pop(name, None)
    environment["NO_PROXY"] = "*"
    environment["no_proxy"] = "*"

    process = subprocess.Popen(
        [str(duckdb_cli), "-unsigned", "-json", "-no-init", "-c", setup + query],
        cwd=ROOT,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        active = _wait_for_stats(
            systemone_mock_server,
            lambda stats: stats["active"] > 0,
            timeout=8,
            process=process,
        )
        assert active["active"] > 0, (
            "the CLI exited or no HTTP request became active before interruption; "
            f"returncode={process.poll()}, stats={active}"
        )
        process.send_signal(signal.SIGINT)
        try:
            stdout, stderr = process.communicate(timeout=6)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
            pytest.fail(f"DuckDB CLI did not stop after SIGINT\nstdout:\n{stdout}\nstderr:\n{stderr}")

        assert process.returncode != 0, f"SIGINT did not interrupt the SQL query\nstdout:\n{stdout}\nstderr:\n{stderr}"
        cleaned = _wait_for_stats(
            systemone_mock_server,
            lambda stats: stats["active"] == 0,
            timeout=5,
        )
        assert cleaned["active"] == 0, cleaned
        assert cleaned["requests"] < 100, "cancellation still dispatched every row"
        assert any(state.startswith("delay:1000:cancel-") for state in cleaned["states"])
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
