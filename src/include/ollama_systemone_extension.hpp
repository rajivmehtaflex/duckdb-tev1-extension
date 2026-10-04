#pragma once

#include "tev1_extension.hpp"

namespace duckdb {

// DuckDB v1.5.6 generates its statically linked loader from the extension
// name and expects this canonical class name.
using OllamaSystemoneExtension = Tev1Extension;

} // namespace duckdb
