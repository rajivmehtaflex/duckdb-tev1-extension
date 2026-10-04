#define DUCKDB_EXTENSION_MAIN

#include "tev1_extension.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	(void)loader;
}

void Tev1Extension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string Tev1Extension::Name() {
	return "ollama_systemone";
}

std::string Tev1Extension::Version() const {
#ifdef EXT_VERSION_OLLAMA_SYSTEMONE
	return EXT_VERSION_OLLAMA_SYSTEMONE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(ollama_systemone, loader) {
	duckdb::LoadInternal(loader);
}

}
