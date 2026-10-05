#include "systemone/response.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using duckdb::systemone::QuestionSpec;
using duckdb::systemone::QuestionType;
using duckdb::systemone::ValidatedQuestions;
using duckdb::systemone::ValidateResponse;
using nlohmann::ordered_json;

std::filesystem::path FixturePath(const std::string &name) {
	return std::filesystem::path(__FILE__).parent_path().parent_path() / "mock" / "fixtures" / "response" /
	       name;
}

std::string ReadFixture(const std::string &name) {
	std::ifstream input(FixturePath(name));
	if (!input) {
		throw std::runtime_error("could not open response fixture: " + name);
	}
	std::ostringstream contents;
	contents << input.rdbuf();
	return contents.str();
}

ValidatedQuestions MixedQuestions() {
	auto choice_criteria = ordered_json::object();
	choice_criteria["billing"] = "Billing";
	choice_criteria["tech"] = nullptr;
	choice_criteria["other"] = "Other";
	auto score_criteria = ordered_json::array({"Low", "Medium", "High"});
	return {{QuestionSpec{"is_urgent", QuestionType::NOUL, "Is this urgent?", std::nullopt},
	         QuestionSpec{"department", QuestionType::CHOICE, "Choose a department.", choice_criteria},
	         QuestionSpec{"severity", QuestionType::SCORE, "Rate the severity.", score_criteria}}};
}

ordered_json ValidResponse() {
	return ordered_json::parse(ReadFixture("valid_mixed.json"));
}

void Check(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void ExpectInvalid(const std::string &description, const std::string &response_json, const std::string &category,
                   const ValidatedQuestions &questions = MixedQuestions()) {
	try {
		(void)ValidateResponse(response_json, questions);
	} catch (const std::invalid_argument &error) {
		const std::string message = error.what();
		if (message.find(category) == std::string::npos) {
			throw std::runtime_error(description + " failed with an unexpected category: " + message);
		}
		return;
	} catch (const std::exception &error) {
		throw std::runtime_error(description + " threw an unexpected exception: " + error.what());
	}
	throw std::runtime_error(description + " was accepted");
}

std::string Dump(const ordered_json &response) {
	return response.dump();
}

void TestValidResponsePreservesAllFieldsAndNumericValues() {
	const std::string fixture = ReadFixture("valid_mixed.json");
	const ordered_json expected = ordered_json::parse(fixture);
	const ordered_json actual = ValidateResponse(fixture, MixedQuestions());
	Check(actual.dump() == expected.dump(), "complete response JSON was not preserved");
	Check(actual.at("answers").at("severity").at("score").is_number_float(),
	      "fractional score was not preserved as a number");
	Check(actual.at("answers").at("is_urgent").at("server_metadata").at("version") == 1,
	      "additional answer fields were not preserved");
	Check(actual.at("trace_id") == "systemone-test-001", "additional response fields were not preserved");
}

void TestMalformedJsonAndEnvelopeErrorsAreRejected() {
	ExpectInvalid("malformed response fixture", ReadFixture("malformed.json"), "malformed JSON");
	ExpectInvalid("array response envelope", "[]", "envelope");
	ExpectInvalid("missing model", Dump([](ordered_json value) { value.erase("model"); return value; }(ValidResponse())),
	              "model");
	ExpectInvalid("non-string model", Dump([](ordered_json value) { value["model"] = 7; return value; }(ValidResponse())),
	              "model");
	ExpectInvalid("empty model", Dump([](ordered_json value) { value["model"] = ""; return value; }(ValidResponse())),
	              "model");
	ExpectInvalid("missing answers", Dump([](ordered_json value) { value.erase("answers"); return value; }(ValidResponse())),
	              "answers");
	ExpectInvalid("non-object answers", Dump([](ordered_json value) { value["answers"] = ordered_json::array(); return value; }(ValidResponse())),
	              "answers");
	ExpectInvalid("missing usage", Dump([](ordered_json value) { value.erase("usage"); return value; }(ValidResponse())),
	              "usage");
	ExpectInvalid("non-object usage", Dump([](ordered_json value) { value["usage"] = nullptr; return value; }(ValidResponse())),
	              "usage");
}

void TestUsageCountsMustBePresentNonnegativeIntegers() {
	for (const auto &mutation : std::vector<std::pair<std::string, std::function<void(ordered_json &)>>> {
	         {"missing input_tokens", [](ordered_json &value) { value["usage"].erase("input_tokens"); }},
	         {"missing output_tokens", [](ordered_json &value) { value["usage"].erase("output_tokens"); }},
	         {"negative input_tokens", [](ordered_json &value) { value["usage"]["input_tokens"] = -1; }},
	         {"fractional output_tokens", [](ordered_json &value) { value["usage"]["output_tokens"] = 2.5; }},
	         {"string input_tokens", [](ordered_json &value) { value["usage"]["input_tokens"] = "123"; }}}) {
		auto invalid = ValidResponse();
		mutation.second(invalid);
		ExpectInvalid(mutation.first, Dump(invalid), "usage");
	}
	for (auto count : {uint64_t{0}, uint64_t{1}}) {
		auto boundary = ValidResponse();
		boundary["usage"]["input_tokens"] = count;
		boundary["usage"]["output_tokens"] = count;
		ValidateResponse(Dump(boundary), MixedQuestions());
	}
}

void TestAnswersMustExactlyMatchConfiguredQuestionNames() {
	auto missing = ValidResponse();
	missing["answers"].erase("department");
	ExpectInvalid("missing configured answer", Dump(missing), "answers");
	auto extra = ValidResponse();
	extra["answers"]["unexpected"] = ordered_json::object();
	ExpectInvalid("unexpected answer name", Dump(extra), "answers");
	auto non_object = ValidResponse();
	non_object["answers"]["department"] = "tech";
	ExpectInvalid("non-object answer", Dump(non_object), "answer");
	auto missing_type = ValidResponse();
	missing_type["answers"]["is_urgent"].erase("type");
	ExpectInvalid("missing answer type", Dump(missing_type), "type");
	auto wrong_type = ValidResponse();
	wrong_type["answers"]["is_urgent"]["type"] = "choice";
	ExpectInvalid("wrong answer type", Dump(wrong_type), "type");
}

void TestNoulRequiresANumericProbabilityInTheClosedUnitInterval() {
	for (const auto &mutation : std::vector<std::pair<std::string, std::function<void(ordered_json &)>>> {
	         {"missing noul", [](ordered_json &value) { value["answers"]["is_urgent"].erase("noul"); }},
	         {"boolean noul", [](ordered_json &value) { value["answers"]["is_urgent"]["noul"] = true; }},
	         {"string noul", [](ordered_json &value) { value["answers"]["is_urgent"]["noul"] = "0.5"; }},
	         {"negative noul", [](ordered_json &value) { value["answers"]["is_urgent"]["noul"] = -0.01; }},
	         {"noul above one", [](ordered_json &value) { value["answers"]["is_urgent"]["noul"] = 1.01; }}}) {
		auto invalid = ValidResponse();
		mutation.second(invalid);
		ExpectInvalid(mutation.first, Dump(invalid), "noul");
	}
	ExpectInvalid("non-finite noul", R"({"model":"m","answers":{"is_urgent":{"type":"noul","noul":1e400}},"usage":{"input_tokens":0,"output_tokens":0}})",
	              "malformed JSON");
	for (auto probability : {0.0, 1.0}) {
		auto boundary = ValidResponse();
		boundary["answers"]["is_urgent"]["noul"] = probability;
		ValidateResponse(Dump(boundary), MixedQuestions());
	}
}

void TestChoiceRequiresConfiguredLabelsProbabilitiesAndConfidence() {
	for (const auto &mutation : std::vector<std::pair<std::string, std::function<void(ordered_json &)>>> {
	         {"missing choice", [](ordered_json &value) { value["answers"]["department"].erase("choice"); }},
	         {"unexpected choice label", [](ordered_json &value) { value["answers"]["department"]["choice"] = "unknown"; }},
	         {"non-string choice label", [](ordered_json &value) { value["answers"]["department"]["choice"] = 1; }},
	         {"missing probability key", [](ordered_json &value) { value["answers"]["department"]["probabilities"].erase("tech"); }},
	         {"extra probability key", [](ordered_json &value) { value["answers"]["department"]["probabilities"]["extra"] = 0.0; }},
	         {"non-object probabilities", [](ordered_json &value) { value["answers"]["department"]["probabilities"] = ordered_json::array(); }},
	         {"string probability", [](ordered_json &value) { value["answers"]["department"]["probabilities"]["tech"] = "0.5"; }},
	         {"negative probability", [](ordered_json &value) { value["answers"]["department"]["probabilities"]["tech"] = -0.01; }},
	         {"probability above one", [](ordered_json &value) { value["answers"]["department"]["probabilities"]["tech"] = 1.01; }},
	         {"missing confidence", [](ordered_json &value) { value["answers"]["department"].erase("confidence"); }},
	         {"string confidence", [](ordered_json &value) { value["answers"]["department"]["confidence"] = "0.5"; }},
	         {"negative confidence", [](ordered_json &value) { value["answers"]["department"]["confidence"] = -0.01; }},
	         {"confidence above one", [](ordered_json &value) { value["answers"]["department"]["confidence"] = 1.01; }}}) {
		auto invalid = ValidResponse();
		mutation.second(invalid);
		const std::string category = mutation.first.find("probabilit") != std::string::npos ? "probabilities" :
		                             mutation.first.find("confidence") != std::string::npos ? "confidence" :
		                             "choice";
		ExpectInvalid(mutation.first, Dump(invalid), category);
	}
	ExpectInvalid("non-finite choice probability",
	              R"({"model":"m","answers":{"department":{"type":"choice","choice":"tech","probabilities":{"billing":0,"tech":1e400,"other":0},"confidence":0.5}},"usage":{"input_tokens":0,"output_tokens":0}})",
	              "malformed JSON");
	for (auto probability : {0.0, 1.0}) {
		auto boundary = ValidResponse();
		boundary["answers"]["department"]["probabilities"]["tech"] = probability;
		boundary["answers"]["department"]["confidence"] = probability;
		ValidateResponse(Dump(boundary), MixedQuestions());
	}
}

void TestScoreRequiresBoundedNumericScoreAndMatchingLegendAndProbabilities() {
	for (const auto &mutation : std::vector<std::pair<std::string, std::function<void(ordered_json &)>>> {
	         {"missing score", [](ordered_json &value) { value["answers"]["severity"].erase("score"); }},
	         {"string score", [](ordered_json &value) { value["answers"]["severity"]["score"] = "1.5"; }},
	         {"negative score", [](ordered_json &value) { value["answers"]["severity"]["score"] = -0.01; }},
	         {"score above last criterion", [](ordered_json &value) { value["answers"]["severity"]["score"] = 2.01; }},
	         {"missing score probability", [](ordered_json &value) { value["answers"]["severity"]["probabilities"].erase("1"); }},
	         {"extra score probability", [](ordered_json &value) { value["answers"]["severity"]["probabilities"]["3"] = 0.0; }},
	         {"score probability out of range", [](ordered_json &value) { value["answers"]["severity"]["probabilities"]["1"] = 1.1; }},
	         {"missing legend", [](ordered_json &value) { value["answers"]["severity"].erase("legend"); }},
	         {"non-object legend", [](ordered_json &value) { value["answers"]["severity"]["legend"] = ordered_json::array(); }},
	         {"missing legend key", [](ordered_json &value) { value["answers"]["severity"]["legend"].erase("1"); }},
	         {"extra legend key", [](ordered_json &value) { value["answers"]["severity"]["legend"]["3"] = "Extra"; }},
	         {"non-string legend value", [](ordered_json &value) { value["answers"]["severity"]["legend"]["1"] = 1; }},
	         {"mismatched legend description", [](ordered_json &value) { value["answers"]["severity"]["legend"]["1"] = "Wrong"; }},
	         {"missing score confidence", [](ordered_json &value) { value["answers"]["severity"].erase("confidence"); }},
	         {"score confidence out of range", [](ordered_json &value) { value["answers"]["severity"]["confidence"] = 1.01; }}}) {
		auto invalid = ValidResponse();
		mutation.second(invalid);
		const std::string category = mutation.first.find("legend") != std::string::npos ? "legend" :
		                             mutation.first.find("probabilit") != std::string::npos ? "probabilities" :
		                             mutation.first.find("confidence") != std::string::npos ? "confidence" :
		                             "score";
		ExpectInvalid(mutation.first, Dump(invalid), category);
	}
	ExpectInvalid("non-finite score",
	              R"({"model":"m","answers":{"severity":{"type":"score","score":1e400,"legend":{"0":"Low","1":"Medium","2":"High"},"probabilities":{"0":0,"1":1,"2":0},"confidence":0.5}},"usage":{"input_tokens":0,"output_tokens":0}})",
	              "malformed JSON");
	for (auto score : {0.0, 2.0}) {
		auto boundary = ValidResponse();
		boundary["answers"]["severity"]["score"] = score;
		ValidateResponse(Dump(boundary), MixedQuestions());
	}
}

void TestCorruptQuestionMetadataIsRejected() {
	auto invalid_noul = MixedQuestions();
	invalid_noul.ordered[0].criteria = ordered_json::object({{"yes", "Yes"}});
	ExpectInvalid("invalid noul criterion key", Dump(ValidResponse()), "metadata", invalid_noul);
	auto non_string_noul = MixedQuestions();
	non_string_noul.ordered[0].criteria = ordered_json::object({{"true", 1}});
	ExpectInvalid("non-string noul criterion description", Dump(ValidResponse()), "metadata", non_string_noul);
}

} // namespace

int main() {
	const std::vector<std::pair<std::string, std::function<void()>>> tests = {
	    {"valid response preservation", TestValidResponsePreservesAllFieldsAndNumericValues},
	    {"malformed JSON and envelope", TestMalformedJsonAndEnvelopeErrorsAreRejected},
	    {"usage count validation", TestUsageCountsMustBePresentNonnegativeIntegers},
	    {"answer name and type validation", TestAnswersMustExactlyMatchConfiguredQuestionNames},
	    {"noul probability validation", TestNoulRequiresANumericProbabilityInTheClosedUnitInterval},
	    {"choice validation", TestChoiceRequiresConfiguredLabelsProbabilitiesAndConfidence},
	    {"score validation", TestScoreRequiresBoundedNumericScoreAndMatchingLegendAndProbabilities},
	    {"validated question metadata", TestCorruptQuestionMetadataIsRejected},
	};
	std::size_t failures = 0;
	for (const auto &test : tests) {
		try {
			test.second();
			std::cout << "PASS: " << test.first << '\n';
		} catch (const std::exception &error) {
			++failures;
			std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
		}
	}
	return failures == 0 ? 0 : 1;
}
