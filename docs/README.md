# DuckDB Tev1 SystemOne

`ollama_systemone(state VARCHAR, config JSON) -> JSON` evaluates the configured
questions for each non-NULL row through Ollama's Tev1 SystemOne endpoint.

## User documentation

- [Install and configure](INSTALL.md): DuckDB v1.5.6 matching, Ollama/model
  setup, local and published extension loading, endpoints, containers, data
  sent to Ollama, and distribution trust.
- [SQL and Python examples](EXAMPLES.md): function inputs, JSON response
  extraction, and runtime limits.
- [T7 release evidence](release-evidence/2026-10-05-t7-acceptance.md): live
  compatibility, benchmark results, and platform acceptance status.

The intended public release route is DuckDB Community Extensions. Submission
and publication are pending; until then, use a local build. See the install
guide for the documented custom public GitHub Pages alternative.
