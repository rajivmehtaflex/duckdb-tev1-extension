# Repository Guidelines

## Project Structure

This is a DuckDB C++ extension targeting DuckDB v1.5.6. Runtime code is in `src/`: extension loading and registration are at the root, and SystemOne configuration, request validation, response validation, transport, and scalar execution are in `src/systemone/`. SQLLogicTests are in `test/sql/`; native C++ tests are in `test/cpp/`; Python integration tests are in `test/python/`; the mock Ollama server and fixtures are in `test/mock/`. User and release documentation is in `docs/`. `duckdb/` and `extension-ci-tools/` are pinned submodules/tooling; avoid changing them for extension work.

## Build and Test

- `make` configures and builds the Release CLI and loadable extension using the pinned submodules and vcpkg manifest.
- `make test` runs the SQLLogicTests.
- `cmake --build build/release --target systemone_test_request systemone_test_response systemone_transport_test_driver` builds native contract tests and the transport probe.
- `ctest --test-dir build/release/extension/ollama_systemone --output-on-failure` runs the native C++ tests.
- `python -m pytest -q test/python/test_transport.py test/python/test_vectorized.py test/python/test_concurrency.py` runs deterministic HTTP, vectorization, and concurrency tests.
- Live tests require Ollama v0.35+ and `tev1:0.8b`: `SYSTEMONE_RUN_LIVE_OLLAMA=1 python -m pytest -q test/python/test_live_ollama.py`.

For local use, start Ollama with the model installed, launch `./build/release/duckdb -unsigned`, then run `LOAD json;` and load `build/release/extension/ollama_systemone/ollama_systemone.duckdb_extension` by path.

## Style and Tests

Follow DuckDB C++ conventions: C++17, tabs for indentation, `PascalCase` types/functions, and uppercase constants. Use four spaces in Python and `snake_case` names. Keep C++ unit tests named `test_*.cpp`, Python tests `test_*.py`, and deterministic fixtures under `test/mock/fixtures/`. The distribution workflow runs DuckDB format and tidy checks; keep changes compatible with those checks.

## Commits and Pull Requests

Use the repository’s Conventional Commit style, for example `feat: add response validation` or `fix: cancel queued requests`. PRs should describe behavior and scope, list verification commands and results, link a relevant issue when one exists, and distinguish local evidence from CI evidence. No screenshots are needed for this SQL/runtime project. Do not mark T7.2 or T7.6 complete without actual project GitHub Actions evidence; the configured platform matrix alone is not proof of execution.

## Configuration and Agent Notes

The SQL function sends row text and question config to `ollama_endpoint` (default `http://localhost:11434/v1/systemone`); disclose this when using a remote endpoint and do not log request contents. Enable unsigned-extension loading only for binaries you trust. When `.codegraph/` exists, use CodeGraph before grep/find or source reads; do not initialize an index when it is absent. Community Extensions publication is currently deferred.
