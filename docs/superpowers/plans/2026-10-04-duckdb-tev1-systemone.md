# DuckDB Tev1 SystemOne Extension Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a loadable C++ DuckDB extension that evaluates one row's complete set of Tev1 SystemOne questions through Ollama and returns the validated response as DuckDB `JSON`.

**Architecture:** Register a volatile, fallible scalar function and session settings through the DuckDB extension lifecycle. Parse and validate ordered JSON, submit owned request objects to a bounded per-expression libcurl multi dispatcher, validate complete responses, then map them back to DuckDB chunk slots. Keep socket I/O off DuckDB workers while recognizing that the scalar call must wait for its result.

**Tech Stack:** DuckDB C++ extension template pinned to DuckDB v1.5.6; C++; `nlohmann::ordered_json` with SAX duplicate-key detection; libcurl multi; vcpkg-pinned dependencies; native C++, SQL, Python, and mock HTTP tests.

**Spec:** `duckdb-tev1-extension-implementation-plan.md`; end-user installation and usage requirements in `duckdb-tev1-extension-end-to-end.md`.

## Global Constraints

- Target DuckDB v1.5.6 and an extension-template commit whose DuckDB submodule is v1.5.6; another DuckDB version needs an explicit compatibility build and test.
- Public SQL contract: `ollama_systemone(state VARCHAR, config JSON) -> JSON`; register as volatile and fallible, with DuckDB's default NULL handling and no HTTP request for NULL inputs.
- Require nonempty text state and a config object with exactly one supported top-level key, `questions`; accept 1–64 named questions and only `noul`, `choice`, and `score` types.
- Reject unknown/duplicate keys, invalid or empty names/instructions, unsupported types, and invalid criteria before sending a request; preserve question and option order.
- Always send model `tev1:0.8b`; default endpoint is `http://localhost:11434/v1/systemone`; allow only `http` or `https`, disable redirects, and omit `keep_alive` unless configured.
- Return the complete Ollama response as DuckDB JSON, preserving `model`, `answers`, `usage`, answer fields, numeric values, and permitted additional response fields.
- Use the v1 defaults from the implementation plan: connect timeout 1000 ms; request timeout 120000 ms; max in-flight 8; queue capacity 16; request body 65536 bytes; response body 1048576 bytes; normalized source 1024 bytes; retry count 1; keep-alive unset.
- Enforce request-body and normalized-source byte limits independently. Never truncate input. The 1024-byte source guard is not an exact tokenizer guarantee; exact prompt token preflight is outside v1.
- Use two dedicated I/O event-loop threads per bound function expression. Bound active and queued work, response buffering, and wait time. A DuckDB worker can be parked while the scalar function awaits results, but must not perform socket reads or wait while holding a shared mutex.
- Never retain DuckDB `DataChunk`, `Vector`, `string_t`, or result-vector pointers beyond the scalar callback. Network callbacks never write DuckDB output vectors.
- Errors identify the function, one-based slot in the current input `DataChunk`, and concise cause; do not include state, instructions, criteria, response bodies, or secrets in errors or logs.
- Ollama 0.35 or later and local model `tev1:0.8b` are required for the documented live compatibility path.

## Review Focus

- **Selection vectors, NULLs, and chunk boundaries can mis-map results.** Task 6 pins constant/varying config, dictionary vectors, filtered selections, NULLs, multiple chunks, and delayed out-of-order responses to original chunk slots.
- **Duplicate keys or order changes can alter the request and tie behavior.** Task 3 tests duplicate detection, ordered questions/options, Unicode, and escaping before serialization.
- **Malformed responses can return plausible but invalid answers.** Task 4 tests missing answers/fields, wrong answer types, invalid labels, non-finite or out-of-range numbers, and permitted extra fields.
- **Queue saturation, cancellation, and teardown can leak work or outlive a query.** Tasks 5–6 test active/queued bounds, query interruption, cleanup, and concurrent expressions.
- **Byte bounds do not prove a hard token limit.** Tasks 3 and 7 test byte-limit boundaries and the largest accepted inputs against the pinned live Ollama/model release, recording usage where available without claiming exact tokenizer compliance.

---

## Execution Dependency Diagram

```text
                         PRIORITY / EXECUTION FLOW

P0  [T1 Pin template, build scaffold, dependencies]
                          |
                          v
    [T2 Extension lifecycle, settings, SQL signature]
                          |
             +------------+------------+
             |                         |
             v                         v
P1  [T3 Request parsing,          [T4 Response parsing,
     validation, serialization]   validation, JSON result]
             |                         |
             +------------+------------+
                          v
                 [T5 Bounded dispatcher]
                          |
                          v
                 [T6 Vectorized SQL callback]
                          |
                          v
P2  [T7 Live compatibility, CI/platform artifacts,
     install/use docs, release checks and benchmarks]

Independent work within a task may proceed together. T2 defines the shared
question metadata contract; after that, T3 and T4 can be implemented in
parallel against it. T5 needs both implementations. Platform CI matrix setup
can begin with T1, while T7's release acceptance waits for T6.
```

## Priority and Dependency Summary

| Task | Priority | Depends on | Deliverable |
|---|---|---|---|
| T1. Pin scaffold and build | P0 | — | Reproducible DuckDB v1.5.6 extension build and load |
| T2. Define types and registration | P0 | T1 | Extension hooks, settings, SQL function signature and NULL contract |
| T3. Request validation and serialization | P1 | T2 | Ordered validated request representation and byte guards |
| T4. Response parsing and validation | P1 | T2 | Validated complete JSON response representation |
| T5. Bounded HTTP dispatcher | P1 | T2, T3, T4 | Per-expression async transport, limits, retry, cancellation, teardown |
| T6. Vectorized callback integration | P1 | T2, T3, T4, T5 | Correct chunk-slot results and SQL behavior under DuckDB vector execution |
| T7. Integration and packaging | P2 | T1–T6 for final acceptance | Live compatibility, docs, platform artifacts, release evidence |

Priorities indicate implementation order and release gating: P0 establishes the compatible build and public contract; P1 completes the runtime; P2 proves the supported deployment and release path. Every task is required for the stated release acceptance.

## Task 1: Pin Scaffold and Build

**Files:**
- Create/update: `CMakeLists.txt`, `Makefile`, `extension_config.cmake`, `vcpkg.json`
- Create/update: extension-template source layout and pinned DuckDB submodule reference
- Build outputs: debug and release extension artifacts for DuckDB v1.5.6

**Interfaces:**
- Consumes: the official DuckDB C++ extension template.
- Produces: a loadable extension target named `ollama_systemone`, pinned build baseline, and standard build/test entry points for later tasks.

- [x] **Subtask 1.1 — Pin the template and DuckDB version.** Select an extension-template commit whose DuckDB submodule is v1.5.6; record both pins in repository metadata.
- [x] **Subtask 1.2 — Add extension metadata and build entry points.** Configure the extension name, source list, version metadata, CMake build, Makefile targets, and pinned libcurl/ordered-JSON dependencies.
- [x] **Subtask 1.3 — Prove debug and release loading.** Build both configurations against the pinned DuckDB and load each artifact into the matching DuckDB build. Expected result: extension loads and reports its registered name/version.
- [x] **Subtask 1.4 — Commit the scaffold.** Keep the template pin, dependency pins, and build metadata in one reviewable checkpoint.

**Acceptance:** Debug and release artifacts compile and load against DuckDB v1.5.6. The extension-template commands and resulting artifact paths are recorded for CI and later verification.

## Task 2: Define Types and Registration

**Files:**
- Modify: `src/include/tev1_extension.hpp` (T1 declaration)
- Modify: `src/tev1_extension.cpp` (T1 lifecycle stub)
- Create: `src/registration.cpp`
- Create: `src/systemone/config.hpp`, `src/systemone/config.cpp`
- Create: `src/systemone/types.hpp`
- Create: `test/sql/ollama_systemone_registration.test`
- Modify: `CMakeLists.txt` to discover and register isolated native unit-test executables from `test/cpp/test_*.cpp`

**Interfaces:**
- Consumes: T1's pinned extension loader/build conventions.
- Produces: `ollama_systemone(state VARCHAR, config JSON) -> JSON`; settings and a consistent invocation settings snapshot for T3/T5; stable shared question metadata types for T3/T4.

- [x] **Subtask 2.1 — Wire the lifecycle hooks.** Connect the existing T1 `Load`, `Name`, `Version`, and C++ entry points to settings and scalar registration through the pinned `ExtensionLoader` API.
- [x] **Subtask 2.2 — Register session settings and defaults.** Use `DBConfig::AddExtensionOption` with `SetScope::SESSION` as the default scope for endpoint, connect/request timeouts, in-flight, queue, request/response/source-byte limits, retry, and optional keep-alive settings. Apply the exact v1 defaults in Global Constraints, validate endpoint scheme (`http` or `https`), and expose a helper that reads one immutable settings snapshot for execution state.
- [x] **Subtask 2.3 — Define shared types and test infrastructure.** In `src/systemone/types.hpp`, define `enum class QuestionType`, `QuestionSpec {std::string name, QuestionType type, std::string instructions, std::optional<nlohmann::ordered_json> criteria}`, and `ValidatedQuestions {std::vector<QuestionSpec> ordered}`. Preserve criteria order; use `std::nullopt` when `noul` criteria are omitted. Define the response-validator boundary as `nlohmann::ordered_json ValidateResponse(std::string_view response_json, const ValidatedQuestions &questions)`. T3 produces `ValidatedQuestions`; T4 tests and implements against this type without depending on T3's parser implementation. In `CMakeLists.txt`, add one test executable per discovered `test/cpp/test_*.cpp`, compiling each test with its corresponding pure C++ module source and registering it with CTest; use `CONFIGURE_DEPENDS` for both test and module source globs so T3/T4 can add disjoint files without editing shared CMake files.
- [x] **Subtask 2.4 — Register the SQL contract.** Return `LogicalType::JSON()`, mark the scalar function volatile and fallible, and retain DuckDB default NULL propagation.
- [x] **Subtask 2.5 — Verify catalog and NULL behavior.** Registration tests inspect argument/return types and setting defaults, and confirm DuckDB's default NULL propagation. T6 verifies that NULL inputs cause zero mock HTTP requests after transport is integrated.
- [x] **Subtask 2.6 — Commit the registered contract and shared interfaces.**

**Acceptance:** Extension loads, exposes the exact SQL signature and settings, and does not submit requests for NULL input rows.

## Task 3: Implement Deterministic Request Validation and Serialization

**Files:**
- Create: `src/systemone/request.hpp`, `src/systemone/request.cpp`
- Create: `test/cpp/test_request.cpp`
- Create: deterministic request fixtures under `test/mock/fixtures/request/`

**Interfaces:**
- Consumes: T2's settings snapshot and SQL input types.
- Produces: an owned ordered request object containing serialized bytes, normalized source/question text, and validated question metadata for T5/T6.

- [x] **Subtask 3.1 — Parse ordered JSON with duplicate-key detection.** Use a SAX validation layer before constructing `nlohmann::ordered_json`; reject duplicate keys rather than accepting parser replacement behavior.
- [x] **Subtask 3.2 — Validate the config schema.** Require exactly the `questions` top-level key and 1–64 nonempty question names. Validate nonempty string instructions, allowed question types, allowed per-question keys, and criteria: `noul` omitted or a true/false-to-string object; `choice` an ordered object of 2–26 nonempty option keys whose values are strings or JSON null; `score` an ordered array of 2–26 strings.
- [x] **Subtask 3.3 — Preserve ordering and build the fixed request.** Preserve question and option order; always serialize model `tev1:0.8b`, state, and the complete questions object. Do not accept a per-call model override. Add `keep_alive` only when set.
- [x] **Subtask 3.4 — Enforce independent limits before enqueueing.** Reject normalized state, questions, and requested-field text over `ollama_max_source_bytes`; separately reject escaped serialized request bodies over `ollama_max_request_bytes`. Never truncate.
- [x] **Subtask 3.5 — Verify boundary fixtures.** In `test/cpp/test_request.cpp`, cover question counts 0/1/64/65; choice/score counts 1/2/26/27; invalid `noul` criteria; duplicate and empty names; missing/extra fields; unsupported types; non-string instructions; empty state; malformed JSON; Unicode/escaping; ordering; and both size limits. Run `cmake --build build/release --target systemone_test_request` followed by `ctest --test-dir build/release/extension/ollama_systemone -R systemone_test_request --output-on-failure`; expected result: all request-contract cases pass without an Ollama server.
- [x] **Subtask 3.6 — Commit request behavior.**

**Acceptance:** Request tests pass without a server; accepted shapes preserve order and serialize correctly, and all specified invalid shapes/limits fail before dispatcher admission.

## Task 4: Implement Response Parsing and Validation

**Files:**
- Create: `src/systemone/response.hpp`, `src/systemone/response.cpp`
- Create: response fixtures under `test/mock/fixtures/response/`
- Create: `test/cpp/test_response.cpp`

**Interfaces:**
- Consumes: T2's shared `ValidatedQuestions` contract; integrated calls receive the value produced by T3.
- Produces: a validated complete response serialized as JSON for T6; preserves unknown permitted response fields.

- [x] **Subtask 4.1 — Validate the response envelope.** Require an object with required `model`, `answers`, and `usage` fields as specified by the Ollama contract; retain the complete response rather than rebuilding only answer fields.
- [x] **Subtask 4.2 — Validate one expected answer per configured question.** Require `noul` answers to contain `type` and `noul` (a probability in [0,1]); require `choice` answers to contain `type`, a configured choice label, probabilities for exactly the configured option keys with each value in [0,1], and confidence in [0,1]; require `score` answers to contain `type`, score in `[0, criterion_count - 1]`, probabilities for exactly the zero-based criterion keys with each value in [0,1], a legend for those keys, and confidence in [0,1]. Require top-level `model`, `answers`, and `usage` with nonnegative integer `input_tokens`/`output_tokens`. Permit unrelated additional response fields and preserve numeric values.
- [x] **Subtask 4.3 — Verify fixtures.** In `test/cpp/test_response.cpp`, verify mixed-type fixtures preserve `model`, `answers`, `usage`, and extra fields; malformed JSON, missing/extra required answer data, wrong types, unexpected labels, non-finite/out-of-range values, and missing probability/legend fields fail with concise categories. Run `cmake --build build/release --target systemone_test_response` followed by `ctest --test-dir build/release/extension/ollama_systemone -R systemone_test_response --output-on-failure`; expected result: all response-contract cases pass without an Ollama server.
- [x] **Subtask 4.4 — Commit response behavior.**

**Acceptance:** Complete valid responses round-trip as DuckDB-compatible JSON; invalid envelopes and answer values are rejected while forward-compatible additional fields remain intact.

## Task 5: Implement the Bounded HTTP Dispatcher

**Files:**
- Create: `src/systemone/transport.hpp`, `src/systemone/transport.cpp`
- Create: `test/cpp/transport_test_driver.cpp` as a direct C++ dispatcher test probe
- Modify: `CMakeLists.txt` to build the transport probe against the pure dispatcher and pinned dependencies
- Create: deterministic mock server at `test/mock/server.py`
- Create: `test/python/test_transport.py`

**Interfaces:**
- Consumes: T2's immutable settings snapshot; T3's owned request bytes; T4's response validator.
- Produces: completion results tagged with original chunk slots; a `FunctionData`-owned dispatcher shared by workers for one bound expression.

- [x] **Subtask 5.1 — Create expression-owned event loops.** Use two dedicated I/O event-loop threads per bound expression, each owning a libcurl multi handle and reusable connection cache. Keep bind data immutable across workers and implement `FunctionData::Copy()`/`Equals()` so plan/expression copies share the expression-scoped dispatcher safely. Ensure the dispatcher destructor cancels pending work and joins threads. **Execution note:** T6's Release build, prepared-plan settings-refresh test, independent concurrent-query test, and cancellation cleanup test verify the expression identity and query-scoped dispatcher attachment.
- [x] **Subtask 5.2 — Enforce admission and transport bounds.** Bound `ollama_max_inflight` and `ollama_queue_capacity` across workers for the expression. Use owned request/response buffers and bounded response callbacks. Set JSON content type, keep-alive, no redirects, connect/total deadlines, and no body-content logging.
- [x] **Subtask 5.3 — Implement retry and cancellation rules.** Retry at most once only when libcurl confirms the request body was not sent. Do not retry after upload starts, on HTTP status errors, malformed response, or timeout. On interruption, stop admission, abort queued/active requests, and free all owned buffers. Keep missing model, unsupported Ollama version/route, refused connection, and timeout as distinguishable diagnostic categories.
- [x] **Subtask 5.4 — Add bounded observability.** Track only counters/timings: queued, active, completed, queue wait, request duration, timeout/retry/error counts, and response bytes. Keep diagnostics opt-in or expose through extension metrics.
- [x] **Subtask 5.5 — Verify with the mock server.** The Python test starts `test/mock/server.py` and runs `transport_test_driver.cpp` against its URL, so transport is testable before T6 exposes the SQL callback. Assert max active transfers, queue capacity/backpressure, connection reuse, slot-tagged out-of-order completions, retry eligibility, timeout/response-size behavior, distinguishable failure categories, cancellation, and no leaked threads/handles. Built with `/tmp/tev1-ninja/bin/ninja -C build/release -j1 systemone_transport_test_driver`; `/usr/local/py-utils/venvs/pytest/bin/python -m pytest -q test/python/test_transport.py` passes 16/16 without Ollama. The final stale-connection regression also fails against the pre-fix transport as expected. All 16 cases pass under an isolated AddressSanitizer driver. **Follow-up verification (2026-10-05):** event-loop reservations now prefer the less-loaded loop and alternate ties, preventing an idle loop from being bypassed when work completes out of order; the connection-reuse test passed 20 consecutive isolated runs.
- [x] **Subtask 5.6 — Commit dispatcher behavior.** Commits: `37636d3` initial dispatcher, `7f48678` at-most-once replay fix and strengthened transport coverage. The scoped fix review is clean.

**Acceptance:** Mock-server transport checks demonstrate per-expression limits, bounded buffering/wait, allowed retry behavior, cancellation, and clean teardown. DuckDB workers perform no synchronous socket reads and hold no shared dispatcher mutex while waiting.

## Task 6: Connect the Vectorized Scalar Callback

**Files:**
- Create: `src/systemone/scalar_function.cpp`
- Modify: `src/registration.cpp` to bind the scalar callback and its `FunctionData`
- Create/update: `test/sql/ollama_systemone.test`
- Create/update: `test/python/test_vectorized.py`, `test/python/test_concurrency.py`

**Interfaces:**
- Consumes: T2 registration/settings; T3 request builder; T4 response validator; T5 dispatcher.
- Produces: the complete SQL behavior with correct row-to-answer mapping, DuckDB-managed JSON output storage, and slot-based errors.

- [x] **Subtask 6.1 — Decode actual DuckDB chunks correctly.** Use each vector's selection index and validity mask; do not assume 2,048 rows. Preserve chunk slot indexes separately from completion order. Implement immutable expression identity in `FunctionData` with correct `Copy()`/`Equals()` behavior, and attach each query's dispatcher through query/expression-scoped client-context state rather than persistent bind data.
- [x] **Subtask 6.2 — Reuse config parsing within each chunk.** Parse constant config once for that chunk/local function state; for varying config, parse each distinct row value once and reuse repeats within that chunk. Do not infer constancy from equal values across unrelated chunks.
- [x] **Subtask 6.3 — Submit bounded owned work.** For each non-NULL row, validate/build before admission and submit owned strings/bytes tagged with the input slot. Submit at most the available in-flight window, collect completions, then submit more; do not enqueue a whole large chunk's payloads at once.
- [x] **Subtask 6.4 — Handle failures and interruption by slot.** If any request fails, stop submitting more rows from that chunk; let already submitted lower slots finish, cancel submitted higher slots, drain cleanup, then report the lowest failing slot in that submitted window. Poll the DuckDB client context for query interruption during long submission/wait loops; on interruption, cancel all queued and active work and throw DuckDB's interruption exception. **Follow-up fix (2026-10-05):** a per-chunk `RequestGroup` records the lowest failed slot inside the dispatcher so higher queued/inbox work is cancelled before pending work can be promoted; the transport regression verifies queued work never reaches the mock, and the SQL regression verifies the one-based failing slot.
- [x] **Subtask 6.5 — Write output only after validation.** Validate successful responses before writing results; write with DuckDB-managed string storage and set validity correctly. Network callbacks must never touch output vectors.
- [x] **Subtask 6.6 — Verify end-to-end SQL/vector behavior.** Keep `test/sql/ollama_systemone.test` deterministic without a running server by checking registration, NULL propagation, and SQL error contracts. In `test/python/test_vectorized.py` and `test/python/test_concurrency.py`, start the mock server and execute SQL statements to check JSON extraction, multiple chunks, constant/varying configs, dictionary vectors, filtered selections, delayed out-of-order responses, exact slot mapping, concurrent queries, one mock request per evaluated non-NULL row, and cancellation/lifecycle cleanup. **Verified in Release (2026-10-05):** `make test` passed (16 assertions, 2 SQL test cases); native CTest passed 2/2; focused benchmark/transport/vector/concurrency pytest passed 30/30; live Ollama passed 4/4. The rendezvous-based out-of-order overlap test passed 15 consecutive isolated runs, and the keep-alive test passed 20 consecutive isolated runs after event-loop load balancing. The dictionary case passes nonconstant `list_extract` child expressions under `PRAGMA debug_verify_vector='dictionary_expression'`, which transforms child result vectors before the scalar callback. The 2,050-row loopback case uses a 5,000 ms connect timeout to avoid the default 1,000 ms connect deadline under a local request burst.
- [x] **Subtask 6.7 — Commit the scalar integration after verification.**

**Acceptance:** SQL tests preserve row mapping for supported vector shapes and concurrent execution; failures identify the one-based current-chunk slot; no DuckDB-owned memory escapes the callback.

## Task 7: Integration, Documentation, Platform Builds, and Release Evidence

**Files:**
- Create/update: `test/python/` live Ollama and compatibility tests
- Create/update: examples and install/load documentation
- Create/update: `.github/workflows/` build and artifact publishing workflow
- Create/update: versioned release artifact directories for supported platforms

**Interfaces:**
- Consumes: complete runtime from T1–T6.
- Produces: repeatable CI artifacts, deployment instructions, compatibility/performance evidence, and a release-ready distribution package.

- [x] **Subtask 7.1 — Add the live compatibility suite.** The opt-in suite passed four live tests against Ollama 0.35.1 and `tev1:0.8b`: mixed answer types and usage, eight questions, exactly 1,024 normalized source bytes, and 64 questions. The 64-question request is valid under extension schema/byte limits but receives Ollama's verified HTTP 400 because 2,488 prompt tokens exceed `num_ctx=2050`; this model-context limit is recorded in the evidence and user docs. Hardware, DuckDB, Ollama, and model details: `docs/release-evidence/2026-10-05-t7-acceptance.md`.
- [ ] **Subtask 7.2 — Add platform CI and versioned artifacts.** The native v1.5.6 workflow is configured for nine supported platforms, uploads per-platform build artifacts, and assembles successful outputs under DuckDB's version/platform layout. Seven targets directly load the emitted artifact in the SQLLogicTest suite; two targets skipped by the reusable workflow have explicit post-package LOAD jobs using matching DuckDB v1.5.6 CLIs. Deterministic mock CI remains enabled and live compatibility is an explicitly provisioned job. Actual remote Actions build/load evidence is pending because this checkout has no project remote; do not treat the matrix as executed.
- [x] **Subtask 7.3 — Document install and use.** `docs/INSTALL.md`, `docs/EXAMPLES.md`, and `docs/README.md` cover SQL/Python install/load/call examples, JSON extraction, DuckDB version matching, Ollama/model/service setup, endpoint and container guidance, remote-data disclosure, and scalar-worker waiting.
- [x] **Subtask 7.4 — Document distribution trust and complete release route.** The intended public route is DuckDB Community Extensions. Submission and approval are explicitly deferred; they are not required to finish local implementation and testing. Docs also explain custom public GitHub Pages, unsigned-binary trust via `allow_unsigned_extensions`, and the private repository authentication limitation. No external publication was performed.
- [x] **Subtask 7.5 — Benchmark and confirm defaults.** Cold/warm runs measured 1 and 8 questions; the 64-question path was measured as an explicit context-limit rejection because this model cannot run that prompt. One-question row concurrency 1/2/4/8 and eight-question concurrency 1/2 were captured, along with throughput, p50/p95/p99 request duration, queue wait, combined latency, sampled client-process peak RSS, and mock connection reuse. On this CPU-only host, higher concurrency did not improve throughput enough to justify changing the general defaults; keep two I/O threads, eight active transfers, and queue capacity 16. Evidence and scope limits are in `docs/release-evidence/2026-10-05-t7-acceptance.md`.
- [ ] **Subtask 7.6 — Run the release acceptance matrix.** Local Release build, SQL/native/Python checks, and live compatibility are verified and recorded in `docs/release-evidence/2026-10-05-t7-acceptance.md`. The nine-platform matrix now has direct artifact LOAD checks configured for all targets, but still needs an actual project GitHub Actions run before this subtask can be checked.
- [x] **Subtask 7.7 — Commit release docs, CI, and evidence.** Local checkpoint saved after deterministic and live compatibility verification.

**Acceptance:** Deterministic mock tests pass in CI; live compatibility succeeds with supported Ollama/model versions; supported platform builds load under matching DuckDB binaries; release artifacts and install instructions use the selected distribution route and version/platform layout.

## Execution Gates

1. **Gate A — Build baseline:** complete T1 before feature work; establish the pinned DuckDB/template/dependency versions.
2. **Gate B — Public contract:** complete T2; review settings defaults, SQL signature, NULL behavior, model, and endpoint.
3. **Gate C — Pure contracts:** establish the shared metadata types in T2, then complete T3 and T4 in parallel against those types; dispatcher work starts only after both implementations are stable.
4. **Gate D — Runtime lifecycle:** complete T5 with mock-server evidence before connecting it to DuckDB's vector callback.
5. **Gate E — SQL correctness:** complete T6; verify selection/validity handling, mapping, cancellation, and slot diagnostics.
6. **Gate F — Release readiness:** complete T7's live checks, platform matrix, docs, release route, and performance evidence.

## Suggested Subagent Allocation

- **Coordinator:** Complete T1 and T2 first, including `types.hpp`; own shared headers and integration points so parallel agents have a stable contract.
- **Agent A — T3:** Own `request.hpp/.cpp`, `test/cpp/test_request.cpp`, and request fixtures. Build/run only its native test target in an isolated build directory.
- **Agent B — T4:** Own `response.hpp/.cpp`, `test/cpp/test_response.cpp`, and response fixtures; consume the T2 `ValidatedQuestions` contract and use local test fixtures rather than waiting on T3's implementation. Build/run only its native test target in an isolated build directory.
- **After A and B finish — T5:** Assign transport files and mock-server tests to one agent because dispatcher lifecycle and request/response interfaces meet here.
- **Then — T6:** Integrate vector execution after T5; keep the scalar callback and registration edits under one owner.
- **Early parallel work:** A third agent can prepare T7 docs or CI files after T1/T2, with exclusive ownership of those paths. Final artifact and live compatibility acceptance wait for T6.

Use separate file ownership for concurrent agents. Review and merge each task's changes before starting dependent tasks; all agents share the workspace.

## Known Limits and Release Notes

- The exact Tev1 prompt token count cannot be proven with the documented SystemOne API. The byte guard plus live maximum-input compatibility check is the v1 contract; if a hard 2,000-token guarantee is mandatory, a stable model-matched tokenizer or prompt-preflight API is a prerequisite.
- A scalar SQL function must wait for inference results. The design moves socket I/O to dedicated threads and avoids shared-lock waits, but cannot make the scalar call itself fully nonblocking.
- Pool limits are per bound function expression. Aggregate concurrency across simultaneous expressions can exceed one expression's limit.
- Structured object/array state, image/video input, remote authentication, per-row endpoint/model overrides, partial-error JSON results, and silent truncation are out of scope for v1.
