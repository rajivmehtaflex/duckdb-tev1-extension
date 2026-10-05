#include "systemone/config.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>

namespace duckdb {
namespace systemone {
namespace {

constexpr const char *DEFAULT_ENDPOINT = "http://localhost:11434/v1/systemone";

bool IsValidPort(std::string_view port) {
	if (port.empty()) {
		return false;
	}
	uint32_t value = 0;
	for (const auto character : port) {
		if (!std::isdigit(static_cast<unsigned char>(character))) {
			return false;
		}
		value = value * 10 + static_cast<uint32_t>(character - '0');
		if (value > 65535) {
			return false;
		}
	}
	return true;
}

void ValidateEndpoint(std::string_view endpoint) {
	const auto separator = endpoint.find("://");
	if (separator == std::string_view::npos) {
		throw InvalidInputException("ollama_endpoint must be an absolute HTTP(S) URL with a host");
	}

	std::string scheme(endpoint.substr(0, separator));
	std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	if (scheme != "http" && scheme != "https") {
		throw InvalidInputException("ollama_endpoint must use the http or https scheme");
	}

	const auto authority_start = separator + 3;
	const auto authority_end = endpoint.find_first_of("/?#", authority_start);
	const auto authority_finish = authority_end == std::string_view::npos ? endpoint.size() : authority_end;
	const auto authority = endpoint.substr(authority_start, authority_finish - authority_start);
	if (authority.empty() ||
	    std::any_of(endpoint.begin(), endpoint.end(), [](unsigned char value) { return std::isspace(value); })) {
		throw InvalidInputException("ollama_endpoint must be an absolute HTTP(S) URL with a host");
	}
	if (endpoint.find('\0') != std::string_view::npos) {
		throw InvalidInputException("ollama_endpoint must be an absolute HTTP(S) URL with a host");
	}

	// Userinfo is optional. The host and optional port follow the final '@'.
	const auto at = authority.rfind('@');
	const auto host_port = authority.substr(at == std::string_view::npos ? 0 : at + 1);
	if (host_port.empty()) {
		throw InvalidInputException("ollama_endpoint must be an absolute HTTP(S) URL with a host");
	}

	if (host_port.front() == '[') {
		const auto closing_bracket = host_port.find(']');
		if (closing_bracket == std::string_view::npos || closing_bracket == 1) {
			throw InvalidInputException("ollama_endpoint must be an absolute HTTP(S) URL with a host");
		}
		const auto suffix = host_port.substr(closing_bracket + 1);
		if (!suffix.empty() && (suffix.front() != ':' || !IsValidPort(suffix.substr(1)))) {
			throw InvalidInputException("ollama_endpoint must have a valid host and optional port");
		}
	} else {
		const auto colon = host_port.find(':');
		const auto host = host_port.substr(0, colon);
		if (host.empty() || host.find_first_of("[]") != std::string_view::npos ||
		    (colon != std::string_view::npos &&
		     (host_port.find(':', colon + 1) != std::string_view::npos || !IsValidPort(host_port.substr(colon + 1))))) {
			throw InvalidInputException("ollama_endpoint must have a valid host and optional port");
		}
	}

	// Use libcurl's URL parser to reject malformed IP literals and other invalid authorities.
	const std::string endpoint_string(endpoint);
	CURLU *parsed_url = curl_url();
	if (parsed_url == nullptr) {
		throw InternalException("Could not allocate a URL parser for ollama_endpoint");
	}
	bool valid_url = curl_url_set(parsed_url, CURLUPART_URL, endpoint_string.c_str(), 0) == CURLUE_OK;
	char *parsed_host = nullptr;
	if (valid_url) {
		valid_url = curl_url_get(parsed_url, CURLUPART_HOST, &parsed_host, 0) == CURLUE_OK && parsed_host != nullptr &&
		            parsed_host[0] != '\0';
	}
	if (parsed_host != nullptr) {
		curl_free(parsed_host);
	}
	curl_url_cleanup(parsed_url);
	if (!valid_url) {
		throw InvalidInputException("ollama_endpoint must be a valid absolute HTTP(S) URL with a host");
	}
}

void ValidatePositive(ClientContext &, SetScope, Value &value, const char *setting_name) {
	if (value.IsNull()) {
		throw InvalidInputException("%s must not be NULL", setting_name);
	}
	if (value.GetValue<uint64_t>() == 0) {
		throw InvalidInputException("%s must be greater than zero", setting_name);
	}
}

void ValidateTimeout(ClientContext &context, SetScope scope, Value &value, const char *setting_name) {
	ValidatePositive(context, scope, value, setting_name);
	if (value.GetValue<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<long>::max())) {
		throw InvalidInputException("%s exceeds this platform's supported range", setting_name);
	}
}

void ValidateEndpointOption(ClientContext &, SetScope, Value &value) {
	if (value.IsNull()) {
		throw InvalidInputException("ollama_endpoint must not be NULL");
	}
	ValidateEndpoint(value.GetValue<std::string>());
}

void ValidateQueueCapacity(ClientContext &, SetScope, Value &value) {
	if (value.IsNull()) {
		throw InvalidInputException("ollama_queue_capacity must not be NULL");
	}
}

void ValidateConnectTimeout(ClientContext &context, SetScope scope, Value &value) {
	ValidateTimeout(context, scope, value, "ollama_connect_timeout_ms");
}

void ValidateRequestTimeout(ClientContext &context, SetScope scope, Value &value) {
	ValidateTimeout(context, scope, value, "ollama_request_timeout_ms");
}

void ValidateMaxInflight(ClientContext &context, SetScope scope, Value &value) {
	ValidatePositive(context, scope, value, "ollama_max_inflight");
}

void ValidateMaxRequestBytes(ClientContext &context, SetScope scope, Value &value) {
	ValidatePositive(context, scope, value, "ollama_max_request_bytes");
}

void ValidateMaxResponseBytes(ClientContext &context, SetScope scope, Value &value) {
	ValidatePositive(context, scope, value, "ollama_max_response_bytes");
}

void ValidateMaxSourceBytes(ClientContext &context, SetScope scope, Value &value) {
	ValidatePositive(context, scope, value, "ollama_max_source_bytes");
}

void ValidateRetryCount(ClientContext &, SetScope, Value &value) {
	if (value.IsNull()) {
		throw InvalidInputException("ollama_retry_count must not be NULL");
	}
	if (value.GetValue<uint64_t>() > 1) {
		throw InvalidInputException("ollama_retry_count must be 0 or 1");
	}
}

template <class T>
T ReadSetting(ClientContext &context, const char *setting_name) {
	Value value;
	if (!context.TryGetCurrentSetting(setting_name, value) || value.IsNull()) {
		throw InternalException("Required SystemOne setting '%s' has no value", setting_name);
	}
	return value.GetValue<T>();
}

uint64_t ReadUnsigned(ClientContext &context, const char *setting_name) {
	return ReadSetting<uint64_t>(context, setting_name);
}

void ValidateSnapshot(const SettingsSnapshot &settings) {
	ValidateEndpoint(settings.endpoint);
	if (settings.connect_timeout_ms == 0 || settings.request_timeout_ms == 0 || settings.max_inflight == 0 ||
	    settings.max_request_bytes == 0 || settings.max_response_bytes == 0 || settings.max_source_bytes == 0) {
		throw InvalidInputException("SystemOne timeouts, in-flight limit, and byte limits must be greater than zero");
	}
	if (settings.retry_count > 1) {
		throw InvalidInputException("ollama_retry_count must be 0 or 1");
	}
	if (settings.connect_timeout_ms > static_cast<uint64_t>(std::numeric_limits<long>::max()) ||
	    settings.request_timeout_ms > static_cast<uint64_t>(std::numeric_limits<long>::max())) {
		throw InvalidInputException("SystemOne timeout settings exceed this platform's supported range");
	}
}

} // namespace

void RegisterSettings(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	const auto session_scope = SetScope::SESSION;

	config.AddExtensionOption("ollama_endpoint", "Complete HTTP(S) endpoint for the Ollama SystemOne API",
	                          LogicalType::VARCHAR, Value(DEFAULT_ENDPOINT), ValidateEndpointOption, session_scope);
	config.AddExtensionOption("ollama_connect_timeout_ms", "Maximum time to establish an Ollama connection",
	                          LogicalType::UBIGINT, Value::UBIGINT(1000), ValidateConnectTimeout, session_scope);
	config.AddExtensionOption("ollama_request_timeout_ms", "Maximum duration of one SystemOne request",
	                          LogicalType::UBIGINT, Value::UBIGINT(120000), ValidateRequestTimeout, session_scope);
	config.AddExtensionOption("ollama_max_inflight", "Maximum active SystemOne requests per SQL expression",
	                          LogicalType::UBIGINT, Value::UBIGINT(8), ValidateMaxInflight, session_scope);
	config.AddExtensionOption("ollama_queue_capacity", "Maximum queued SystemOne requests per SQL expression",
	                          LogicalType::UBIGINT, Value::UBIGINT(16), ValidateQueueCapacity, session_scope);
	config.AddExtensionOption("ollama_max_request_bytes", "Maximum serialized SystemOne request body size",
	                          LogicalType::UBIGINT, Value::UBIGINT(65536), ValidateMaxRequestBytes, session_scope);
	config.AddExtensionOption("ollama_max_response_bytes", "Maximum buffered SystemOne response size",
	                          LogicalType::UBIGINT, Value::UBIGINT(1048576), ValidateMaxResponseBytes, session_scope);
	config.AddExtensionOption("ollama_max_source_bytes", "Maximum normalized SystemOne source text size",
	                          LogicalType::UBIGINT, Value::UBIGINT(1024), ValidateMaxSourceBytes, session_scope);
	config.AddExtensionOption("ollama_retry_count", "Maximum safe pre-upload connection retry count",
	                          LogicalType::UBIGINT, Value::UBIGINT(1), ValidateRetryCount, session_scope);
	config.AddExtensionOption("ollama_keep_alive", "Optional Ollama model residency duration",
	                          LogicalType::VARCHAR, Value(LogicalType::VARCHAR), nullptr, session_scope);
}

SettingsSnapshot ReadSettingsSnapshot(ClientContext &context) {
	auto endpoint = ReadSetting<std::string>(context, "ollama_endpoint");
	const auto connect_timeout_ms = ReadUnsigned(context, "ollama_connect_timeout_ms");
	const auto request_timeout_ms = ReadUnsigned(context, "ollama_request_timeout_ms");
	const auto max_inflight = ReadUnsigned(context, "ollama_max_inflight");
	const auto queue_capacity = ReadUnsigned(context, "ollama_queue_capacity");
	const auto max_request_bytes = ReadUnsigned(context, "ollama_max_request_bytes");
	const auto max_response_bytes = ReadUnsigned(context, "ollama_max_response_bytes");
	const auto max_source_bytes = ReadUnsigned(context, "ollama_max_source_bytes");
	const auto retry_count = ReadUnsigned(context, "ollama_retry_count");

	Value keep_alive;
	context.TryGetCurrentSetting("ollama_keep_alive", keep_alive);
	std::optional<std::string> keep_alive_value;
	if (!keep_alive.IsNull()) {
		keep_alive_value = keep_alive.GetValue<std::string>();
	}

	SettingsSnapshot result(std::move(endpoint), connect_timeout_ms, request_timeout_ms, max_inflight, queue_capacity,
	                        max_request_bytes, max_response_bytes, max_source_bytes, retry_count,
	                        std::move(keep_alive_value));
	ValidateSnapshot(result);
	return result;
}

} // namespace systemone
} // namespace duckdb
