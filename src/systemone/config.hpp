#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace duckdb {
class ClientContext;
class ExtensionLoader;

namespace systemone {

struct SettingsSnapshot {
	SettingsSnapshot(std::string endpoint_p, uint64_t connect_timeout_ms_p, uint64_t request_timeout_ms_p,
	                 uint64_t max_inflight_p, uint64_t queue_capacity_p, uint64_t max_request_bytes_p,
	                 uint64_t max_response_bytes_p, uint64_t max_source_bytes_p, uint64_t retry_count_p,
	                 std::optional<std::string> keep_alive_p)
	    : endpoint(std::move(endpoint_p)), connect_timeout_ms(connect_timeout_ms_p),
	      request_timeout_ms(request_timeout_ms_p), max_inflight(max_inflight_p), queue_capacity(queue_capacity_p),
	      max_request_bytes(max_request_bytes_p), max_response_bytes(max_response_bytes_p),
	      max_source_bytes(max_source_bytes_p), retry_count(retry_count_p), keep_alive(std::move(keep_alive_p)) {
	}

	const std::string endpoint;
	const uint64_t connect_timeout_ms;
	const uint64_t request_timeout_ms;
	const uint64_t max_inflight;
	const uint64_t queue_capacity;
	const uint64_t max_request_bytes;
	const uint64_t max_response_bytes;
	const uint64_t max_source_bytes;
	const uint64_t retry_count;
	const std::optional<std::string> keep_alive;
};

void RegisterSettings(ExtensionLoader &loader);
SettingsSnapshot ReadSettingsSnapshot(ClientContext &context);

} // namespace systemone
} // namespace duckdb
