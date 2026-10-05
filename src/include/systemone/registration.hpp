#pragma once

namespace duckdb {
class ExtensionLoader;

namespace systemone {
void RegisterScalarFunction(ExtensionLoader &loader);
} // namespace systemone

void RegisterSystemOne(ExtensionLoader &loader);
} // namespace duckdb
