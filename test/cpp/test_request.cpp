#include "systemone/request.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using duckdb::systemone::BuildRequest;
using duckdb::systemone::OwnedRequest;
using duckdb::systemone::QuestionType;
using duckdb::systemone::SettingsSnapshot;
using OrderedJson = nlohmann::ordered_json;

namespace {

int checks = 0;

void Check(bool condition, const std::string &message) {
	++checks;
	if (!condition) {
		throw std::runtime_error("check failed: " + message);
	}
}

template <class Function>
std::string ExpectInvalid(Function &&function, const std::string &message) {
	++checks;
	try {
		function();
	} catch (const std::invalid_argument &error) {
		return error.what();
	} catch (const std::exception &error) {
		throw std::runtime_error(message + ": wrong exception type: " + error.what());
	}
	throw std::runtime_error(message + ": expected invalid_argument");
}

SettingsSnapshot Settings(uint64_t max_source = 1024, uint64_t max_request = 65536,
                          std::optional<std::string> keep_alive = std::nullopt) {
	return SettingsSnapshot("http://localhost:11434/v1/systemone", 1000, 120000, 8, 16, max_request, 1048576,
	                        max_source, 1, std::move(keep_alive));
}

std::string Fixture(const std::string &name) {
	std::filesystem::path directory = std::filesystem::path(__FILE__).parent_path().parent_path() /
	                                  "mock/fixtures/request";
	std::ifstream input(directory / name, std::ios::binary);
	if (!input) {
		throw std::runtime_error("cannot open request fixture " + (directory / name).string());
	}
	std::ostringstream contents;
	contents << input.rdbuf();
	return contents.str();
}

std::string ConfigWithCount(size_t count) {
	OrderedJson questions = OrderedJson::object();
	for (size_t i = 0; i < count; ++i) {
		questions["q" + std::to_string(i)] = {{"type", "noul"}, {"instructions", "question"}};
	}
	return OrderedJson{{"questions", std::move(questions)}}.dump();
}

std::string ConfigWithCriteria(const std::string &type, size_t count) {
	OrderedJson criteria;
	if (type == "choice") {
		criteria = OrderedJson::object();
		for (size_t i = 0; i < count; ++i) {
			criteria["option" + std::to_string(i)] = nullptr;
		}
	} else {
		criteria = OrderedJson::array();
		for (size_t i = 0; i < count; ++i) {
			criteria.push_back("level" + std::to_string(i));
		}
	}
	OrderedJson question = {{"type", type}, {"instructions", "classify"}, {"criteria", std::move(criteria)}};
	return OrderedJson{{"questions", {{"q", std::move(question)}}}}.dump();
}

void TestQuestionCountBoundaries() {
	ExpectInvalid([] { BuildRequest("state", ConfigWithCount(0), Settings()); }, "zero questions rejected");
	Check(BuildRequest("state", ConfigWithCount(1), Settings()).questions.ordered.size() == 1,
	      "one question accepted");
	Check(BuildRequest("state", ConfigWithCount(64), Settings()).questions.ordered.size() == 64,
	      "64 questions accepted");
	ExpectInvalid([] { BuildRequest("state", ConfigWithCount(65), Settings()); }, "65 questions rejected");
}

void TestChoiceAndScoreBoundaries() {
	ExpectInvalid([] { BuildRequest("state", ConfigWithCriteria("choice", 1), Settings()); },
	              "one choice rejected");
	Check(BuildRequest("state", ConfigWithCriteria("choice", 2), Settings()).body.size() > 0,
	      "two choices accepted");
	Check(BuildRequest("state", ConfigWithCriteria("choice", 26), Settings()).body.size() > 0,
	      "26 choices accepted");
	ExpectInvalid([] { BuildRequest("state", ConfigWithCriteria("choice", 27), Settings()); },
	              "27 choices rejected");
	ExpectInvalid([] { BuildRequest("state", ConfigWithCriteria("score", 1), Settings()); }, "one score rejected");
	Check(BuildRequest("state", ConfigWithCriteria("score", 2), Settings()).body.size() > 0,
	      "two score levels accepted");
	Check(BuildRequest("state", ConfigWithCriteria("score", 26), Settings()).body.size() > 0,
	      "26 score levels accepted");
	ExpectInvalid([] { BuildRequest("state", ConfigWithCriteria("score", 27), Settings()); },
	              "27 score levels rejected");
}

void TestSchemaRejections() {
	ExpectInvalid([] { BuildRequest("state", "{", Settings()); }, "malformed JSON rejected");
	ExpectInvalid([] { BuildRequest("state", "[]", Settings()); }, "non-object top-level config rejected");
	ExpectInvalid([] { BuildRequest("state", R"({"questions":{},"model":"other"})", Settings()); },
	              "extra top-level model key rejected");
	ExpectInvalid([] { BuildRequest("state", R"({"other":{}})", Settings()); }, "missing questions key rejected");
	ExpectInvalid([] { BuildRequest("state", R"({"questions":{"q":{"instructions":"x"}}})", Settings()); },
	              "missing type rejected");
	ExpectInvalid([] { BuildRequest("state", R"({"questions":{"q":{"type":"noul"}}})", Settings()); },
	              "missing instructions rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"noul","instructions":"x","extra":true}}})",
		             Settings());
	}, "extra question key rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"future","instructions":"x"}}})", Settings());
	}, "unsupported question type rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"noul","instructions":7}}})", Settings());
	}, "non-string instructions rejected");
	ExpectInvalid([] { BuildRequest("state", R"({"questions":{"":{"type":"noul","instructions":"x"}}})", Settings()); },
	              "empty question name rejected");
	ExpectInvalid([] { BuildRequest("state", R"({"questions":{"   ":{"type":"noul","instructions":"x"}}})", Settings()); },
	              "whitespace-only question name rejected");
	ExpectInvalid([] { BuildRequest("state", R"({"questions":{"q":{"type":"noul","instructions":""}}})", Settings()); },
	              "empty instructions rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"choice","instructions":"x"}}})", Settings());
	}, "missing required choice criteria rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"score","instructions":"x","criteria":[]}}})",
		             Settings());
	}, "empty score criteria rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"choice","instructions":"x","criteria":{" ":null,"b":null}}}})",
		             Settings());
	}, "whitespace-only choice option rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"choice","instructions":"x","criteria":{"":null,"b":null}}}})",
		             Settings());
	}, "empty choice option rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"choice","instructions":"x","criteria":{"a":1,"b":null}}}})",
		             Settings());
	}, "non-string choice description rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"score","instructions":"x","criteria":["ok",null]}}})",
		             Settings());
	}, "non-string score level rejected");
	ExpectInvalid([] { BuildRequest("", R"({"questions":{"q":{"type":"noul","instructions":"x"}}})", Settings()); },
	              "empty state rejected");
}

void TestNoulCriteria() {
	Check(BuildRequest("state", R"({"questions":{"q":{"type":"noul","instructions":"x"}}})", Settings())
	          .questions.ordered.front().criteria == std::nullopt,
	      "omitted noul criteria remains absent");
	Check(BuildRequest("state", R"({"questions":{"q":{"type":"noul","instructions":"x","criteria":{"true":"yes","false":"no"}}}})",
	                    Settings())
	          .questions.ordered.front()
	          .criteria->size() == 2,
	      "true/false string criteria accepted");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"noul","instructions":"x","criteria":[]}}})", Settings());
	}, "array noul criteria rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"noul","instructions":"x","criteria":{"yes":"y"}}}})", Settings());
	}, "unknown noul criteria key rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"noul","instructions":"x","criteria":{"true":false}}}})", Settings());
	}, "non-string noul criteria description rejected");
}

void TestDuplicateKeys() {
	ExpectInvalid([&] { BuildRequest("state", Fixture("duplicate_top_level.json"), Settings()); },
	              "duplicate top-level key rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"noul","type":"choice","instructions":"x"}}})",
		             Settings());
	}, "duplicate question key rejected");
	ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"q":{"type":"choice","instructions":"x","criteria":{"a":null,"a":"other"}}}})",
		             Settings());
	}, "duplicate option key rejected");
	const auto duplicate_empty_error = ExpectInvalid([] {
		BuildRequest("state", R"({"questions":{"":{"type":"noul","instructions":"x"},"":{"type":"noul","instructions":"y"}}})",
		             Settings());
	}, "duplicate empty object key rejected");
	Check(duplicate_empty_error.find("duplicate JSON object key") != std::string::npos,
	      "duplicate empty key rejected by SAX before empty-name validation");
}

void TestFixedRequestAndKeepAlive() {
	auto defaults = BuildRequest("state", R"({"questions":{"q":{"type":"noul","instructions":"x"}}})", Settings());
	Check(defaults.body ==
	          R"({"model":"tev1:0.8b","state":"state","questions":{"q":{"type":"noul","instructions":"x"}}})",
	      "fixed model and complete ordered questions serialized");
	Check(defaults.body.find("keep_alive") == std::string::npos, "keep_alive omitted by default");
	auto kept = BuildRequest("state", R"({"questions":{"q":{"type":"noul","instructions":"x"}}})",
	                        Settings(1024, 65536, std::string("10m")));
	Check(kept.body ==
	          R"({"model":"tev1:0.8b","state":"state","questions":{"q":{"type":"noul","instructions":"x"}},"keep_alive":"10m"})",
	      "configured keep_alive is appended");
}

void TestPreservesQuestionAndCriterionOrder() {
	const auto request = BuildRequest("state", Fixture("valid_mixed.json"), Settings());
	Check(request.questions.ordered.size() == 3, "all questions retained");
	Check(request.questions.ordered[0].name == "is_urgent" &&
	          request.questions.ordered[0].type == QuestionType::NOUL,
	      "first question order retained in metadata");
	Check(request.questions.ordered[1].name == "department" &&
	          request.questions.ordered[1].type == QuestionType::CHOICE,
	      "second question order retained in metadata");
	Check(request.questions.ordered[2].name == "severity" &&
	          request.questions.ordered[2].type == QuestionType::SCORE,
	      "third question order retained in metadata");
	OrderedJson body = OrderedJson::parse(request.body);
	auto question_it = body["questions"].begin();
	Check(question_it.key() == "is_urgent", "first serialized question order retained");
	++question_it;
	Check(question_it.key() == "department", "second serialized question order retained");
	++question_it;
	Check(question_it.key() == "severity", "third serialized question order retained");
	Check(body["questions"]["department"]["criteria"].begin().key() == "billing",
	      "first choice option order retained");
	Check(body["questions"]["department"]["criteria"].begin().value().is_null(),
	      "choice null value retained");
	Check(request.questions.ordered[1].criteria->begin().key() == "billing",
	      "choice criteria order retained in metadata");
	Check(request.normalized_source ==
	          "state\nis_urgent\nIs this urgent?\nurgent\nnot urgent\ndepartment\nRoute this item.\nbilling\ntechnical\nengineering\nother\nseverity\nRate severity.\nlow\nmedium\nhigh",
	      "normalized source concatenates exact input text and omits JSON syntax and null values");
}

void TestUnicodeAndEscaping() {
	const std::string state = "Line 1\nquote: \"hi\" — 🚀";
	const auto request = BuildRequest(state, Fixture("unicode_and_escapes.json"), Settings());
	OrderedJson body = OrderedJson::parse(request.body);
	Check(body["state"].get<std::string>() == state, "state survives JSON escaping and UTF-8 round trip");
	Check(body["questions"].begin().key() == "quote\"name", "escaped question name is decoded exactly");
	Check(body["questions"].begin().value()["instructions"].get<std::string>() == "Line one\nline two — 🚀",
	      "unicode and escaped instruction survives round trip");
	Check(request.normalized_source.find(state) != std::string::npos,
	      "normalized source owns original state text");
}

void TestIndependentByteLimits() {
	const std::string config = R"({"questions":{"q":{"type":"noul","instructions":"x"}}})";
	const auto baseline = BuildRequest("a\n\"b", config, Settings());
	const auto source_baseline = BuildRequest("state", config, Settings());
	Check(source_baseline.normalized_source == "state\nq\nx", "source text uses the defined field ordering");
	Check(BuildRequest("state", config, Settings(source_baseline.normalized_source.size(), 65536)).body ==
	          source_baseline.body,
	      "source exactly at byte limit accepted");
	ExpectInvalid([&] {
		BuildRequest("state", config, Settings(source_baseline.normalized_source.size() - 1, 65536));
	}, "source one byte over limit rejected");
	Check(baseline.body.find("\\n") != std::string::npos && baseline.body.find("\\\"") != std::string::npos,
	      "serialized body counts JSON escaping bytes");
	Check(BuildRequest("a\n\"b", config, Settings(1024, baseline.body.size())).body == baseline.body,
	      "request exactly at escaped byte limit accepted");
	const std::string body_error = ExpectInvalid(
	    [&] { BuildRequest("a\n\"b", config, Settings(1024, baseline.body.size() - 1)); },
	    "request over escaped byte limit rejected");
	Check(body_error.find("ollama_max_request_bytes") != std::string::npos,
	      "request-body overflow reports its independent setting");
	const std::string source_error = ExpectInvalid(
	    [&] { BuildRequest("state", config, Settings(5, 65536)); }, "normalized source over byte limit rejected");
	Check(source_error.find("ollama_max_source_bytes") != std::string::npos,
	      "source overflow reports its independent setting");
	Check(BuildRequest("state", config, Settings(1024, 65536)).normalized_source.size() <= 1024,
	      "accepted normalized source stays within source limit");
}

} // namespace

int main() {
	try {
		TestQuestionCountBoundaries();
		TestChoiceAndScoreBoundaries();
		TestSchemaRejections();
		TestNoulCriteria();
		TestDuplicateKeys();
		TestFixedRequestAndKeepAlive();
		TestPreservesQuestionAndCriterionOrder();
		TestUnicodeAndEscaping();
		TestIndependentByteLimits();
		std::cout << "systemone request contract: " << checks << " checks passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
