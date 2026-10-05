#include "systemone/request.hpp"

#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace duckdb {
namespace systemone {
namespace {

using OrderedJson = nlohmann::ordered_json;

class DuplicateKeySax final : public nlohmann::json_sax<OrderedJson> {
public:
	using string_t = typename nlohmann::json_sax<OrderedJson>::string_t;
	using binary_t = typename nlohmann::json_sax<OrderedJson>::binary_t;
	using number_integer_t = typename nlohmann::json_sax<OrderedJson>::number_integer_t;
	using number_unsigned_t = typename nlohmann::json_sax<OrderedJson>::number_unsigned_t;
	using number_float_t = typename nlohmann::json_sax<OrderedJson>::number_float_t;

	bool null() override { return true; }
	bool boolean(bool) override { return true; }
	bool number_integer(number_integer_t) override { return true; }
	bool number_unsigned(number_unsigned_t) override { return true; }
	bool number_float(number_float_t, const string_t &) override { return true; }
	bool string(string_t &) override { return true; }
	bool binary(binary_t &) override { return true; }
	bool start_object(std::size_t) override {
		object_keys.emplace_back();
		return true;
	}
	bool key(string_t &value) override {
		if (object_keys.empty()) {
			return false;
		}
		if (!object_keys.back().insert(value).second) {
			has_duplicate_key = true;
			return false;
		}
		return true;
	}
	bool end_object() override {
		if (object_keys.empty()) {
			return false;
		}
		object_keys.pop_back();
		return true;
	}
	bool start_array(std::size_t) override { return true; }
	bool end_array() override { return true; }
	bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override {
		return false;
	}

	bool has_duplicate_key = false;

private:
	std::vector<std::unordered_set<std::string>> object_keys;
};

[[noreturn]] void Invalid(const std::string &message) {
	throw std::invalid_argument("ollama_systemone request: " + message);
}

OrderedJson ParseConfigJson(std::string_view config_json) {
	DuplicateKeySax sax;
	const bool valid = OrderedJson::sax_parse(config_json.begin(), config_json.end(), &sax);
	if (sax.has_duplicate_key) {
		Invalid("duplicate JSON object key");
	}
	if (!valid) {
		Invalid("malformed config JSON");
	}
	try {
		return OrderedJson::parse(config_json.begin(), config_json.end());
	} catch (const nlohmann::json::exception &) {
		Invalid("malformed config JSON");
	}
}

bool ContainsOnly(const OrderedJson &object, const std::set<std::string> &allowed) {
	for (auto it = object.begin(); it != object.end(); ++it) {
		if (allowed.find(it.key()) == allowed.end()) {
			return false;
		}
	}
	return true;
}

bool IsBlank(std::string_view text) {
	for (const unsigned char byte : text) {
		if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\v' && byte != '\f' && byte != '\r') {
			return false;
		}
	}
	return true;
}

QuestionType ParseQuestionType(const OrderedJson &type_value) {
	if (!type_value.is_string()) {
		Invalid("question type must be a string");
	}
	const auto type = type_value.get<std::string>();
	if (type == "noul") {
		return QuestionType::NOUL;
	}
	if (type == "choice") {
		return QuestionType::CHOICE;
	}
	if (type == "score") {
		return QuestionType::SCORE;
	}
	Invalid("unsupported question type");
}

void ValidateNoulCriteria(const OrderedJson &criteria) {
	if (!criteria.is_object()) {
		Invalid("noul criteria must be an object");
	}
	for (auto it = criteria.begin(); it != criteria.end(); ++it) {
		if (it.key() != "true" && it.key() != "false") {
			Invalid("noul criteria keys must be true or false");
		}
		if (!it.value().is_string()) {
			Invalid("noul criteria descriptions must be strings");
		}
	}
}

void ValidateChoiceCriteria(const OrderedJson &criteria) {
	if (!criteria.is_object()) {
		Invalid("choice criteria must be an object");
	}
	if (criteria.size() < 2 || criteria.size() > 26) {
		Invalid("choice criteria must contain between 2 and 26 options");
	}
	for (auto it = criteria.begin(); it != criteria.end(); ++it) {
		if (IsBlank(it.key())) {
			Invalid("choice option names must contain non-whitespace text");
		}
		if (!it.value().is_string() && !it.value().is_null()) {
			Invalid("choice option descriptions must be strings or null");
		}
	}
}

void ValidateScoreCriteria(const OrderedJson &criteria) {
	if (!criteria.is_array()) {
		Invalid("score criteria must be an array");
	}
	if (criteria.size() < 2 || criteria.size() > 26) {
		Invalid("score criteria must contain between 2 and 26 levels");
	}
	for (const auto &value : criteria) {
		if (!value.is_string()) {
			Invalid("score criteria levels must be strings");
		}
	}
}

ValidatedQuestions ValidateQuestions(const OrderedJson &config, const OrderedJson *&questions_json) {
	if (!config.is_object() || config.size() != 1 || !config.contains("questions")) {
		Invalid("config must contain exactly the questions key");
	}
	questions_json = &config.at("questions");
	if (!questions_json->is_object()) {
		Invalid("questions must be an object");
	}
	if (questions_json->size() < 1 || questions_json->size() > 64) {
		Invalid("questions must contain between 1 and 64 named fields");
	}

	ValidatedQuestions validated;
	validated.ordered.reserve(questions_json->size());
	static const std::set<std::string> allowed_fields = {"type", "instructions", "criteria"};
	for (auto question_it = questions_json->begin(); question_it != questions_json->end(); ++question_it) {
		const auto &name = question_it.key();
		const auto &question = question_it.value();
		if (IsBlank(name)) {
			Invalid("question names must contain non-whitespace text");
		}
		if (!question.is_object() || !ContainsOnly(question, allowed_fields)) {
			Invalid("each question must be an object with only type, instructions, and criteria");
		}
		if (!question.contains("type") || !question.contains("instructions")) {
			Invalid("each question requires type and instructions");
		}
		const auto type = ParseQuestionType(question.at("type"));
		const auto &instructions_value = question.at("instructions");
		if (!instructions_value.is_string()) {
			Invalid("question instructions must be strings");
		}
		auto instructions = instructions_value.get<std::string>();
		if (instructions.empty()) {
			Invalid("question instructions must be nonempty");
		}

		std::optional<OrderedJson> criteria;
		if (question.contains("criteria")) {
			criteria = question.at("criteria");
		}
		switch (type) {
		case QuestionType::NOUL:
			if (criteria) {
				ValidateNoulCriteria(*criteria);
			}
			break;
		case QuestionType::CHOICE:
			if (!criteria) {
				Invalid("choice questions require criteria");
			}
			ValidateChoiceCriteria(*criteria);
			break;
		case QuestionType::SCORE:
			if (!criteria) {
				Invalid("score questions require criteria");
			}
			ValidateScoreCriteria(*criteria);
			break;
		}
		validated.ordered.push_back({name, type, std::move(instructions), std::move(criteria)});
	}
	return validated;
}

void AppendSourceText(std::string &destination, bool &first, std::string_view text) {
	if (!first) {
		destination.push_back('\n');
	}
	first = false;
	if (!text.empty()) {
		destination.append(text.data(), text.size());
	}
}

std::string BuildNormalizedSource(std::string_view state, const OrderedJson &questions) {
	std::string source;
	bool first = true;
	AppendSourceText(source, first, state);
	for (auto question = questions.begin(); question != questions.end(); ++question) {
		AppendSourceText(source, first, question.key());
		AppendSourceText(source, first, question.value().at("instructions").get_ref<const std::string &>());
		if (!question.value().contains("criteria")) {
			continue;
		}
		const auto &criteria = question.value().at("criteria");
		if (question.value().at("type") == "choice") {
			for (auto option = criteria.begin(); option != criteria.end(); ++option) {
				AppendSourceText(source, first, option.key());
				if (option.value().is_string()) {
					AppendSourceText(source, first, option.value().get_ref<const std::string &>());
				}
			}
		} else if (question.value().at("type") == "score") {
			for (const auto &level : criteria) {
				AppendSourceText(source, first, level.get_ref<const std::string &>());
			}
		} else {
			for (auto value = criteria.begin(); value != criteria.end(); ++value) {
				AppendSourceText(source, first, value.value().get_ref<const std::string &>());
			}
		}
	}
	return source;
}

} // namespace

ValidatedConfig ParseConfig(std::string_view config_json) {
	const auto config = ParseConfigJson(config_json);
	const OrderedJson *questions_json = nullptr;
	auto validated = ValidateQuestions(config, questions_json);
	return {*questions_json, std::move(validated)};
}

OwnedRequest BuildRequest(std::string_view state, const ValidatedConfig &config, const SettingsSnapshot &settings) {
	if (state.empty()) {
		Invalid("state must be nonempty");
	}

	auto normalized_source = BuildNormalizedSource(state, config.questions_json);
	if (normalized_source.size() > settings.max_source_bytes) {
		Invalid("normalized source exceeds ollama_max_source_bytes");
	}

	OrderedJson payload = OrderedJson::object();
	payload["model"] = "tev1:0.8b";
	payload["state"] = std::string(state);
	payload["questions"] = config.questions_json;
	if (settings.keep_alive) {
		payload["keep_alive"] = *settings.keep_alive;
	}
	std::string body;
	try {
		body = payload.dump();
	} catch (const nlohmann::json::exception &) {
		Invalid("request contains invalid UTF-8 text");
	}
	if (body.size() > settings.max_request_bytes) {
		Invalid("serialized request exceeds ollama_max_request_bytes");
	}
	return {std::move(body), std::move(normalized_source), config.questions};
}

OwnedRequest BuildRequest(std::string_view state, std::string_view config_json, const SettingsSnapshot &settings) {
	return BuildRequest(state, ParseConfig(config_json), settings);
}

} // namespace systemone
} // namespace duckdb
