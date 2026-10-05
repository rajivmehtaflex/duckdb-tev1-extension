#include "systemone/request.hpp"
#include "systemone/transport.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::ordered_json;
using duckdb::systemone::BuildRequest;
using duckdb::systemone::OwnedRequest;
using duckdb::systemone::RequestHandle;
using duckdb::systemone::SettingsSnapshot;
using duckdb::systemone::TransportDispatcher;
using duckdb::systemone::TransportResult;

class CurlGlobalGuard {
public:
	CurlGlobalGuard() {
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
			throw std::runtime_error("libcurl global initialization failed");
		}
	}
	~CurlGlobalGuard() {
		curl_global_cleanup();
	}
	CurlGlobalGuard(const CurlGlobalGuard &) = delete;
	CurlGlobalGuard &operator=(const CurlGlobalGuard &) = delete;
};

uint64_t ParsePositive(const char *name, const char *value, bool allow_zero = false) {
	std::size_t consumed = 0;
	const std::string text(value);
	uint64_t parsed;
	try {
		parsed = std::stoull(text, &consumed);
	} catch (...) {
		throw std::invalid_argument(std::string(name) + " must be an integer");
	}
	if (consumed != text.size() || (!allow_zero && parsed == 0)) {
		throw std::invalid_argument(std::string(name) + (allow_zero ? " must be a non-negative integer" : " must be positive"));
	}
	return parsed;
}

std::string QuestionConfig(uint64_t count) {
	Json questions = Json::object();
	for (uint64_t i = 0; i < count; ++i) {
		questions["q" + std::to_string(i)] = Json{{"type", "noul"}, {"instructions", "x"}};
	}
	return Json{{"questions", std::move(questions)}}.dump();
}

OwnedRequest MakeRequest(uint64_t row, const std::string &config, const SettingsSnapshot &settings) {
	return BuildRequest("benchmark-row-" + std::to_string(row), config, settings);
}

Json ResultJson(const TransportResult &result) {
	return Json{{"row", result.slot},
	            {"success", result.success},
	            {"error", static_cast<uint8_t>(result.error)},
	            {"http_status", result.http_status},
	            {"queue_wait_ms", result.queue_wait_ms},
	            {"request_duration_ms", result.request_duration_ms},
	            {"response_bytes", result.response_bytes},
	            {"retries", result.retries}};
}

void CollectReady(std::vector<RequestHandle> &handles, std::vector<Json> &rows, const Clock::time_point started,
                  uint64_t &completion_order) {
	for (std::size_t i = 0; i < handles.size();) {
		if (!handles[i].completion.valid() ||
		    handles[i].completion.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
			++i;
			continue;
		}
		auto result = handles[i].completion.get();
		auto row = ResultJson(result);
		row["completion_order"] = completion_order++;
		row["completion_elapsed_ms"] = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
		rows.at(static_cast<std::size_t>(result.slot)) = std::move(row);
		handles.erase(handles.begin() + static_cast<std::ptrdiff_t>(i));
	}
}

Json Run(const std::string &endpoint, uint64_t row_count, uint64_t question_count, uint64_t max_inflight,
         uint64_t queue_capacity) {
	SettingsSnapshot settings(endpoint, 1000, 120000, max_inflight, queue_capacity, 65536, 1048576, 1024, 0,
	                         std::string("5m"));
	TransportDispatcher dispatcher(settings);
	const auto config = QuestionConfig(question_count);
	std::vector<Json> rows(static_cast<std::size_t>(row_count));
	std::vector<RequestHandle> handles;
	const auto admission_limit = max_inflight > std::numeric_limits<uint64_t>::max() - queue_capacity
	                                 ? std::numeric_limits<uint64_t>::max()
	                                 : std::max<uint64_t>(1, max_inflight + queue_capacity);
	const auto started = Clock::now();
	uint64_t completion_order = 0;
	for (uint64_t row = 0; row < row_count; ++row) {
		while (handles.size() >= admission_limit) {
			CollectReady(handles, rows, started, completion_order);
			if (handles.size() >= admission_limit) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		handles.push_back(dispatcher.Submit(row, MakeRequest(row, config, settings)));
		CollectReady(handles, rows, started, completion_order);
	}
	while (!handles.empty()) {
		CollectReady(handles, rows, started, completion_order);
		if (!handles.empty()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	const auto elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
	const auto stats = dispatcher.StatsSnapshot();
	return Json{{"rows", std::move(rows)},
	            {"elapsed_ms", elapsed_ms},
	            {"throughput_rows_per_second", elapsed_ms > 0 ? row_count * 1000.0 / elapsed_ms : 0.0},
	            {"settings", {{"row_count", row_count}, {"question_count", question_count},
	                           {"max_inflight", max_inflight}, {"queue_capacity", queue_capacity},
	                           {"transport_io_threads", duckdb::systemone::TRANSPORT_EVENT_LOOP_COUNT}}},
	            {"request_limits", {{"max_request_bytes", settings.max_request_bytes},
	                                {"max_response_bytes", settings.max_response_bytes},
	                                {"max_source_bytes", settings.max_source_bytes}}},
	            {"transport_stats", {{"completed", stats.completed}, {"queue_wait_ms", stats.queue_wait_ms},
	                                  {"request_duration_ms", stats.request_duration_ms},
	                                  {"response_bytes", stats.response_bytes}, {"error_count", stats.error_count}}}};
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 6) {
		std::cerr << "usage: systemone_transport_benchmark <endpoint> <rows> <questions:1|8|64> <max_inflight> <queue_capacity>\n";
		return 2;
	}
	try {
		const auto rows = ParsePositive("rows", argv[2]);
		const auto questions = ParsePositive("questions", argv[3]);
		if (questions != 1 && questions != 8 && questions != 64) {
			throw std::invalid_argument("questions must be one of 1, 8, or 64");
		}
		const auto max_inflight = ParsePositive("max_inflight", argv[4]);
		const auto queue_capacity = ParsePositive("queue_capacity", argv[5], true);
		CurlGlobalGuard curl_global;
		std::cout << Run(argv[1], rows, questions, max_inflight, queue_capacity).dump() << '\n';
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "systemone_transport_benchmark: " << error.what() << '\n';
		return 1;
	}
}
