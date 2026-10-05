#pragma once

#include <string>
#include <string_view>

#include "systemone/config.hpp"
#include "systemone/types.hpp"

namespace duckdb {
namespace systemone {

struct OwnedRequest {
	std::string body;
	std::string normalized_source;
	ValidatedQuestions questions;
};

OwnedRequest BuildRequest(std::string_view state, std::string_view config_json, const SettingsSnapshot &settings);

} // namespace systemone
} // namespace duckdb
