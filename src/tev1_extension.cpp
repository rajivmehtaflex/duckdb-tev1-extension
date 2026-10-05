#define DUCKDB_EXTENSION_MAIN

#include "systemone/registration.hpp"
#include "tev1_extension.hpp"

#include "duckdb/common/exception.hpp"

#include <curl/curl.h>

#include <mutex>

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	static std::once_flag curl_init_flag;
	static CURLcode curl_init_result = CURLE_FAILED_INIT;
	std::call_once(curl_init_flag, []() { curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT); });
	if (curl_init_result != CURLE_OK) {
		throw IOException("Failed to initialize libcurl for ollama_systemone");
	}
	RegisterSystemOne(loader);
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
