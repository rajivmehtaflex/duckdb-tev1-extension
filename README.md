# DuckDB Tev1 SystemOne Extension

This DuckDB extension adds `ollama_systemone(state VARCHAR, config JSON) -> JSON`.
It sends text and typed questions to a SystemOne decision-model endpoint and
returns the model's structured answers. Supported question types are `noul`
(yes/no probability), `choice`, and `score`.

## Providers

### Ollama

Ollama is the primary provider. Use Ollama v0.35 or later and download the
`tev1:0.8b` model:

```sh
ollama pull tev1:0.8b
```

The default endpoint is `http://localhost:11434/v1/systemone`.

### llama.cpp

You can configure a different endpoint with `ollama_endpoint` if the server
implements the TypeSafe-compatible `POST /v1/systemone` API and returns the
response fields this extension validates. The upstream llama.cpp server
documents a SystemOne endpoint; see its [API documentation](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md#typesafe-compatible-api-endpoints).

This extension always sends `"model": "tev1:0.8b"`. Check that your selected
llama.cpp server and decision model accept that request. A regular
`/v1/chat/completions` endpoint is not compatible, and this repository does not
include a live llama.cpp integration test. Compatibility depends on the
server/model combination, not only on the URL.

## Build

Build for the same DuckDB version, operating system, and architecture where you
will load the extension. The target is DuckDB v1.5.6. A local build produces an
unsigned native extension, so only load a binary you trust.

```sh
git clone https://github.com/rajivmehtaflex/duckdb-tev1-extension.git
cd duckdb-tev1-extension
git submodule update --init --recursive
make
```

The build uses CMake, a C++17 toolchain, and the pinned vcpkg dependencies. See
[`docs/build-baseline.md`](docs/build-baseline.md) for the pinned vcpkg setup and
[`docs/INSTALL.md`](docs/INSTALL.md) for platform-specific build and install
details. No prebuilt release binary is currently provided by this repository.

## Load and query

For the locally built CLI:

```sh
./build/release/duckdb -unsigned
```

Then load JSON support and the extension:

```sql
LOAD json;
LOAD './build/release/extension/ollama_systemone/ollama_systemone.duckdb_extension';
```

Example query:

```sql
SELECT ollama_systemone(
    'The customer was charged twice and cannot access the account.',
    '{"questions":{"department":{"type":"choice","instructions":"Choose the handling team.","criteria":{"billing":null,"account_access":null,"other":null}}}}'::JSON
) AS result;
```

The result contains `model`, `answers`, and `usage`. For example, extract the
selected department with:

```sql
SELECT json_extract_string(result, '$.answers.department.choice') AS department
FROM (
    SELECT ollama_systemone(
        'The customer was charged twice and cannot access the account.',
        '{"questions":{"department":{"type":"choice","instructions":"Choose the handling team.","criteria":{"billing":null,"account_access":null,"other":null}}}}'::JSON
    ) AS result
);
```

For Python, install the matching package:

```sh
python -m pip install 'duckdb==1.5.6'
```

Connect with `config={"allow_unsigned_extensions": "true"}`, run `LOAD json`,
and load the extension file shown above. See
[`docs/EXAMPLES.md`](docs/EXAMPLES.md) for a complete Python example.

## Docker, Colab, and Codespaces

DuckDB loads the extension inside the CLI or Python process; the inference
server runs separately. This repository does not provide a Docker image. One
way to run the model server in Docker is:

```sh
docker network create tev1-net
docker run -d --name ollama --network tev1-net \
  -p 127.0.0.1:11434:11434 \
  -v ollama:/root/.ollama \
  ollama/ollama
docker exec ollama ollama pull tev1:0.8b
```

If the DuckDB application container is also attached to `tev1-net`, set its
endpoint to the Ollama service name:

```sql
SET ollama_endpoint = 'http://ollama:11434/v1/systemone';
```

If DuckDB runs on the Docker host, publish Ollama's port and use
`http://127.0.0.1:11434/v1/systemone`. If DuckDB and Ollama are in separate
containers, use their shared Docker network and the service name; `localhost`
inside the DuckDB container points back to DuckDB itself.

In Colab or Codespaces, load the extension into the notebook or Codespace's
DuckDB process and make sure the configured endpoint is reachable from that
runtime. `localhost` refers to the runtime itself, not another machine or
container. Build or provide an extension binary matching the runtime's DuckDB
version and platform. Hosted notebook runtimes are temporary, so setup and
build steps may need to be repeated after a reset. For a local Colab Docker
runtime, Google provides CPU and GPU runtime images for Linux/amd64; see its
[local runtime instructions](https://research.google.com/colaboratory/local-runtimes.html).

The extension has no setting for API-key or Bearer authentication; keep remote
inference endpoints on a trusted private network. The state text and questions
for each non-NULL row are sent to the configured endpoint.

See [`docs/INSTALL.md`](docs/INSTALL.md) for container networking, runtime
requirements, configuration, limits, and troubleshooting.
