#include "systemone/response.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace duckdb {
namespace systemone {
namespace {

using Json = nlohmann::ordered_json;

[[noreturn]] void InvalidResponse(const std::string &reason) {
	throw std::invalid_argument("response: " + reason);
}

const Json *RequireMember(const Json &object, const std::string &name, const std::string &error) {
	if (!object.contains(name)) {
		InvalidResponse(error);
	}
	return &object.at(name);
}

bool IsNonnegativeInteger(const Json &value) {
	if (!value.is_number_integer()) {
		return false;
	}
	if (value.is_number_unsigned()) {
		return true;
	}
	return value.get<Json::number_integer_t>() >= 0;
}

void ValidateProbability(const Json &value, const std::string &field_category) {
	if (!value.is_number()) {
		InvalidResponse(field_category + " must be a number in [0,1]");
	}
	const double probability = value.get<double>();
	if (!std::isfinite(probability) || probability < 0.0 || probability > 1.0) {
		InvalidResponse(field_category + " must be a number in [0,1]");
	}
}

std::vector<std::string> ExpectedKeys(const QuestionSpec &question) {
	std::vector<std::string> keys;
	switch (question.type) {
	case QuestionType::NOUL:
		if (question.criteria.has_value()) {
			if (!question.criteria->is_object() || question.criteria->size() > 2) {
				InvalidResponse("validated question metadata is invalid");
			}
			for (auto it = question.criteria->begin(); it != question.criteria->end(); ++it) {
				if ((it.key() != "true" && it.key() != "false") || !it.value().is_string()) {
					InvalidResponse("validated question metadata is invalid");
				}
			}
		}
		return keys;
	case QuestionType::CHOICE:
		if (!question.criteria.has_value() || !question.criteria->is_object() || question.criteria->size() < 2 ||
		    question.criteria->size() > 26) {
			InvalidResponse("validated question metadata is invalid");
		}
		for (auto it = question.criteria->begin(); it != question.criteria->end(); ++it) {
			if (it.key().empty() || (!it.value().is_string() && !it.value().is_null())) {
				InvalidResponse("validated question metadata is invalid");
			}
			keys.push_back(it.key());
		}
		return keys;
	case QuestionType::SCORE:
		if (!question.criteria.has_value() || !question.criteria->is_array() || question.criteria->size() < 2 ||
		    question.criteria->size() > 26) {
			InvalidResponse("validated question metadata is invalid");
		}
		for (std::size_t index = 0; index < question.criteria->size(); ++index) {
			if (!question.criteria->at(index).is_string()) {
				InvalidResponse("validated question metadata is invalid");
			}
			keys.push_back(std::to_string(index));
		}
		return keys;
	}
	InvalidResponse("validated question metadata is invalid");
}

void ValidateExactKeys(const Json &object, const std::vector<std::string> &keys, const std::string &field_name,
                       const std::string &key_error) {
	if (!object.is_object()) {
		InvalidResponse(field_name + " must be an object");
	}
	if (object.size() != keys.size()) {
		InvalidResponse(key_error);
	}
	for (const auto &key : keys) {
		if (!object.contains(key)) {
			InvalidResponse(key_error);
		}
	}
}

void ValidateProbabilities(const Json &probabilities, const std::vector<std::string> &keys) {
	ValidateExactKeys(probabilities, keys, "probabilities", "probabilities keys do not match criteria");
	for (const auto &key : keys) {
		ValidateProbability(probabilities.at(key), "probabilities");
	}
}

void ValidateConfidence(const Json &answer) {
	const auto *confidence = RequireMember(answer, "confidence", "confidence is required");
	ValidateProbability(*confidence, "confidence");
}

void ValidateAnswer(const Json &answer, const QuestionSpec &question) {
	if (!answer.is_object()) {
		InvalidResponse("answer must be an object");
	}
	const auto *type = RequireMember(answer, "type", "answer type is required");
	if (!type->is_string()) {
		InvalidResponse("answer type must be a string");
	}

	std::string expected_type;
	switch (question.type) {
	case QuestionType::NOUL:
		expected_type = "noul";
		break;
	case QuestionType::CHOICE:
		expected_type = "choice";
		break;
	case QuestionType::SCORE:
		expected_type = "score";
		break;
	default:
		InvalidResponse("validated question metadata is invalid");
	}
	if (type->get_ref<const std::string &>() != expected_type) {
		InvalidResponse("answer type does not match question type");
	}

	const auto keys = ExpectedKeys(question);
	switch (question.type) {
	case QuestionType::NOUL: {
		const auto *noul = RequireMember(answer, "noul", "noul probability is required");
		ValidateProbability(*noul, "noul probability");
		break;
	}
	case QuestionType::CHOICE: {
		const auto *choice = RequireMember(answer, "choice", "choice is required");
		if (!choice->is_string()) {
			InvalidResponse("choice must be a configured label");
		}
		const auto &choice_label = choice->get_ref<const std::string &>();
		if (std::find(keys.begin(), keys.end(), choice_label) == keys.end()) {
			InvalidResponse("choice must be a configured label");
		}
		const auto *probabilities = RequireMember(answer, "probabilities", "probabilities are required");
		ValidateProbabilities(*probabilities, keys);
		ValidateConfidence(answer);
		break;
	}
	case QuestionType::SCORE: {
		const auto *score = RequireMember(answer, "score", "score is required");
		if (!score->is_number()) {
			InvalidResponse("score must be a number within the configured range");
		}
		const double numeric_score = score->get<double>();
		const double maximum_score = static_cast<double>(keys.size() - 1);
		if (!std::isfinite(numeric_score) || numeric_score < 0.0 || numeric_score > maximum_score) {
			InvalidResponse("score must be a number within the configured range");
		}

		const auto *probabilities = RequireMember(answer, "probabilities", "probabilities are required");
		ValidateProbabilities(*probabilities, keys);
		const auto *legend = RequireMember(answer, "legend", "legend is required");
		ValidateExactKeys(*legend, keys, "legend", "legend keys do not match criteria");
		for (std::size_t index = 0; index < keys.size(); ++index) {
			const auto &description = legend->at(keys[index]);
			if (!description.is_string() || description != question.criteria->at(index)) {
				InvalidResponse("legend descriptions do not match criteria");
			}
		}
		ValidateConfidence(answer);
		break;
	}
	}
}

void ValidateQuestions(const ValidatedQuestions &questions) {
	if (questions.ordered.empty() || questions.ordered.size() > 64) {
		InvalidResponse("validated question metadata is invalid");
	}
	std::set<std::string> names;
	for (const auto &question : questions.ordered) {
		if (question.name.empty() || !names.insert(question.name).second) {
			InvalidResponse("validated question metadata is invalid");
		}
		(void)ExpectedKeys(question);
	}
}

} // namespace

nlohmann::ordered_json ValidateResponse(std::string_view response_json, const ValidatedQuestions &questions) {
	Json response;
	try {
		response = Json::parse(response_json.begin(), response_json.end());
	} catch (const nlohmann::json::exception &) {
		InvalidResponse("malformed JSON");
	}
	if (!response.is_object()) {
		InvalidResponse("envelope must be an object");
	}

	const auto *model = RequireMember(response, "model", "model must be a nonempty string");
	if (!model->is_string() || model->get_ref<const std::string &>().empty()) {
		InvalidResponse("model must be a nonempty string");
	}
	const auto *answers = RequireMember(response, "answers", "answers must be an object");
	if (!answers->is_object()) {
		InvalidResponse("answers must be an object");
	}
	const auto *usage = RequireMember(response, "usage", "usage must be an object");
	if (!usage->is_object()) {
		InvalidResponse("usage must be an object");
	}
	const auto *input_tokens = RequireMember(*usage, "input_tokens", "usage counts must be nonnegative integers");
	const auto *output_tokens = RequireMember(*usage, "output_tokens", "usage counts must be nonnegative integers");
	if (!IsNonnegativeInteger(*input_tokens) || !IsNonnegativeInteger(*output_tokens)) {
		InvalidResponse("usage counts must be nonnegative integers");
	}

	ValidateQuestions(questions);
	if (answers->size() != questions.ordered.size()) {
		InvalidResponse("answers must contain exactly the configured questions");
	}
	for (const auto &question : questions.ordered) {
		const auto answer = answers->find(question.name);
		if (answer == answers->end()) {
			InvalidResponse("answers must contain exactly the configured questions");
		}
		ValidateAnswer(*answer, question);
	}
	return response;
}

} // namespace systemone
} // namespace duckdb
