# T7 release acceptance evidence — 2026-10-05

## Acceptance status

| Area | Result |
|---|---|
| Local Release build on Linux x86_64, DuckDB v1.5.6 | Passed |
| Deterministic SQL, request/response, transport, vector, and concurrency checks | Passed; see the final command record and release acceptance matrix below, with the tracked [implementation plan](../superpowers/plans/2026-10-04-duckdb-tev1-systemone.md) |
| Live Ollama compatibility | Passed 4 tests on Ollama 0.35.1 with `tev1:0.8b`; the model rejects the valid 64-question maximum because of its configured context |
| Native platform workflow | Configured for 9 platforms and versioned artifacts; no project Actions run is available, so cross-platform build/load evidence is pending |
| External release | Deferred by scope; Community Extensions is the intended route, with no submission or publication performed |

The workflow prepares successful platform artifacts under `v1.5.6/<platform>/`. It does not publish them. The repository's configured Git remote still points to the extension template, so this checkout cannot run the project's platform workflow.

The pinned `extension-ci-tools/scripts/modify_distribution_matrix.py` resolves the workflow inputs to these nine targets: `linux_amd64`, `linux_arm64`, `linux_amd64_musl`, `linux_arm64_musl`, `osx_amd64`, `osx_arm64`, `windows_amd64`, `windows_arm64`, and `windows_amd64_mingw`. This verifies the configured matrix definition only; it is not evidence that those builds ran.

## Environment

- Date: 2026-10-05
- Host: Linux x86_64, kernel `6.8.0-1064-azure`
- CPU: AMD EPYC 9V74 guest, 4 logical CPUs (2 cores × 2 threads)
- Memory: 16,770,490,368 bytes total (about 15.62 GiB); no swap
- Python: 3.14.2, including the pytest environment
- DuckDB CLI: `v1.5.6 (Variegata) 069cc9f9b5`
- Ollama: `0.35.1`
- Model: `tev1:0.8b`, Ollama digest `8d11b3146b7f3f4f4d5e9a64665ab2bdaf8b46e4ec42a5880b60a716ae50e3fb`, Q8_0 model layer `sha256:fa9732e3924db99f614181a7a28384a0891ae17f478db228a47c989462b2405a` (811,843,424 bytes)
- Modelfile: `num_ctx=2050`; Ollama reports CPU inference and 896 MB model residency while loaded

## Live compatibility

Command:

```sh
env NO_PROXY='*' no_proxy='*' SYSTEMONE_RUN_LIVE_OLLAMA=1 \
  /usr/local/py-utils/venvs/pytest/bin/python -m pytest -q -s \
  test/python/test_live_ollama.py
```

Result: **4 passed**. The suite checks:

1. Mixed `noul`, `choice`, and `score` results, response fields, and usage.
2. Eight `noul` questions against the provisioned model.
3. One question with exactly 1,024 normalized source bytes.
4. The schema maximum of 64 questions.

The extension accepts the 64-question request under its schema and byte limits. Ollama 0.35.1 rejects it for the model's context setting. A direct request reproduced:

```text
HTTP 400 {"error":"prompt 0 has 2488 tokens; expected 1–2050 (input is never truncated)"}
```

The live test requires the extension to identify HTTP 400 at slot 1 and separately verifies that Ollama's error body names the prompt context limit. Version 1 does not tokenize or preflight against the model context, as required by the design constraints. The 64-question cap remains a schema limit, not a guarantee that every installed model context can accept the resulting prompt.

## Benchmark method and limits

The benchmark driver exercises the actual `TransportDispatcher` directly, without DuckDB planning or scalar-callback overhead. Each row uses a short synthetic state (`benchmark-row-N`); each question is `noul` with instruction `x`. The request settings are 1,000 ms connect timeout, 120,000 ms total timeout, 65,536-byte request limit, 1,048,576-byte response limit, and 1,024-byte normalized-source limit. All runs used two transport I/O threads and queue capacity 16. Question-count runs use 1, 8, or 64 questions per request.

For cold runs, `--cold-ollama` ran `ollama stop tev1:0.8b` with a 10-second subprocess deadline and confirmed the model was absent from `/api/ps` before issuing one probe request. A regression test uses a deliberately slow local Ollama CLI to verify that cold preparation does not wait indefinitely. Warm summaries either follow a separate two-row warmup batch or exclude the first request in a serial run. Percentiles use linear interpolation over the recorded request rows. Small samples are descriptive measurements, not a statistically powered comparison.

The harness samples the benchmark driver process's `VmRSS` every 10 ms. Those peak RSS values exclude Ollama and the mock server. Ollama's reported 896 MB residency and a post-run `llama-server` RSS snapshot of 1,252,956 KiB are separate observations, not a sampled peak. Ollama does not expose the live HTTP connection cache, so connection reuse is measured only against the deterministic HTTP/1.1 mock by counting distinct client TCP peers; it is an estimate for that mock and does not represent Ollama connection reuse.

### Live Ollama measurements

Times are milliseconds; percentile cells list `p50 / p95 / p99`. Throughput is for the measured batch. Queue wait and request duration are reported separately; combined latency is their sum.

| Questions | In-flight | Measured rows | Cold probe | Request duration | Queue wait | Including queue | Throughput rows/s | Failed rows | Peak client RSS |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 8 | 2,951 | 1,133 / 1,144.5 / 1,147.3 | 1,931.5 / 5,500.4 / 5,818.5 | 3,074.5 / 6,632.5 / 6,948.9 | 1.138 | 0 | 8.19 MiB |
| 1 | 2 | 8 | 2,994 | 2,220 / 2,237.4 / 2,241.9 | 761.5 / 4,275.4 / 4,587.9 | 2,994 / 6,496.5 / 6,806.5 | 1.162 | 0 | 8.31 MiB |
| 1 | 4 | 8 | 3,035 | 3,104.5 / 4,702 / 4,702 | 63 / 2,152.4 / 2,481.7 | 3,167 / 6,846 / 7,171.6 | 1.103 | 0 | 8.57 MiB |
| 1 | 8 | 8 | 2,993 | 4,371.5 / 7,375.4 / 7,407.9 | 0 / 0 / 0 | 4,371.5 / 7,375.4 / 7,407.9 | 1.078 | 0 | 8.49 MiB |
| 8 | 1 | 4 | 7,580 | 5,376 / 24,727.5 / 27,453.5 | 15,771 / 34,444.3 / 35,094.5 | 32,547.5 / 39,790.9 / 40,431 | 0.099 | 0 | 8.27 MiB |
| 8 | 2 | 4 | 7,270 | 19,338.5 / 34,744.1 / 34,939.2 | 813.5 / 29,983.9 / 33,987.2 | 34,988.5 / 39,505.9 / 40,143.6 | 0.099 | 0 | 8.44 MiB |
| 64 | 1 | 8 context rejections | 2,068 | 130 / 134.8 / 135.8 | 530 / 880.4 / 912.1 | 658 / 1,010.3 / 1,041.3 | 7.576 rejected requests/s | 8 expected HTTP 400 | 8.45 MiB |

The one- and eight-question inference runs completed without transport failures. Raising one-question concurrency from 1 to 8 did not improve throughput on this CPU-only Ollama configuration. For eight questions, concurrency 1 and 2 each processed four measured rows at about 0.099 rows/s; the small sample does not justify a general default change. The planned defaults remain two I/O threads, eight active transfers, and queue capacity 16. The 64-question row is an HTTP 400 context-rejection measurement, not inference performance.

### Deterministic mock measurements and connection reuse

| Questions | In-flight | Request duration p50/p95/p99 (ms) | Queue wait p50/p95/p99 (ms) | Throughput rows/s | Requests / unique TCP peers / estimated reuse | Failures | Peak client RSS |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 0 / 40.7 / 40.9 | 42 / 109.7 / 121.1 | 63.592 | 8 / 2 / 6 | 0 | 8.45 MiB |
| 8 | 4 | 21 / 41 / 41 | 0 / 1.7 / 1.9 | 181.780 | 8 / 4 / 4 | 0 | 8.40 MiB |
| 64 | 8 | 3 / 27.1 / 37.4 | 0 / 0 / 0 | 186.265 | 8 / 7 / 1 | 0 | 8.65 MiB |

The mock runs verify transport scaling, queue reporting, and keep-alive reuse accounting at each question size. Their latency and throughput values are not model inference measurements.

## Release acceptance matrix

| Gate | Evidence | Status |
|---|---|---|
| T1 pinned scaffold and Release load | DuckDB v1.5.6 Release extension rebuilt with Ninja and loaded in the matching CLI; `duckdb_extensions()` reported `installed=true`, `loaded=true`. The tracked plan retains the completed T1 Debug/Release gate. | Passed |
| T2 settings and SQL contract | SQLLogicTest: 16 assertions across 2 cases; the tracked implementation plan records registration/settings checks under T2 | Passed |
| T3 request contract | Native request contract: 73 checks; `systemone_test_request` CTest passed | Passed |
| T4 response contract | `systemone_test_response` CTest passed | Passed |
| T5 bounded dispatcher and lifecycle | Transport CTest and deterministic Python transport mock tests passed; final focused suite below includes the transport tests | Passed |
| T6 vector SQL integration | 16 SQL assertions and 27 focused vector/concurrency/transport tests passed at T6; final focused suite below adds the two benchmark tests | Passed |
| T7 live Ollama compatibility | Four tests, including eight questions, source-byte boundary, and documented context rejection | Passed |
| T7 local benchmark and defaults | Cold/warm results and deterministic mock measurements above | Passed with stated measurement limits |
| T7 nine-platform build and load matrix | CI workflow configured; requires project GitHub Actions execution | **Pending** |
| Community Extensions submission / publication | Explicitly deferred; neither is required to finish local implementation and testing | **Deferred; no external publication performed** |

## Local verification commands

The final local pass completed after documenting the results and before creating the local T7 checkpoint commit:

```sh
/tmp/tev1-ninja/bin/ninja -C build/release -j4 \
  ollama_systemone_loadable_extension systemone_transport_benchmark
build/release/duckdb -c "LOAD './build/release/extension/ollama_systemone/ollama_systemone.duckdb_extension'; SELECT extension_name, installed, loaded FROM duckdb_extensions() WHERE extension_name = 'ollama_systemone';"
make test
/usr/local/py-utils/venvs/pytest/bin/python -m pytest -q \
  test/python/test_benchmark.py test/python/test_transport.py \
  test/python/test_vectorized.py test/python/test_concurrency.py
/tmp/tev1-actionlint/actionlint -color \
  -ignore 'label "ollama-systemone" is unknown' \
  .github/workflows/MainDistributionPipeline.yml \
  .github/workflows/ollama-systemone-ci.yml
```

Results:

- Release extension and benchmark targets built successfully with Ninja `-j4`.
- `make test`: 16 assertions passed in 2 SQLLogicTest cases.
- Native request/response CTest: 2/2 passed.
- Focused Python benchmark, transport, vector, and concurrency suite: 29 passed.
- Opt-in live Ollama compatibility suite: 4 passed.
- Actionlint passed; the one ignored rule is the repository's custom self-hosted runner label `ollama-systemone`.
- Python syntax compilation and `git diff --check` passed.
