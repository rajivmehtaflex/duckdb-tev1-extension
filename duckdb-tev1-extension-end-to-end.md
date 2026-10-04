# DuckDB Tev1 Extension: End-to-End Installation and Usage

DuckDB remains embedded in the consuming application. The extension is a native DuckDB plugin loaded into that process. Ollama runs separately as a local service: SQL calls the extension, the extension sends HTTP requests to Ollama, and the response returns to DuckDB as JSON.

## 1. Build and distribute the extension

Build a binary for every supported DuckDB version and operating system/architecture. DuckDB checks both when loading an extension, so a binary built for a different DuckDB version or platform is rejected. This guide uses the implementation plan’s DuckDB v1.5.6 target.

There are two public installation routes:

1. **DuckDB Community Extensions (recommended for broad distribution):** submit the extension source and metadata to the Community Extensions repository. After it is accepted and published, DuckDB signs the binary and users install it with `INSTALL ollama_systemone FROM community;`.
2. **Your own public GitHub repository:** keep the source in GitHub, use GitHub Actions to build the extension for each supported DuckDB version/platform, then publish the resulting binaries through GitHub Pages in DuckDB’s custom extension repository layout. DuckDB’s `INSTALL ... FROM` expects that repository layout; it does not install directly from a Git clone URL or a GitHub Releases page URL.

For an owned repository, a typical publishing workflow is:

1. Start from the [DuckDB extension template](https://github.com/duckdb/extension-template) and its CI workflow. Build against the exact DuckDB release and platform combinations you intend to support.
2. Have GitHub Actions collect the build artifacts and copy each binary to a `gh-pages` publishing directory using the extension repository layout below. Publish that directory to GitHub Pages.
3. In the repository’s GitHub settings, enable Pages for the branch or deployment environment used by the workflow. The Pages URL becomes the custom repository URL used in `INSTALL`.

For example, the published site for DuckDB v1.5.6 might contain:

```text
https://OWNER.github.io/REPO/
  v1.5.6/
    linux_amd64/
      ollama_systemone.duckdb_extension
    linux_arm64/
      ollama_systemone.duckdb_extension
    osx_amd64/
      ollama_systemone.duckdb_extension
    osx_arm64/
      ollama_systemone.duckdb_extension
    windows_amd64/
      ollama_systemone.duckdb_extension
```

Publish only platforms that the build actually produces. Add a separate version directory for every DuckDB release supported. DuckDB can also use the compressed `.duckdb_extension.gz` file where published.

These GitHub Pages binaries are normally unsigned unless they come through DuckDB’s Community Extensions process. Users of a custom unsigned repository must explicitly allow unsigned extensions. Treat that setting as a trust decision: it allows loading unsigned native code into the DuckDB process. For a private GitHub repository, DuckDB’s installer does not provide GitHub token authentication. Use an authenticated download step and load the downloaded local file, or publish through an authenticated internal artifact repository that DuckDB can reach.

References: [DuckDB extension distribution](https://duckdb.org/docs/current/extensions/extension_distribution), [extension template distribution guide](https://github.com/duckdb/extension-template), [DuckDB extension security](https://duckdb.org/docs/current/operations_manual/securing_duckdb/securing_extensions).

## 2. Install and start Ollama on each target machine

Install Ollama for the target operating system, then download Tev1 and start the service:

```sh
ollama pull tev1:0.8b
ollama serve
```

Some installers start Ollama automatically. Tev1 requires Ollama 0.35 or later. Confirm the API responds at `http://localhost:11434` before using the extension.

Reference: [Ollama Tev1 instructions](https://ollama.com/library/tev1).

## 3. Install and load the extension in DuckDB

In the DuckDB CLI, install the published community extension and load it into the current DuckDB process:

```sql
INSTALL ollama_systemone FROM community;
LOAD ollama_systemone;
```

Installation downloads the binary to DuckDB’s extension cache. Load the extension again when a new DuckDB process starts.

For your public GitHub Pages repository, use its base URL (replace `OWNER` and `REPO` with the GitHub account/organization and repository name):

```sql
INSTALL ollama_systemone FROM 'https://OWNER.github.io/REPO';
LOAD ollama_systemone;
```

If Pages is configured for the account-level site repository named `OWNER.github.io`, use `https://OWNER.github.io` as the base URL. The repository must expose DuckDB’s version/platform directory structure, for example:

```text
https://OWNER.github.io/REPO/
  v1.5.6/
    linux_amd64/
      ollama_systemone.duckdb_extension
```

For unsigned binaries, start the DuckDB CLI with unsigned extensions enabled:

```sh
duckdb -unsigned
```

Then run the `INSTALL` and `LOAD` statements above in that process. In Python, enable the setting when opening the connection and install from the same Pages URL:

```python
import duckdb

con = duckdb.connect(
    ":memory:",
    config={"allow_unsigned_extensions": "true"},
)
con.execute("INSTALL ollama_systemone FROM 'https://OWNER.github.io/REPO'")
con.load_extension("ollama_systemone")
```

Community Extensions users do not need the unsigned setting. For private GitHub Releases, download the matching binary with an authenticated tool such as `gh`, then load its local path; alternatively, mirror it to a private HTTPS custom repository.

In Python, use a DuckDB package version matching the extension build:

```sh
pip install "duckdb==1.5.6"
```

```python
import duckdb

con = duckdb.connect(":memory:")
con.install_extension("ollama_systemone", repository="community")
con.load_extension("ollama_systemone")
```

For a trusted but unsigned local binary downloaded from a GitHub Release or built locally, enable unsigned extensions when creating the connection and load the downloaded path:

```python
con = duckdb.connect(
    ":memory:",
    config={"allow_unsigned_extensions": "true"},
)
con.load_extension("/path/to/ollama_systemone.duckdb_extension")
```

References: [DuckDB extension installation](https://duckdb.org/docs/current/extensions/installing_extensions), [DuckDB Python extension loading](https://duckdb.org/docs/stable/clients/python/overview).

## 4. Call the function from SQL

Pass each row’s text and a JSON configuration containing all questions. The function returns the complete Ollama response as DuckDB JSON:

```sql
SELECT ticket_id,
       ollama_systemone(
         ticket_text,
         '{"questions": {
           "is_urgent": {
             "type": "noul",
             "instructions": "Is the user angry?"
           },
           "department": {
             "type": "choice",
             "instructions": "Route this ticket.",
             "criteria": {
               "billing": null,
               "tech": null,
               "none": null
             }
           },
           "severity": {
             "type": "score",
             "instructions": "Rate priority level.",
             "criteria": ["Low", "Medium", "High"]
           }
         }}'::JSON
       ) AS model_analysis
FROM support_tickets;
```

Extract answer fields with DuckDB’s JSON functions:

```sql
SELECT ticket_id,
       json_extract_string(model_analysis, '$.answers.department.choice') AS department,
       CAST(json_extract(model_analysis, '$.answers.is_urgent.noul') AS DOUBLE) AS urgent_probability,
       CAST(json_extract(model_analysis, '$.answers.severity.score') AS DOUBLE) AS severity
FROM analyzed_tickets;
```

The `noul` value is a probability, not a SQL Boolean. The score is the probability-weighted, zero-based rubric level. The confidence value describes probability concentration; it is not calibrated correctness probability.

## 5. Configure the endpoint when needed

The default endpoint is `http://localhost:11434/v1/systemone`. It works when the DuckDB application and Ollama share a machine and network namespace.

If DuckDB runs in a container while Ollama runs on its host or another machine, configure `ollama_endpoint` to an address reachable from the DuckDB container. `localhost` inside a container refers to that container. The extension sends row text to the configured endpoint, so using a remote endpoint sends that text outside the DuckDB host.

## Runtime flow

```text
Application process
  DuckDB query
    → ollama_systemone(state, questions)
      → HTTP request to Ollama
        → Tev1 inference
      ← JSON response
    ← DuckDB JSON result
```

DuckDB does not need to run as a server. “In-memory” describes where DuckDB stores its database state; the application can still load a native extension and make local HTTP requests. Because the SQL function must return model answers, its DuckDB worker waits for those answers, while the extension’s transport threads handle the socket I/O.
