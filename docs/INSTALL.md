# Install and configure

`ollama_systemone` is a native DuckDB extension. DuckDB stays embedded in the
CLI or application process; Ollama runs as a separate HTTP service. The
extension targets DuckDB **v1.5.6**. Use the same DuckDB version and operating
system/architecture as the extension binary: DuckDB rejects binaries built for
a different version or platform.

No release repository has been selected for this project yet. The examples
below show the local build, Community Extensions, and custom public repository
routes. Publishing and the final route choice are separate release work; the
CI workflow in this repository does not publish binaries.

## Requirements

- DuckDB v1.5.6, including the matching Python package when using Python.
- Ollama v0.35 or later, with `tev1:0.8b` downloaded.
- An HTTP endpoint reachable from the DuckDB process.

Install the model and start Ollama if your installer has not already started
the service:

```sh
ollama --version
ollama pull tev1:0.8b
ollama serve
```

In another terminal, confirm Ollama responds locally:

```sh
curl --fail --silent --show-error http://localhost:11434/api/version
ollama list
```

The default extension endpoint is
`http://localhost:11434/v1/systemone`. The function always requests
`tev1:0.8b`; callers cannot override the model per SQL row.

## Build and load a local binary

Build from the repository root using the pinned DuckDB submodule and dependency
manifest:

```sh
make
```

For a local development build, load the resulting binary by path. In the CLI:

```sh
./build/release/duckdb -unsigned
```

```sql
LOAD './build/release/extension/ollama_systemone/ollama_systemone.duckdb_extension';
```

In Python, install the matching DuckDB package and load the same local file:

```sh
python -m pip install 'duckdb==1.5.6'
```

```python
import duckdb

con = duckdb.connect(
    ":memory:",
    config={"allow_unsigned_extensions": "true"},
)
con.load_extension(
    "./build/release/extension/ollama_systemone/ollama_systemone.duckdb_extension"
)
```

The unsigned setting is needed for a locally built binary that DuckDB has not
signed. Enable it only when you trust the binary: an extension is native code
loaded into the DuckDB process. For a signed Community Extension, do not enable
this setting.

## Install a published extension

### DuckDB Community Extensions

Community Extensions are the recommended route for broad public distribution.
After this extension is accepted and published there, install it with the
matching DuckDB v1.5.6 CLI:

```sql
INSTALL ollama_systemone FROM community;
LOAD ollama_systemone;
```

The matching Python connection can install and load it as follows:

```sh
python -m pip install 'duckdb==1.5.6'
```

```python
import duckdb

con = duckdb.connect(":memory:")
con.install_extension("ollama_systemone", repository="community")
con.load_extension("ollama_systemone")
```

### Custom public GitHub Pages repository

A custom repository must serve DuckDB's version/platform directory layout. For
example, a public Pages site could contain:

```text
https://OWNER.github.io/REPO/
  v1.5.6/
    linux_amd64/ollama_systemone.duckdb_extension
    linux_arm64/ollama_systemone.duckdb_extension
    osx_amd64/ollama_systemone.duckdb_extension
    osx_arm64/ollama_systemone.duckdb_extension
    windows_amd64/ollama_systemone.duckdb_extension
```

Publish only platform binaries that have been built and verified. Replace the
placeholders with the actual public Pages base URL after a release repository
has been chosen. To install an unsigned custom binary, allow unsigned
extensions for that DuckDB process:

```sh
duckdb -unsigned
```

```sql
INSTALL ollama_systemone FROM 'https://OWNER.github.io/REPO';
LOAD ollama_systemone;
```

For Python, set the trust option when opening the connection, then install
from the same custom repository:

```python
import duckdb

con = duckdb.connect(
    ":memory:",
    config={"allow_unsigned_extensions": "true"},
)
con.execute("INSTALL ollama_systemone FROM 'https://OWNER.github.io/REPO'")
con.load_extension("ollama_systemone")
```

Treat `allow_unsigned_extensions` as a trust decision. It permits DuckDB to
load unsigned native code into the process. DuckDB's extension installer does
not authenticate to private GitHub repositories. For private distribution,
download the version- and platform-matched binary with an authenticated tool
and load its local path, or serve it from an authenticated internal repository
that DuckDB can reach.

See [DuckDB's extension distribution guide](https://duckdb.org/docs/current/extensions/extension_distribution),
[extension installation guide](https://duckdb.org/docs/current/extensions/installing_extensions),
and [extension security guide](https://duckdb.org/docs/current/operations_manual/securing_duckdb/securing_extensions)
for current DuckDB behavior.

## Configure an endpoint

The default works when the DuckDB application and Ollama share a machine and
network namespace. To use a different reachable HTTP or HTTPS endpoint, set
the extension option after loading the extension. For remote traffic, use an
HTTPS URL served by a TLS endpoint or proxy in front of Ollama:

```sql
SET ollama_endpoint = 'https://ollama-host.example/v1/systemone';
```

Python can set the same session option:

```python
con.execute(
    "SET ollama_endpoint = 'https://ollama-host.example/v1/systemone'"
)
```

When DuckDB runs in a container and Ollama runs on the host,
`localhost` inside DuckDB refers to the container itself. On Docker Desktop,
`host.docker.internal` commonly reaches the host. On Linux, add a host-gateway
mapping when starting the DuckDB container, for example:

```sh
docker run --add-host=host.docker.internal:host-gateway ...
```

Then point `ollama_endpoint` at
`http://host.docker.internal:11434/v1/systemone`. The Ollama service must also
listen on an interface reachable from the container. If DuckDB and Ollama are
both containers on one Docker network, use the Ollama service name instead,
such as `http://ollama:11434/v1/systemone`.

Ollama's default local-only listener may not accept requests from another
container. If you change its bind address, restrict access to a trusted
network; v1 does not configure remote endpoint authentication.

## Data sent to Ollama

For each non-NULL row, the extension sends the row's text state and configured
questions to the endpoint as an HTTP request. With the default endpoint, that
request stays on the local machine. A remote endpoint receives the state and
question text outside the DuckDB host. Use HTTPS for remote traffic and send
only data that is appropriate for that endpoint. The extension does not log
request bodies, but endpoint operators and any proxies follow their own data
handling policies.

## SQL worker behavior

The extension handles socket I/O on dedicated transport threads and bounds
active and queued requests. The scalar SQL call still has to wait for the
answers before returning, so a DuckDB worker can be parked while inference
finishes. This interface is not a fully nonblocking query API. The default
request timeout is 120 seconds; the extension reports an error if a request
fails or times out.
