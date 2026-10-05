#include "systemone/config.hpp"
#include "systemone/registration.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace {

void OllamaSystemOneFunction(DataChunk &, ExpressionState &, Vector &) {
	throw NotImplementedException("ollama_systemone request execution is not implemented yet");
}

} // namespace

void RegisterSystemOne(ExtensionLoader &loader) {
	systemone::RegisterSettings(loader);

	ScalarFunction function("ollama_systemone", {LogicalType::VARCHAR, LogicalType::JSON()}, LogicalType::JSON(),
	                        OllamaSystemOneFunction);
	function.SetVolatile();
	function.SetFallible();
	loader.RegisterFunction(function);
}

} // namespace duckdb
