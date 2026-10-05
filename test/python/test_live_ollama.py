"""Opt-in compatibility coverage against a locally running Ollama service.

Set ``SYSTEMONE_RUN_LIVE_OLLAMA=1`` to run this suite. It makes real model
requests and is intentionally excluded from ordinary CI runs.
"""

from __future__ import annotations

import json
import os
import re
from typing import Any
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import pytest


pytestmark = pytest.mark.skipif(
    os.environ.get("SYSTEMONE_RUN_LIVE_OLLAMA") != "1",
    reason="set SYSTEMONE_RUN_LIVE_OLLAMA=1 to make requests to local Ollama",
)

OLLAMA_BASE_URL = os.environ.get("SYSTEMONE_LIVE_OLLAMA_URL", "http://localhost:11434")
MODEL = "tev1:0.8b"


def _api_json(path: str) -> dict[str, Any]:
    with urlopen(f"{OLLAMA_BASE_URL.rstrip('/')}{path}", timeout=5) as response:
        payload = json.loads(response.read())
    assert isinstance(payload, dict)
    return payload


def _sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _question(name: str, kind: str) -> dict[str, Any]:
    question: dict[str, Any] = {
        "type": kind,
        "instructions": f"Classify {name} using the supplied context.",
    }
    if kind == "choice":
        question["criteria"] = {"alpha": None, "beta": None}
    elif kind == "score":
        question["criteria"] = ["low", "high"]
    return question


def _config(questions: dict[str, dict[str, Any]]) -> str:
    return json.dumps({"questions": questions}, separators=(",", ":"))


def _decode_json(value: Any) -> Any:
    # DuckDB's JSON CLI renderer can return JSON values either decoded or as text.
    return json.loads(value) if isinstance(value, str) else value


def _rows(completed: Any) -> list[dict[str, Any]]:
    assert completed.returncode == 0, (
        f"DuckDB SQL failed with exit code {completed.returncode}\n"
        f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
    )
    rows = json.loads(completed.stdout)
    assert isinstance(rows, list)
    return rows


def _assert_response(result_value: Any, questions: dict[str, dict[str, Any]]) -> dict[str, Any]:
    result = _decode_json(result_value)
    assert isinstance(result, dict)
    assert result["model"] == MODEL
    answers = result["answers"]
    assert isinstance(answers, dict)
    assert set(answers) == set(questions)
    for name, question in questions.items():
        answer = answers[name]
        kind = question["type"]
        assert isinstance(answer, dict)
        assert answer["type"] == kind
        if kind == "noul":
            assert isinstance(answer["noul"], (int, float))
            assert 0 <= answer["noul"] <= 1
        elif kind == "choice":
            assert answer["choice"] in question["criteria"]
            assert set(answer["probabilities"]) == set(question["criteria"])
            assert isinstance(answer["confidence"], (int, float))
            assert 0 <= answer["confidence"] <= 1
        elif kind == "score":
            assert 0 <= answer["score"] <= len(question["criteria"]) - 1
            expected_keys = {str(index) for index in range(len(question["criteria"]))}
            assert set(answer["probabilities"]) == expected_keys
            assert set(answer["legend"]) == expected_keys
            assert isinstance(answer["confidence"], (int, float))
            assert 0 <= answer["confidence"] <= 1
        else:  # pragma: no cover - all callers construct known question types
            raise AssertionError(f"unexpected question type {kind!r}")
    usage = result["usage"]
    assert isinstance(usage, dict)
    for key in ("input_tokens", "output_tokens"):
        assert isinstance(usage[key], int)
        assert usage[key] >= 0
    return result


def _assert_ollama_prerequisites() -> None:
    version = _api_json("/api/version").get("version", "")
    match = re.match(r"^(\d+)\.(\d+)\.(\d+)", str(version))
    assert match is not None, f"could not parse Ollama version from {version!r}"
    assert tuple(map(int, match.groups())) >= (0, 35, 0), f"Ollama >= 0.35 required, got {version}"
    tags = _api_json("/api/tags").get("models", [])
    installed = {model.get("name") for model in tags if isinstance(model, dict)}
    assert MODEL in installed, f"required local model {MODEL!r} is missing; installed models: {sorted(installed)}"


def test_live_mixed_question_response_and_usage(run_systemone_sql: Any) -> None:
    """Check mixed answer shapes and returned usage against live Ollama."""
    _assert_ollama_prerequisites()

    # Mixed question types exercise the complete response shape and server usage.
    mixed = {
        "presence": _question("presence", "noul"),
        "route": _question("route", "choice"),
        "severity": _question("severity", "score"),
    }
    completed = run_systemone_sql(
        f"""
        SET ollama_request_timeout_ms = 600000;
        SELECT ollama_systemone('A red vehicle is waiting at an intersection.', {_sql_string(_config(mixed))}) AS result
        """,
        endpoint=f"{OLLAMA_BASE_URL.rstrip('/')}/v1/systemone",
        timeout=660,
    )
    rows = _rows(completed)
    assert len(rows) == 1
    _assert_response(rows[0]["result"], mixed)


def test_live_eight_question_response(run_systemone_sql: Any) -> None:
    """Check an eight-question response against the provisioned model context."""
    _assert_ollama_prerequisites()
    questions = {
        f"q{index}": _question(f"q{index}", "noul")
        for index in range(8)
    }
    completed = run_systemone_sql(
        f"""
        SET ollama_request_timeout_ms = 600000;
        SELECT ollama_systemone('A red vehicle is waiting at an intersection.', {_sql_string(_config(questions))}) AS result
        """,
        endpoint=f"{OLLAMA_BASE_URL.rstrip('/')}/v1/systemone",
        timeout=660,
    )
    rows = _rows(completed)
    assert len(rows) == 1
    _assert_response(rows[0]["result"], questions)


def test_live_one_question_at_normalized_source_limit(run_systemone_sql: Any) -> None:
    """Check a one-question request at the full accepted 1024-byte source limit."""
    _assert_ollama_prerequisites()
    # The one-question request is also exactly the accepted normalized source
    # ceiling: state plus question name and instructions, with newline separators.
    single = {"maximum_state": _question("maximum_state", "noul")}
    normalized_suffix = "\nmaximum_state\n" + single["maximum_state"]["instructions"]
    state = "context " * ( (1024 - len(normalized_suffix)) // len("context ") )
    state += "x" * (1024 - len(state) - len(normalized_suffix))
    assert len((state + normalized_suffix).encode("utf-8")) == 1024
    completed = run_systemone_sql(
        f"""
        SET ollama_request_timeout_ms = 600000;
        SELECT ollama_systemone({_sql_string(state)}, {_sql_string(_config(single))}) AS result
        """,
        endpoint=f"{OLLAMA_BASE_URL.rstrip('/')}/v1/systemone",
        timeout=660,
    )
    rows = _rows(completed)
    assert len(rows) == 1
    _assert_response(rows[0]["result"], single)


def test_live_64_question_request(run_systemone_sql: Any) -> None:
    """Check the maximum question count and recognize only Ollama's context-limit rejection."""
    _assert_ollama_prerequisites()
    # Use the smallest valid per-question source to avoid spending the token
    # budget on descriptions; the mixed-type case is covered separately.
    maximum = {f"q{index}": {"type": "noul", "instructions": "x"} for index in range(64)}
    assert len(maximum) == 64
    completed = run_systemone_sql(
        f"""
        SET ollama_request_timeout_ms = 600000;
        SELECT ollama_systemone('x', {_sql_string(_config(maximum))}) AS result
        """,
        endpoint=f"{OLLAMA_BASE_URL.rstrip('/')}/v1/systemone",
        timeout=660,
    )
    if completed.returncode == 0:
        rows = _rows(completed)
        assert len(rows) == 1
        _assert_response(rows[0]["result"], maximum)
        return

    diagnostic = completed.stdout + completed.stderr
    assert re.search(r"ollama_systemone slot 1: SystemOne endpoint returned HTTP status 400", diagnostic), (
        f"64-question SQL failed for a reason other than the expected HTTP 400\n{diagnostic}"
    )

    # The extension reports status but intentionally does not expose Ollama's
    # error body. Repeat this exact request directly to distinguish the known
    # prompt context limit from any unrelated HTTP 400 response.
    payload = {
        "model": MODEL,
        "state": "x",
        "questions": maximum,
    }
    request = Request(
        f"{OLLAMA_BASE_URL.rstrip('/')}/v1/systemone",
        data=json.dumps(payload, separators=(",", ":")).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    try:
        with urlopen(request, timeout=660) as response:
            pytest.fail(
                "Ollama accepted the direct 64-question request, but the extension returned HTTP 400: "
                f"{response.status} {response.read().decode('utf-8', errors='replace')}"
            )
    except HTTPError as error:
        error_body = error.read().decode("utf-8", errors="replace")
        assert error.code == 400, f"direct 64-question request returned HTTP {error.code}: {error_body}"
        assert re.search(r"prompt \d+ has \d+ tokens; expected \d+[–-]\d+", error_body), (
            f"Ollama's HTTP 400 was not a prompt/context-limit rejection: {error_body}"
        )
