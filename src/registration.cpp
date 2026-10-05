#include "systemone/config.hpp"
#include "systemone/registration.hpp"

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
void RegisterSystemOne(ExtensionLoader &loader) {
	systemone::RegisterSettings(loader);
	systemone::RegisterScalarFunction(loader);
}

} // namespace duckdb
