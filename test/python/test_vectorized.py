"""SQL-level coverage for SystemOne's DuckDB vector callback."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parents[2]


def _sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _load_json_extension() -> str:
    configured = os.environ.get("SYSTEMONE_JSON_EXTENSION")
    candidates = [Path(configured)] if configured else []
    candidates.extend(
        [
            ROOT / "build" / "release" / "extension" / "json" / "json.duckdb_extension",
            ROOT / "build" / "debug" / "extension" / "json" / "json.duckdb_extension",
            ROOT / "build" / "release" / "repository" / "v1.5.6" / "linux_amd64" / "json.duckdb_extension",
        ]
    )
    for candidate in candidates:
        if candidate.is_file():
            return f"LOAD {_sql_string(candidate.as_posix())};"
    return "INSTALL json; LOAD json;"


def _question_config(name: str, kind: str = "noul") -> str:
    question: dict[str, Any] = {"type": kind, "instructions": f"Classify {name}."}
    if kind == "choice":
        question["criteria"] = {"first": None, "second": None}
    elif kind == "score":
        question["criteria"] = ["low", "high"]
    return json.dumps({"questions": {name: question}}, separators=(",", ":"))


def _rows(completed: Any) -> list[dict[str, Any]]:
    assert completed.returncode == 0, (
        f"DuckDB SQL failed with exit code {completed.returncode}\n"
        f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
    )
    try:
        rows = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise AssertionError(f"DuckDB did not emit JSON rows: {completed.stdout!r}") from error
    assert isinstance(rows, list)
    return rows


def _decode_json(value: Any) -> Any:
    # DuckDB's JSON CLI renderer can emit a JSON value directly or as text.
    return json.loads(value) if isinstance(value, str) else value


def _stats(server: dict[str, Any]) -> dict[str, Any]:
    with urlopen(server["stats_url"], timeout=3) as response:
        return json.loads(response.read())


def test_mixed_question_answers_are_returned_as_decoded_json(
    run_systemone_sql: Any, systemone_mock_server: dict[str, Any]
) -> None:
    config = json.dumps(
        {
            "questions": {
                "noul_signal": {"type": "noul", "instructions": "Is the signal present?"},
                "choice_route": {
                    "type": "choice",
                    "instructions": "Choose a route.",
                    "criteria": {"priority": None, "standard": None},
                },
                "score_severity": {
                    "type": "score",
                    "instructions": "Score severity.",
                    "criteria": ["low", "high"],
                },
            }
        },
        separators=(",", ":"),
    )
    completed = run_systemone_sql(
        f"""
        {_load_json_extension()}
        WITH result_row AS MATERIALIZED (
            SELECT ollama_systemone('mixed_output_state', {_sql_string(config)}) AS result
        )
        SELECT result, json_extract(result, '$.answers.noul_signal.noul') AS extracted_probability
        FROM result_row
        """,
        endpoint=systemone_mock_server["url"],
    )

    rows = _rows(completed)
    assert len(rows) == 1
    result = _decode_json(rows[0]["result"])
    assert result["model"] == "tev1:0.8b"
    assert result["usage"] == {"input_tokens": 5, "output_tokens": 3}
    assert _decode_json(rows[0]["extracted_probability"]) == 0.75
    assert result["answers"]["noul_signal"] == {"type": "noul", "noul": 0.75}
    assert result["answers"]["choice_route"] == {
        "type": "choice",
        "choice": "priority",
        "probabilities": {"priority": 1.0, "standard": 0.0},
        "confidence": 1.0,
    }
    assert result["answers"]["score_severity"] == {
        "type": "score",
        "score": 0,
        "legend": {"0": "low", "1": "high"},
        "probabilities": {"0": 1.0, "1": 0.0},
        "confidence": 1.0,
    }
    server_stats = _stats(systemone_mock_server)
    assert server_stats["requests"] == 1
    assert server_stats["states"] == {"mixed_output_state": 1}


def test_varying_repeated_configs_map_through_filtered_dictionary_rows(
    run_systemone_sql: Any, systemone_mock_server: dict[str, Any]
) -> None:
    even_config = _sql_string(_question_config("even_config_answer"))
    odd_config = _sql_string(_question_config("odd_config_answer", "choice"))
    sql = f"""
        {_load_json_extension()}
        PRAGMA debug_verify_vector='dictionary_expression';
        SELECT i,
               list_extract(['dict_left', 'dict_middle', 'dict_right'], (i % 3) + 1) AS state,
               ollama_systemone(
                   list_extract(['dict_left', 'dict_middle', 'dict_right'], (i % 3) + 1),
                   list_extract([
                       CAST({even_config} AS JSON), CAST({odd_config} AS JSON)
                   ], (i % 2) + 1)
               ) AS result
        FROM range(0, 6) AS input(i)
        WHERE i % 4 <> 1
    """
    completed = run_systemone_sql(sql, endpoint=systemone_mock_server["url"])

    assert completed.returncode == 0, (
        f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}\n"
        f"mock stats: {_stats(systemone_mock_server)}"
    )
    rows = _rows(completed)
    expected_indices = [0, 2, 3, 4]
    by_index = {row["i"]: row for row in rows}
    assert set(by_index) == set(expected_indices)
    for index, row in by_index.items():
        expected_state = ["dict_left", "dict_middle", "dict_right"][index % 3]
        assert row["state"] == expected_state
        result = _decode_json(row["result"])
        expected_name = "even_config_answer" if index % 2 == 0 else "odd_config_answer"
        assert list(result["answers"]) == [expected_name]

    server_stats = _stats(systemone_mock_server)
    assert server_stats["requests"] == len(expected_indices)
    assert server_stats["states"] == {"dict_left": 2, "dict_middle": 1, "dict_right": 1}


def test_results_stay_aligned_across_multiple_vector_chunks(
    run_systemone_sql: Any, systemone_mock_server: dict[str, Any]
) -> None:
    before_boundary = _sql_string(_question_config("first_chunk_question"))
    after_boundary = _sql_string(_question_config("second_chunk_question", "score"))
    sql = f"""
        {_load_json_extension()}
        -- The local loopback server can exceed the default connect deadline during this burst.
        SET ollama_connect_timeout_ms = 5000;
        WITH source_rows AS (
            SELECT i,
                   'chunk_state_' || CAST(i AS VARCHAR) AS state,
                   list_extract([
                       CAST({before_boundary} AS JSON), CAST({after_boundary} AS JSON)
                   ], CASE WHEN i < 2048 THEN 1 ELSE 2 END) AS config
            FROM range(0, 2050) AS input(i)
        )
        SELECT i, state, ollama_systemone(state, config) AS result
        FROM source_rows
        ORDER BY i
    """
    completed = run_systemone_sql(sql, endpoint=systemone_mock_server["url"], timeout=45)

    assert completed.returncode == 0, (
        f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}\n"
        f"mock stats: {_stats(systemone_mock_server)}"
    )
    rows = _rows(completed)
    assert len(rows) == 2050
    for row in rows:
        result = _decode_json(row["result"])
        expected_name = "first_chunk_question" if row["i"] < 2048 else "second_chunk_question"
        assert list(result["answers"]) == [expected_name]
        assert row["state"] == f"chunk_state_{row['i']}"

    server_stats = _stats(systemone_mock_server)
    assert server_stats["requests"] == 2050
    assert server_stats["states"] == {f"chunk_state_{i}": 1 for i in range(2050)}


def test_null_state_or_config_produces_no_http_request(
    run_systemone_sql: Any, systemone_mock_server: dict[str, Any]
) -> None:
    config = _sql_string(_question_config("non_null_request"))
    sql = f"""
        {_load_json_extension()}
        SELECT row_name, state, ollama_systemone(state, config) AS result
        FROM (VALUES
            ('before_null', 'request_before_null', CAST({config} AS JSON)),
            ('null_state', NULL, CAST({config} AS JSON)),
            ('null_config', 'request_null_config', CAST(NULL AS JSON)),
            ('after_null', 'request_after_null', CAST({config} AS JSON))
        ) AS input_rows(row_name, state, config)
        ORDER BY row_name
    """
    completed = run_systemone_sql(sql, endpoint=systemone_mock_server["url"])

    rows = _rows(completed)
    by_name = {row["row_name"]: row for row in rows}
    assert len(by_name) == 4
    assert _decode_json(by_name["null_state"]["result"]) is None
    assert _decode_json(by_name["null_config"]["result"]) is None
    assert list(_decode_json(by_name["before_null"]["result"])["answers"]) == ["non_null_request"]
    assert list(_decode_json(by_name["after_null"]["result"])["answers"]) == ["non_null_request"]

    server_stats = _stats(systemone_mock_server)
    assert server_stats["requests"] == 2
    assert server_stats["states"] == {"request_before_null": 1, "request_after_null": 1}


def test_delayed_response_keeps_original_row_order(
    run_systemone_sql: Any, systemone_mock_server: dict[str, Any]
) -> None:
    slow_config = _sql_string(_question_config("slow_row_question"))
    fast_config = _sql_string(_question_config("fast_row_question", "choice"))
    sql = f"""
        {_load_json_extension()}
        SELECT slot, state, ollama_systemone(state, config) AS result
        FROM (VALUES
            (1, 'delay:350:slow_row', CAST({slow_config} AS JSON)),
            (2, 'fast_row', CAST({fast_config} AS JSON))
        ) AS input_rows(slot, state, config)
    """
    completed = run_systemone_sql(sql, endpoint=systemone_mock_server["url"])

    rows = _rows(completed)
    by_slot = {row["slot"]: row for row in rows}
    assert set(by_slot) == {1, 2}
    assert by_slot[1]["state"] == "delay:350:slow_row"
    assert by_slot[2]["state"] == "fast_row"
    assert list(_decode_json(by_slot[1]["result"])["answers"]) == ["slow_row_question"]
    assert list(_decode_json(by_slot[2]["result"])["answers"]) == ["fast_row_question"]
    server_stats = _stats(systemone_mock_server)
    assert server_stats["requests"] == 2
    assert server_stats["max_active"] == 2
    assert server_stats["completed_states"] == ["fast_row", "delay:350:slow_row"]
    assert server_stats["states"] == {"delay:350:slow_row": 1, "fast_row": 1}
