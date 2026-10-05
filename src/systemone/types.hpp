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

nlohmann::ordered_json ValidateResponse(std::string_view response_json, const ValidatedQuestions &questions);

} // namespace systemone
} // namespace duckdb
