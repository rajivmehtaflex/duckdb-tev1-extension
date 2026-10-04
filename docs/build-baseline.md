# Build baseline

- Official extension template: `duckdb/extension-template` commit `04949de122a8ed551a6545f54879793333c5a43c`.
- DuckDB submodule: `069cc9f9b5be802405797faecc284961b07c70ef` (v1.5.6).
- vcpkg registry and CI checkout: `ce613c41372b23b1f51333815feb3edd87ef8a8b`.
- vcpkg packages at this baseline: curl 8.13.0#1 and nlohmann-json 3.11.3#1.
- Extension version: 0.1.0.

Check out the pinned vcpkg commit, run `./bootstrap-vcpkg.sh`, then build using the template targets:

```sh
git submodule update --init --recursive
export VCPKG_TOOLCHAIN_PATH=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
export VCPKG_TARGET_TRIPLET=x64-linux
make GEN=ninja debug
make GEN=ninja release
make test
```

The debug artifact is `build/debug/extension/ollama_systemone/ollama_systemone.duckdb_extension` and the release artifact is `build/release/extension/ollama_systemone/ollama_systemone.duckdb_extension`. Load each with the CLI from the matching build directory (`build/debug/duckdb` or `build/release/duckdb`).
