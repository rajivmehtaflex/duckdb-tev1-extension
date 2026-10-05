#!/usr/bin/env python3
"""Run the standalone transport benchmark against the repository mock server."""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any
from urllib.request import urlopen
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
MOCK_SERVER = ROOT / "test" / "mock" / "server.py"
DEFAULT_DRIVER = ROOT / "build" / "release" / "extension" / "ollama_systemone" / "systemone_transport_benchmark"
OLLAMA_STOP_TIMEOUT_SECONDS = 10


def _percentile(values: list[float], p: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * p
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def _sample_peak_rss(process: subprocess.Popen[str], stop: threading.Event, peak: list[int]) -> None:
    status = Path(f"/proc/{process.pid}/status")
    while not stop.is_set() and process.poll() is None and status.exists():
        try:
            for line in status.read_text(encoding="ascii").splitlines():
                if line.startswith("VmRSS:"):
                    peak[0] = max(peak[0], int(line.split()[1]) * 1024)
                    break
        except (OSError, ValueError, IndexError):
            pass
        stop.wait(0.01)


def _kill_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is None:
        try:
            process.kill()
        except ProcessLookupError:
            pass


def _host_metadata() -> dict[str, Any]:
    memory_bytes = None
    try:
        for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
            if line.startswith("MemTotal:"):
                memory_bytes = int(line.split()[1]) * 1024
                break
    except (OSError, ValueError, IndexError):
        pass
    return {
        "runtime_host": {
            "cpu_count": os.cpu_count(),
            "memory_bytes": memory_bytes,
            "platform": platform.platform(),
            "python": sys.version.split()[0],
        },
    }


def _stats(url: str) -> dict[str, Any]:
    with urlopen(url, timeout=3) as response:
        return json.load(response)


def _ensure_driver(driver: Path, build: bool) -> None:
    if driver.is_file():
        return
    if not build:
        raise FileNotFoundError(f"benchmark driver not found: {driver}; pass --build or build the target")
    build_dir = ROOT / "build" / "release"
    subprocess.run(["cmake", "--build", str(build_dir), "--target", "systemone_transport_benchmark", "-j2"],
                   check=True, cwd=ROOT)
    if not driver.is_file():
        raise FileNotFoundError(f"build completed but driver was not created: {driver}")


def _run_driver(driver: Path, endpoint: str, rows: int, questions: int, max_inflight: int,
                queue_capacity: int, timeout: int) -> tuple[dict[str, Any], int, float]:
    command = [str(driver), endpoint, str(rows), str(questions), str(max_inflight), str(queue_capacity)]
    started = time.monotonic()
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    peak_rss = [0]
    stop_sampling = threading.Event()
    sampler = threading.Thread(target=_sample_peak_rss, args=(process, stop_sampling, peak_rss), daemon=True)
    try:
        sampler.start()
        stdout, stderr = process.communicate(timeout=timeout)
    except BaseException:
        _kill_process(process)
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            _kill_process(process)
            process.wait()
        raise
    finally:
        stop_sampling.set()
        if sampler.is_alive():
            sampler.join(timeout=1)
    elapsed_wall_ms = (time.monotonic() - started) * 1000
    if process.returncode != 0:
        raise RuntimeError(f"benchmark driver failed ({process.returncode}): {stderr.strip()}")
    return json.loads(stdout), peak_rss[0], elapsed_wall_ms


def _ollama_models(endpoint_url: str) -> list[dict[str, Any]]:
    endpoint = urlsplit(endpoint_url)
    return _stats(f"{endpoint.scheme}://{endpoint.netloc}/api/ps").get("models", [])


def _prepare_cold_ollama(endpoint_url: str) -> dict[str, Any]:
    endpoint = urlsplit(endpoint_url)
    if endpoint.scheme not in ("http", "https") or endpoint.hostname not in ("localhost", "127.0.0.1", "::1"):
        raise ValueError("--cold-ollama requires an endpoint hosted on localhost, 127.0.0.1, or ::1")
    ollama = shutil.which("ollama")
    if not ollama:
        raise FileNotFoundError("--cold-ollama requires the ollama CLI on PATH")
    try:
        stopped = subprocess.run(
            [ollama, "stop", "tev1:0.8b"], capture_output=True, text=True, check=False,
            timeout=OLLAMA_STOP_TIMEOUT_SECONDS
        )
    except subprocess.TimeoutExpired as error:
        raise TimeoutError(
            f"ollama stop tev1:0.8b exceeded {OLLAMA_STOP_TIMEOUT_SECONDS} seconds"
        ) from error
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        models = _ollama_models(endpoint_url)
        resident = [model for model in models if model.get("name") == "tev1:0.8b" or model.get("model") == "tev1:0.8b"]
        if not resident:
            return {
                "command": [ollama, "stop", "tev1:0.8b"],
                "command_returncode": stopped.returncode,
                "command_stderr": stopped.stderr.strip(),
                "verified_absent_from_api_ps": True,
            }
        time.sleep(0.25)
    raise TimeoutError("tev1:0.8b remained present in Ollama /api/ps after ollama stop")


def _first_request(row: dict[str, Any]) -> dict[str, Any]:
    return {
        "row": row["row"],
        "completion_elapsed_ms": row["completion_elapsed_ms"],
        "request_duration_ms": row["request_duration_ms"],
        "queue_wait_ms": row["queue_wait_ms"],
        "response_bytes": row["response_bytes"],
    }


def _summarize_warm(rows: list[dict[str, Any]], elapsed_ms: float | None) -> dict[str, Any]:
    request_ms = [float(row["request_duration_ms"]) for row in rows]
    queue_ms = [float(row["queue_wait_ms"]) for row in rows]
    latency_ms = [float(row["queue_wait_ms"]) + float(row["request_duration_ms"]) for row in rows]
    throughput = len(rows) * 1000.0 / elapsed_ms if elapsed_ms is not None and elapsed_ms > 0 else None
    return {
        "count": len(rows),
        "request_duration_ms": {"p50": _percentile(request_ms, 0.50), "p95": _percentile(request_ms, 0.95),
                                "p99": _percentile(request_ms, 0.99)},
        "queue_wait_ms": {"p50": _percentile(queue_ms, 0.50), "p95": _percentile(queue_ms, 0.95),
                          "p99": _percentile(queue_ms, 0.99)},
        "latency_ms_including_queue_wait": {"p50": _percentile(latency_ms, 0.50),
                                            "p95": _percentile(latency_ms, 0.95),
                                            "p99": _percentile(latency_ms, 0.99)},
        "elapsed_ms": elapsed_ms,
        "elapsed_throughput_rows_per_second": throughput,
    }


def run(args: argparse.Namespace) -> dict[str, Any]:
    driver = args.driver.resolve()
    _ensure_driver(driver, args.build)
    cold_ollama = None
    with tempfile.TemporaryDirectory(prefix="systemone-benchmark-") as temp_dir:
        server: subprocess.Popen[str] | None = None
        server_stats = None
        try:
            if args.endpoint:
                endpoint_url = args.endpoint
            else:
                ready_file = Path(temp_dir) / "ready.json"
                server = subprocess.Popen(
                    [sys.executable, str(MOCK_SERVER), "--host", "127.0.0.1", "--port", "0", "--ready-file", str(ready_file)],
                    cwd=ROOT,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                deadline = time.monotonic() + 10
                ready = None
                while time.monotonic() < deadline and ready is None:
                    if server.poll() is not None:
                        raise RuntimeError(f"mock server exited early: {server.stderr.read() if server.stderr else ''}")
                    if ready_file.exists():
                        try:
                            ready = json.loads(ready_file.read_text(encoding="utf-8"))
                        except (OSError, json.JSONDecodeError):
                            pass
                    if ready is None:
                        time.sleep(0.02)
                if ready is None:
                    raise TimeoutError("mock server did not become ready")
                endpoint_url = ready["url"]
            endpoint = urlsplit(endpoint_url)
            if args.cold_ollama:
                cold_ollama = _prepare_cold_ollama(endpoint_url)

            cold_probe = None
            if cold_ollama is not None:
                cold_output, cold_rss, cold_wall_ms = _run_driver(
                    driver, endpoint_url, 1, args.questions, args.max_inflight, args.queue_capacity, args.timeout
                )
                probe_row = cold_output["rows"][0]
                cold_probe = {
                    **_first_request(probe_row),
                    "elapsed_ms": cold_output["elapsed_ms"],
                    "elapsed_wall_ms_including_process_startup": cold_wall_ms,
                    "client_dispatcher_peak_rss_bytes_sampled": cold_rss,
                    "model_was_absent_from_api_ps_before_request": cold_ollama["verified_absent_from_api_ps"],
                }

            warmup_output = None
            warmup_rss = 0
            if args.warmup_rows:
                warmup_output, warmup_rss, _ = _run_driver(
                    driver, endpoint_url, args.warmup_rows, args.questions,
                    args.max_inflight, args.queue_capacity, args.timeout
                )

            # Exclude cold probe and explicit warmup requests from mock connection statistics.
            if server is not None and (cold_probe is not None or warmup_output is not None):
                _stats(f"{endpoint.scheme}://{endpoint.netloc}/__reset")

            output, peak_rss_bytes, elapsed_wall_ms = _run_driver(
                driver, endpoint_url, args.rows, args.questions,
                args.max_inflight, args.queue_capacity, args.timeout
            )
            if server is not None:
                endpoint = urlsplit(endpoint_url)
                server_stats = _stats(f"{endpoint.scheme}://{endpoint.netloc}/__stats")
        finally:
            if server is not None:
                server.terminate()
                try:
                    server.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()

    rows = output["rows"]
    first = min(rows, key=lambda row: row["completion_order"])
    if args.warmup_rows:
        warm_rows = rows
        warm_elapsed_ms = float(output["elapsed_ms"])
        warm_method = f"measured after a separate {args.warmup_rows}-row warmup run"
    elif args.max_inflight == 1:
        warm_rows = [row for row in rows if row["completion_order"] != first["completion_order"]]
        warm_end_ms = max((float(row["completion_elapsed_ms"]) for row in warm_rows), default=None)
        warm_elapsed_ms = (warm_end_ms - float(first["completion_elapsed_ms"])) if warm_end_ms is not None else None
        warm_method = "sequential rows after the separately reported first request"
    else:
        warm_rows = []
        warm_elapsed_ms = None
        warm_method = "not reported: concurrent first wave is not isolated; use --warmup-rows or --max-inflight 1"
    return {
        "host": _host_metadata(),
        "configuration": output["settings"],
        "request_limits": output["request_limits"],
        "first_request_in_measured_run": _first_request(first),
        "cold_ollama_prepare": cold_ollama,
        "cold_ollama_probe": cold_probe,
        "warmup_run": ({"rows": args.warmup_rows, "completed": warmup_output["transport_stats"]["completed"],
                        "client_dispatcher_peak_rss_bytes_sampled": warmup_rss}
                       if warmup_output is not None else None),
        "warmed_rows": {**_summarize_warm(warm_rows, warm_elapsed_ms), "method": warm_method},
        "all_rows": rows,
        "failed_rows": sum(1 for row in rows if not row["success"]),
        "elapsed_ms": output["elapsed_ms"],
        "elapsed_wall_ms_including_process_startup": elapsed_wall_ms,
        "throughput_rows_per_second": output["throughput_rows_per_second"],
        "client_dispatcher_peak_rss_bytes_sampled": peak_rss_bytes,
        "peak_rss_measurement": "benchmark driver process only; maximum VmRSS observed while polling /proc/<pid>/status every 10 ms, so short transient peaks can be missed; excludes Ollama and mock server",
        "transport_stats": output["transport_stats"],
        "connection_reuse": ({
            "measurement": "mock server counts unique remote IP:port TCP peers; repeat requests from the same peer indicate keep-alive socket reuse",
            "requests": server_stats["requests"],
            "unique_connections": server_stats["connection_count"],
            "estimated_reused_requests": max(0, server_stats["requests"] - server_stats["connection_count"]),
            "connection_headers": server_stats["connection_headers"],
            "limitation": "This is an HTTP/1.1 mock-server socket estimate, not a measurement of Ollama/model-server reuse or TLS connection behavior.",
        } if server_stats is not None else {
            "measurement": None,
            "requests": None,
            "unique_connections": None,
            "estimated_reused_requests": None,
            "connection_headers": None,
            "limitation": "The external endpoint does not expose mock socket stats; connection reuse was not measured.",
        }),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, default=DEFAULT_DRIVER)
    parser.add_argument("--build", action="store_true", help="build the CMake benchmark target if missing")
    parser.add_argument("--endpoint", help="benchmark an already-running SystemOne endpoint; defaults to the local mock")
    parser.add_argument("--cold-ollama", action="store_true",
                        help="for a local Ollama endpoint, stop tev1:0.8b and verify /api/ps before a one-row cold probe")
    parser.add_argument("--rows", type=int, default=128)
    parser.add_argument("--warmup-rows", type=int, default=0,
                        help="run a separate warmup batch before measured rows; enables warm percentiles at any concurrency")
    parser.add_argument("--questions", type=int, choices=(1, 8, 64), default=1)
    parser.add_argument("--max-inflight", type=int, default=8)
    parser.add_argument("--queue-capacity", type=int, default=16)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    if args.rows < 1 or args.max_inflight < 1 or args.queue_capacity < 0 or args.warmup_rows < 0:
        parser.error("rows and max-inflight must be positive; queue-capacity must be non-negative")
    if args.cold_ollama and not args.endpoint:
        parser.error("--cold-ollama requires --endpoint pointing to a local Ollama-backed SystemOne endpoint")
    print(json.dumps(run(args), indent=2))


if __name__ == "__main__":
    main()
