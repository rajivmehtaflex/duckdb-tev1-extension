# DuckDB Tev1 SystemOne Extension — Implementation Plan

## Goal and acceptance

Build a loadable C++ DuckDB extension that exposes all three Tev1 decision question types through one SQL call per input row. The caller supplies one text state and a JSON configuration containing 1–64 named questions. The extension sends that row’s full question set in one HTTP request to Ollama `/v1/systemone` and returns the complete structured response as DuckDB `JSON`.

A release is acceptable when the extension builds and loads against the pinned DuckDB version; its SQL function preserves row-to-answer mapping across chunk shapes and concurrent execution; mock-server tests cover protocol, validation, failures, cancellation, and limits; and the live compatibility test succeeds with the supported Ollama/model combination. The function must bound memory, concurrency, response size, and wait time.

## Decisions and public SQL contract

- **Language and compatibility:** C++ extension using the official DuckDB extension-template layout, initially targeting DuckDB v1.5.6 and an extension-template commit whose DuckDB submodule is v1.5.6. Build and CI use that pinned pair. Supporting another DuckDB version requires an explicit compatibility build and test.
- **Function:** `ollama_systemone(state VARCHAR, config JSON) -> JSON`. Register it as fallible and volatile because it can throw and calls an external service. Use DuckDB’s default NULL handling: a NULL state or config yields NULL without an HTTP request.
- **Input:** `state` must be nonempty text. `config` must be an object with exactly one supported top-level key, `questions`. Each question has `type`, nonempty string `instructions`, and `criteria` where required. Support only text states and string instructions in v1; structured JSON state/instructions are out of scope.
- **Question validation:** Require 1–64 questions. `noul` accepts omitted criteria or a `true`/`false` object with string descriptions. `choice` accepts an ordered object of 2–26 nonempty option keys mapped to strings or JSON null. `score` accepts an array of 2–26 strings, lowest level first. Reject unknown keys, duplicate JSON object keys, invalid types, empty names/instructions, and invalid criterion values before sending a request. Preserve question and option order because it controls answer ordering and tie behavior.
- **Request:** Always send `model: "tev1:0.8b"`, the row’s `state`, and its complete `questions` object. Do not allow a per-call model override. Default endpoint is `http://localhost:11434/v1/systemone`; allow an endpoint setting for compatible local deployments. Disable redirects. Add `keep_alive` only when its extension setting is explicitly set; by default omit it and let Ollama apply its own policy.
- **Return:** Return the full Ollama response object, retaining `model`, `answers`, and `usage`. Preserve the server’s answer fields and numeric values. Require one answer of the expected type for every configured question. Validate `noul` and probabilities in [0,1], choice labels against configured keys, score within `[0, criterion_count - 1]`, confidence in [0,1], and required probability/legend fields. Permit additional response fields for forward compatibility. SQL callers can extract fields with DuckDB JSON functions, for example `json_extract(result, '$.answers.severity.score')`.
- **Failure:** Raise a DuckDB query error by default for invalid input, transport failures, non-2xx status, malformed JSON, invalid response shape, timeout, or limit violation. Error text reports the one-based row slot in the current input `DataChunk` and a concise cause; it cannot report a table key such as `ticket_id` because that value is not an argument to this function.

Example:

```sql
SELECT ticket_id,
       ollama_systemone(
         ticket_text,
         '{"questions": {
           "is_urgent": {"type":"noul", "instructions":"Is the user angry?"},
           "department": {"type":"choice", "instructions":"Route this ticket.",
             "criteria":{"billing":null,"tech":null,"none":null}},
           "severity": {"type":"score", "instructions":"Rate priority level.",
             "criteria":["Low","Medium","High"]}
         }}'::JSON
       ) AS model_analysis
FROM support_tickets;
```

## Repository layout and component responsibilities

```text
duckdb-tev1/
  CMakeLists.txt                 # Extension build and pinned dependencies
  Makefile                       # Standard template build/test entry points
  extension_config.cmake         # Extension name, source list, version metadata
  vcpkg.json                     # Pinned libcurl and ordered JSON dependencies
  src/
    include/tev1_extension.hpp   # Extension declaration and shared-state types
    tev1_extension.cpp           # Load/name/version/extension entry hooks
    registration.cpp             # Settings and scalar-function registration
    systemone/
      config.hpp/.cpp            # Runtime settings and per-call config snapshot
      request.hpp/.cpp           # Parse, validate, preserve order, serialize request
      transport.hpp/.cpp         # Bounded dispatcher and libcurl multi event loops
      response.hpp/.cpp          # Response validation and JSON result construction
      scalar_function.cpp        # DataChunk loop, row slots, function properties
  test/
    sql/ollama_systemone.test    # SQL API and result behavior via mock endpoint
    mock/                        # Deterministic mock HTTP server and fixtures
    python/                      # Integration, concurrency, and live Ollama tests
```

Use the extension-template lifecycle: extension `Load` registers settings and the function through `ExtensionLoader`; `Name`, `Version`, and the C++ extension entry macro follow the pinned template. Register options through `DBConfig::AddExtensionOption` with session scope. The scalar bind callback creates one shared dispatcher in its `FunctionData`; DuckDB execution workers for that bound function expression share it, and its destructor cancels pending work and joins I/O threads after query execution ends. Pool limits are per bound function expression, so concurrent statements each have their own pool. Never retain `DataChunk`, `Vector`, `string_t`, or result-vector pointers after the scalar callback returns.

Use `nlohmann::ordered_json` to preserve object member order. Parse through a small SAX validation layer that detects duplicate keys before constructing the ordered JSON value; ordinary object parsing can silently replace duplicate keys. Use libcurl multi handles in two dedicated I/O event-loop threads per bound function expression so each handle can reuse keep-alive connections. Pin dependency versions in the vcpkg manifest and CI baseline. Return `LogicalType::JSON()` and write values through DuckDB-managed string storage.

## Settings and resource limits

Register the following DuckDB settings with the indicated v1 defaults; capture a consistent settings snapshot for each query invocation:

| Setting | Default | Purpose |
|---|---:|---|
| `ollama_endpoint` | `http://localhost:11434/v1/systemone` | Complete endpoint URL; allow `http` or `https` only |
| `ollama_connect_timeout_ms` | `1000` | Bound connection establishment |
| `ollama_request_timeout_ms` | `120000` | Bound one logical request, including cold model load |
| `ollama_max_inflight` | `8` | Active transfers shared by workers for one bound function expression |
| `ollama_queue_capacity` | `16` | Queued requests for one bound function expression |
| `ollama_max_request_bytes` | `65536` | Enforce Ollama’s documented 64 KiB request-body limit |
| `ollama_max_response_bytes` | `1048576` | Bound response buffering to 1 MiB |
| `ollama_max_source_bytes` | `1024` | Conservative byte guard for normalized state, questions, and requested-field text |
| `ollama_retry_count` | `1` | Retry once only if the connection failed before any request body was sent |
| `ollama_keep_alive` | unset | Optional model residency value; omitted unless configured |

Never truncate state, instructions, or criteria. Reject over-limit input with a clear error before enqueueing. `ollama_max_request_bytes` and `ollama_max_source_bytes` are independent checks: JSON escaping and request fields count toward the HTTP body limit, while normalized model input is checked against the source-byte limit.

**Token-limit limitation:** Ollama documents Tev1’s effective context as about 2,000 tokens, but `/v1/systemone` has no documented preflight token-count operation. Do not claim the 1,024-byte source guard is an exact tokenizer guarantee. It is a conservative v1 guard that leaves substantial room for server-added prompt framing. The live compatibility suite must exercise the largest accepted state and question configurations on the pinned Ollama/model release and inspect returned usage where available. If the target server reports or exhibits context failures, lower the default byte guard. A hard exact-token guarantee requires a stable tokenizer/prompt-preflight API from Ollama and is outside this v1 interface.

## Vectorized execution and transport flow

1. DuckDB invokes the scalar callback with each actual `DataChunk` (up to the engine’s configured vector size; do not assume every chunk contains 2,048 rows). Register the function volatile and fallible. Read text and config vectors through DuckDB’s vector/selection APIs, applying each vector’s own selection index and validity mask. Preserve input slot indices independently of completion order.
2. If config is a constant vector, parse and validate it once for that chunk or local function state. If config varies by row, parse each distinct row value and reuse the parsed ordered representation for repeated values within that chunk. Do not infer constancy from equal values alone across unrelated chunks.
3. For each non-NULL row, validate state/config, build the complete request, check source and request byte limits, then submit an owned request object to the dispatcher with a query cancellation token and the original chunk slot. The request object owns all strings and serialized bytes.
4. The dispatcher has two I/O event-loop threads by default, each owning its own libcurl multi handle and connection cache. Enforce `ollama_max_inflight` and the bounded queue across all DuckDB workers executing this bound function expression. Separate SQL expressions and concurrent queries have independent pools and limits. Use `Content-Type: application/json`, keep-alive, disabled redirects, configured connect/total timeouts, bounded response buffering, and no logging of state/config/body contents.
5. Submit at most the available in-flight window from a chunk, collect completions, and then submit more rows. This prevents a 2,048-row chunk from creating an unbounded number of queued payload copies. Completed results return through futures/promises or an equivalent owned completion queue, tagged with the original chunk slot.
6. Parse and validate all successful responses off the DuckDB output vector. After required chunk requests succeed, write JSON strings into the result vector using DuckDB-managed string storage and set validity correctly. No network callback writes directly to a DuckDB vector.
7. If any request fails, stop submitting additional rows from that chunk. Allow already submitted lower-slot requests to finish, cancel submitted higher-slot requests, drain completion cleanup, and raise the failure for the lowest failing input slot in that submitted window. On query interruption, stop admission, abort queued and active work, and release all owned request/response buffers. The dispatcher must never keep query-owned pointers.

**Worker wait behavior:** The transport is asynchronous and performs socket I/O on dedicated I/O threads, so DuckDB workers do not perform synchronous socket reads or hold a shared mutex while waiting. A scalar function still must wait for its output values before returning, so its executing DuckDB worker can be parked while the bounded future completes. This design prevents indefinite waits and lets requests overlap across rows and queries; it cannot provide a truly nonblocking DuckDB scalar call. If the requirement means that no DuckDB worker may wait at all, that requirement conflicts with the selected synchronous scalar SQL contract and needs a separate asynchronous job/result API or DuckDB scheduler support.

## Error, retry, and observability policy

- Retry at most once only when libcurl confirms the request body was not sent. Do not retry after upload begins, on HTTP errors, malformed responses, or timeout; the server may already have run the inference.
- Return a query error containing function name, chunk row slot, HTTP status or transport category, and a concise message. Do not include state, instructions, criteria, response body, or secrets in errors/logs.
- Treat a missing model, unsupported Ollama version/route, refused connection, and timeout as distinguishable diagnostic categories. Document that Ollama 0.35 or later is required by Tev1 and that users must install `tev1:0.8b` locally.
- Record counters/timings only: queued/active/completed requests, queue wait, request duration, timeout/retry/error counts, and response bytes. Keep diagnostics opt-in or expose through extension metrics without logging input contents.

## Ordered implementation tasks

1. **Pin scaffold and build:** Create the extension repository from the official C++ template matching DuckDB v1.5.6. Add extension metadata, CMake/build entry points, and pinned libcurl/ordered-JSON dependencies. Acceptance: debug and release extension artifacts compile and load in the pinned DuckDB build.
2. **Define types and registration:** Add extension lifecycle hooks, settings, JSON logical return type, and `ollama_systemone(VARCHAR, JSON) -> JSON` registration. Mark it volatile/fallible and preserve default NULL propagation. Acceptance: catalog signature/settings tests pass; NULL rows submit no HTTP requests.
3. **Implement deterministic validation/serialization:** Add ordered config parsing, duplicate-key detection, validation, request construction, and independent request/source-size guards. Acceptance: fixture tests cover allowed shapes, all specified validation errors, escaping, ordering, and limits without a server.
4. **Implement response parsing:** Validate response envelope and per-question answer types/ranges while preserving permitted extra fields. Acceptance: valid all-types fixture round-trips complete `model`/`answers`/`usage`; malformed/missing/wrong-type fixtures fail clearly.
5. **Implement dispatcher:** Add a `FunctionData`-owned libcurl multi dispatcher shared by execution workers for that bound expression, keep-alive connection reuse, per-expression limits, backpressure, deadlines, cancellation, bounded retries, and teardown. Acceptance: mock server demonstrates bounded active count per expression, reused connections, request ordering independence, cancellation, and no leaked worker threads.
6. **Connect the vectorized callback:** Add chunk decoding, constant-config reuse, selection/null handling, bounded submission windows, completion-to-slot mapping, and DuckDB string output writes. Acceptance: SQL tests pass for multiple chunks, dictionary/constant/filtered vectors, delayed out-of-order responses, and concurrent queries.
7. **Integration and packaging:** Add the live Ollama compatibility suite, example SQL, install/load instructions, platform CI, and versioned release artifacts. Acceptance: mock suite is deterministic in CI; live suite succeeds with Ollama >=0.35 and `tev1:0.8b` installed; macOS, Linux, and Windows builds pass for supported architectures.

## Verification plan

- **Registration and SQL:** Load the extension, inspect its function signature/settings, call it with the example config, test JSON extraction, test NULL propagation, and verify volatility by observing one mock request per actual evaluated row.
- **Contract boundaries:** Test question counts 0, 1, 64, 65; choice/score option counts 1, 2, 26, 27; valid/invalid noul criteria; duplicate/empty names; missing fields; unsupported keys/types; non-string instructions; empty state; malformed JSON; and Unicode/escaped text.
- **Vector behavior:** Use rows that span several chunks, constant and varying configs, dictionary vectors, filtered selections, NULLs, and out-of-order delayed server responses. Assert exact row-slot mapping and no calls for NULL/unselected rows.
- **Protocol and failure:** Mock 2xx valid responses for all types, missing answers, unexpected labels, nonfinite/out-of-range values, malformed/oversized response bodies, non-2xx statuses, refused connection, timeout, request-body limit, queue saturation, and retry eligibility. Assert bounded active requests and deterministic error row reporting.
- **Cancellation/lifecycle:** Interrupt during queue admission and active I/O; finish or cancel a query with pending work; run multiple concurrent queries; assert queued/active work stops, callbacks never touch released DuckDB memory, expression-owned dispatcher threads/handles are joined and freed, and observed aggregate concurrency matches the documented per-expression limit.
- **Live Ollama:** Verify the minimum supported version and installed model, then run mixed `noul`/`choice`/`score` requests, maximum accepted source-size fixtures, and one-to-64 question cases. Check response shape and returned usage without asserting particular semantic probabilities. Record model tag, Ollama version, DuckDB version, and hardware in the compatibility result.
- **Performance:** Benchmark warm and cold inference on documented hardware at 1, 8, and 64 questions and at increasing row concurrency. Report throughput, p50/p95/p99 latency, queue wait, peak memory, and observed connection reuse. Use the results to confirm the defaults (two I/O threads, eight active transfers, queue capacity 16); change defaults only with measured evidence.

## Assumptions and explicit limitations

- The SQL interface, language, model, endpoint default, per-row request grouping, and fail-query behavior are fixed as described above.
- `confidence` is returned as a probability-concentration metric, not a calibrated chance of correctness. Scores are probability-weighted zero-based levels.
- The original requirement to avoid locking the calling database thread is interpreted as avoiding synchronous socket operations, shared locks during waits, and unbounded blocking. A scalar function necessarily waits for values and therefore occupies/parks its DuckDB worker until completion; this is an acknowledged limitation of the selected API. Pool limits are per bound function expression; aggregate concurrency across simultaneous queries is the per-expression limit multiplied by the number of active expressions.
- The public SystemOne API does not currently provide a documented exact-token preflight for the rendered Tev1 prompt. V1 uses conservative byte limits plus live compatibility checks, not an exact tokenizer proof. If exact compliance with a 2,000-token hard ceiling is mandatory, implementation must wait for or add a supported model-matched prompt tokenizer path.
- State is text only; structured object/array state, image/video input, remote authentication, per-row endpoint/model overrides, partial-error JSON results, and silent truncation are out of scope for v1.

## References

- [Ollama Tev1 model and SystemOne contract](https://ollama.com/library/tev1)
- [Ollama SystemOne request compiler and response construction](https://github.com/ollama/ollama/blob/main/decision/systemone.go)
- [Ollama SystemOne request/response types](https://github.com/ollama/ollama/blob/main/decision/types.go)
- [DuckDB C++ extension template](https://github.com/duckdb/extension-template)
- [DuckDB C++ scalar function API](https://duckdb.org/docs/current/clients/cpp)
- [DuckDB release calendar](https://duckdb.org/release_calendar)
