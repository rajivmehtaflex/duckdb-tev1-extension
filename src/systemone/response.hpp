#pragma once

#include "systemone/types.hpp"

namespace duckdb {
namespace systemone {

// Parse an Ollama SystemOne response, validate its configured answers, and
// return the complete ordered response object. Throws std::invalid_argument
// with a concise response category when parsing or validation fails.
nlohmann::ordered_json ValidateResponse(std::string_view response_json, const ValidatedQuestions &questions);

} // namespace systemone
} // namespace duckdb
