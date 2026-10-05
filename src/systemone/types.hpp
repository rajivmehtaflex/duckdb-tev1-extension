#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace duckdb {
namespace systemone {

enum class QuestionType : uint8_t {
	NOUL,
	CHOICE,
	SCORE,
};

struct QuestionSpec {
	std::string name;
	QuestionType type;
	std::string instructions;
	std::optional<nlohmann::ordered_json> criteria;
};

struct ValidatedQuestions {
	std::vector<QuestionSpec> ordered;
};

// Parsed once per distinct config value within a DuckDB input chunk. Keep the
// ordered JSON representation for faithful request serialization and the
// validated metadata for response checks.
struct ValidatedConfig {
	nlohmann::ordered_json questions_json;
	ValidatedQuestions questions;
};

nlohmann::ordered_json ValidateResponse(std::string_view response_json, const ValidatedQuestions &questions);

} // namespace systemone
} // namespace duckdb
