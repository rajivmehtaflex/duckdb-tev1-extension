#include "systemone/request.hpp"
#include "systemone/transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace {

using namespace std::chrono_literals;
using duckdb::systemone::BuildRequest;
using duckdb::systemone::OwnedRequest;
using duckdb::systemone::RequestCompletion;
using duckdb::systemone::RequestHandle;
using duckdb::systemone::SettingsSnapshot;
using duckdb::systemone::TransportDispatcher;
using duckdb::systemone::TransportErrorCategory;
using duckdb::systemone::TransportResult;
using duckdb::systemone::TransportStats;
using Json = nlohmann::ordered_json;

class CurlGlobalGuard {
public:
	CurlGlobalGuard() {
		const auto result = curl_global_init(CURL_GLOBAL_DEFAULT);
		if (result != CURLE_OK) {
			throw std::runtime_error("could not initialize libcurl for the standalone driver");
		}
	}
	~CurlGlobalGuard() {
		curl_global_cleanup();
	}
	CurlGlobalGuard(const CurlGlobalGuard &) = delete;
	CurlGlobalGuard &operator=(const CurlGlobalGuard &) = delete;
};

SettingsSnapshot MakeSettings(const std::string &endpoint, uint64_t max_inflight = 2,
                              uint64_t queue_capacity = 2, uint64_t request_timeout_ms = 5000,
                              uint64_t max_response_bytes = 1048576, uint64_t retry_count = 0,
                              std::optional<std::string> keep_alive = std::nullopt) {
	return SettingsSnapshot(endpoint, 300, request_timeout_ms, max_inflight, queue_capacity, 65536,
	                        max_response_bytes, 1024, retry_count, std::move(keep_alive));
}

OwnedRequest MakeRequest(const std::string &state, const SettingsSnapshot &settings) {
	const std::string config = R"({"questions":{"q":{"type":"noul","instructions":"test question"}}})";
	return BuildRequest(state, config, settings);
}

const char *ErrorName(TransportErrorCategory error) {
	switch (error) {
	case TransportErrorCategory::NONE:
		return "NONE";
	case TransportErrorCategory::CANCELLED:
		return "CANCELLED";
	case TransportErrorCategory::DISPATCHER_CLOSED:
		return "DISPATCHER_CLOSED";
	case TransportErrorCategory::CONNECTION_REFUSED:
		return "CONNECTION_REFUSED";
	case TransportErrorCategory::CONNECTION_FAILURE:
		return "CONNECTION_FAILURE";
	case TransportErrorCategory::TIMEOUT:
		return "TIMEOUT";
	case TransportErrorCategory::MODEL_NOT_FOUND:
		return "MODEL_NOT_FOUND";
	case TransportErrorCategory::UNSUPPORTED_ROUTE:
		return "UNSUPPORTED_ROUTE";
	case TransportErrorCategory::HTTP_STATUS:
		return "HTTP_STATUS";
	case TransportErrorCategory::RESPONSE_TOO_LARGE:
		return "RESPONSE_TOO_LARGE";
	case TransportErrorCategory::INVALID_RESPONSE:
		return "INVALID_RESPONSE";
	case TransportErrorCategory::CURL_FAILURE:
		return "CURL_FAILURE";
	}
	return "UNKNOWN";
}

Json ResultJson(const TransportResult &result) {
	return Json{{"slot", result.slot},
	            {"success", result.success},
	            {"error", ErrorName(result.error)},
	            {"http_status", result.http_status},
	            {"retries", result.retries},
	            {"response_bytes", result.response_bytes},
	            {"queue_wait_ms", result.queue_wait_ms},
	            {"request_duration_ms", result.request_duration_ms}};
}

Json StatsJson(const TransportStats &stats) {
	Json categories = Json::array();
	for (auto count : stats.errors_by_category) {
		categories.push_back(count);
	}
	return Json{{"queued", stats.queued},
	            {"active", stats.active},
	            {"completed", stats.completed},
	            {"completed_pending", stats.completed_pending},
	            {"queue_wait_ms", stats.queue_wait_ms},
	            {"request_duration_ms", stats.request_duration_ms},
	            {"timeout_count", stats.timeout_count},
	            {"retry_count", stats.retry_count},
	            {"error_count", stats.error_count},
	            {"cancelled_count", stats.cancelled_count},
	            {"response_bytes", stats.response_bytes},
	            {"errors_by_category", std::move(categories)}};
}

Json MakeOutput(const std::vector<TransportResult> &results, const std::vector<uint64_t> &completion_order,
                const TransportStats &stats, Json observations = Json::object()) {
	Json output_results = Json::array();
	for (const auto &result : results) {
		output_results.push_back(ResultJson(result));
	}
	return Json{{"results", std::move(output_results)},
	            {"completion_order", completion_order},
	            {"stats", StatsJson(stats)},
	            {"observations", std::move(observations)}};
}

void AppendResult(TransportResult result, std::vector<TransportResult> &results,
                  std::vector<uint64_t> &completion_order) {
	completion_order.push_back(result.slot);
	results.push_back(std::move(result));
}

void CollectReady(std::vector<RequestHandle> &handles, std::vector<bool> &consumed,
                  std::vector<TransportResult> &results, std::vector<uint64_t> &completion_order) {
	bool made_progress = true;
	while (made_progress) {
		made_progress = false;
		for (size_t i = 0; i < handles.size(); ++i) {
			if (consumed[i] || !handles[i].completion.valid()) {
				continue;
			}
			if (handles[i].completion.wait_for(0ms) == std::future_status::ready) {
				AppendResult(handles[i].completion.get(), results, completion_order);
				consumed[i] = true;
				made_progress = true;
			}
		}
	}
}

void CollectAll(std::vector<RequestHandle> &handles, std::vector<TransportResult> &results,
	            std::vector<uint64_t> &completion_order) {
	std::vector<bool> consumed(handles.size(), false);
	size_t remaining = handles.size();
	while (remaining) {
		const size_t before = results.size();
		CollectReady(handles, consumed, results, completion_order);
		remaining -= results.size() - before;
		if (remaining) {
			std::this_thread::sleep_for(1ms);
		}
	}
}

Json RunKeepAlive(const std::string &endpoint, uint64_t count) {
	const auto queue_capacity = count > 2 ? count - 2 : 0;
	auto settings = MakeSettings(endpoint, 2, queue_capacity, 5000, 1048576, 0, "5m");
	TransportDispatcher dispatcher(settings);
	std::vector<RequestHandle> handles;
	for (uint64_t i = 0; i < count; ++i) {
		handles.push_back(dispatcher.Submit(i + 1, MakeRequest("keepalive-" + std::to_string(i), settings)));
	}
	std::vector<TransportResult> results;
	std::vector<uint64_t> completion_order;
	CollectAll(handles, results, completion_order);
	handles.clear();
	return MakeOutput(results, completion_order, dispatcher.StatsSnapshot());
}

Json RunReusedConnectionClose(const std::string &endpoint) {
	auto settings = MakeSettings(endpoint, 1, 0, 5000, 1048576, 1);
	TransportDispatcher dispatcher(settings);
	std::vector<TransportResult> results;
	std::vector<uint64_t> completion_order;
	for (const auto *state : {"prime-loop-0", "prime-loop-1", "reuse-close"}) {
		RequestHandle handle = dispatcher.Submit(results.size() + 1, MakeRequest(state, settings));
		AppendResult(handle.completion.get(), results, completion_order);
	}
	return MakeOutput(results, completion_order, dispatcher.StatsSnapshot());
}

Json RunOutOfOrder(const std::string &endpoint) {
	auto settings = MakeSettings(endpoint, 2, 2);
	TransportDispatcher dispatcher(settings);
	std::vector<RequestHandle> handles;
	handles.push_back(dispatcher.Submit(10, MakeRequest("delay:420:slow", settings)));
	handles.push_back(dispatcher.Submit(20, MakeRequest("delay:25:fast", settings)));
	std::vector<TransportResult> results;
	std::vector<uint64_t> completion_order;
	CollectAll(handles, results, completion_order);
	handles.clear();
	return MakeOutput(results, completion_order, dispatcher.StatsSnapshot());
}

struct SubmitCell {
	std::optional<RequestHandle> handle;
	std::exception_ptr error;
	bool done = false;
};

Json RunConcurrency(const std::string &endpoint) {
	// Capacity is three: two active reservations and one queued reservation.
	// The fourth Submitter must wait until an earlier result is consumed.
	auto settings = MakeSettings(endpoint, 2, 1, 5000, 1048576, 0);
	TransportDispatcher dispatcher(settings);
	std::mutex mutex;
	std::condition_variable cv;
	size_t returned = 0;
	std::vector<std::unique_ptr<SubmitCell>> cells;
	std::vector<std::thread> submitters;
	for (size_t i = 0; i < 4; ++i) {
		cells.push_back(std::make_unique<SubmitCell>());
	}
	for (size_t i = 0; i < cells.size(); ++i) {
		submitters.emplace_back([&, i] {
			try {
				auto handle = dispatcher.Submit(i + 1, MakeRequest("delay:800:concurrent-" + std::to_string(i), settings));
				std::lock_guard<std::mutex> guard(mutex);
				cells[i]->handle.emplace(std::move(handle));
				++returned;
				cells[i]->done = true;
				cv.notify_all();
			} catch (...) {
				std::lock_guard<std::mutex> guard(mutex);
				cells[i]->error = std::current_exception();
				cells[i]->done = true;
				cv.notify_all();
			}
		});
	}

	{
		std::unique_lock<std::mutex> guard(mutex);
		cv.wait_for(guard, 2s, [&] { return returned >= 3; });
	}
	std::this_thread::sleep_for(45ms);
	size_t returned_before_consume;
	{
		std::lock_guard<std::mutex> guard(mutex);
		returned_before_consume = returned;
	}
	const auto mid_stats = dispatcher.StatsSnapshot();

	// Wait for a completion without consuming it. The fourth Submit must stay
	// blocked while the completed result still owns its admission reservation.
	size_t consumed_index = cells.size();
	const auto wait_until = std::chrono::steady_clock::now() + 3s;
	while (consumed_index == cells.size() && std::chrono::steady_clock::now() < wait_until) {
		{
			std::lock_guard<std::mutex> guard(mutex);
			for (size_t i = 0; i < cells.size(); ++i) {
				if (cells[i]->handle && cells[i]->handle->completion.valid() &&
				    cells[i]->handle->completion.wait_for(0ms) == std::future_status::ready) {
					consumed_index = i;
					break;
				}
			}
		}
		if (consumed_index == cells.size()) {
			std::this_thread::sleep_for(1ms);
		}
	}

	std::vector<TransportResult> results;
	std::vector<uint64_t> completion_order;
	bool released = false;
	size_t returned_with_unconsumed_result;
	TransportStats completed_pending_stats;
	{
		std::lock_guard<std::mutex> guard(mutex);
		returned_with_unconsumed_result = returned;
	}
	if (consumed_index < cells.size()) {
		completed_pending_stats = dispatcher.StatsSnapshot();
	}
	if (consumed_index < cells.size()) {
		AppendResult(cells[consumed_index]->handle->completion.get(), results, completion_order);
		cells[consumed_index]->handle.reset();
		std::unique_lock<std::mutex> guard(mutex);
		released = cv.wait_for(guard, 2s, [&] { return returned == 4; });
	}
	if (!released) {
		// Avoid hanging the harness if admission does not reopen after release.
		dispatcher.Cancel();
	}
	for (auto &thread : submitters) {
		thread.join();
	}
	for (auto &cell : cells) {
		if (cell->error) {
			std::rethrow_exception(cell->error);
		}
	}

	std::vector<RequestHandle> handles;
	for (size_t i = 0; i < cells.size(); ++i) {
		if (cells[i]->handle) {
			handles.push_back(std::move(*cells[i]->handle));
			cells[i]->handle.reset();
		}
	}
	std::vector<bool> consumed(handles.size(), false);
	size_t remaining = handles.size();
	while (remaining) {
		const size_t before = results.size();
		CollectReady(handles, consumed, results, completion_order);
		remaining -= results.size() - before;
		if (remaining) {
			std::this_thread::sleep_for(1ms);
		}
	}
	handles.clear();
	Json observations{{"returned_before_consume", returned_before_consume},
	                  {"mid_stats", StatsJson(mid_stats)},
	                  {"returned_with_unconsumed_result", returned_with_unconsumed_result},
	                  {"completed_pending_stats", StatsJson(completed_pending_stats)},
	                  {"released_after_result_consumed", released}};
	return MakeOutput(results, completion_order, dispatcher.StatsSnapshot(), std::move(observations));
}

Json RunSimple(const std::string &endpoint, const std::vector<std::string> &states,
	           uint64_t request_timeout_ms = 5000, uint64_t max_response_bytes = 1048576,
	           uint64_t retry_count = 0) {
	auto settings = MakeSettings(endpoint, 2, 2, request_timeout_ms, max_response_bytes, retry_count);
	TransportDispatcher dispatcher(settings);
	std::vector<RequestHandle> handles;
	for (size_t i = 0; i < states.size(); ++i) {
		handles.push_back(dispatcher.Submit(i + 1, MakeRequest(states[i], settings)));
	}
	std::vector<TransportResult> results;
	std::vector<uint64_t> completion_order;
	CollectAll(handles, results, completion_order);
	handles.clear();
	return MakeOutput(results, completion_order, dispatcher.StatsSnapshot());
}

Json RunCancel(const std::string &endpoint) {
	auto settings = MakeSettings(endpoint, 2, 1, 10000);
	TransportDispatcher dispatcher(settings);
	std::vector<RequestHandle> handles;
	handles.push_back(dispatcher.Submit(1, MakeRequest("delay:2500:cancel-one", settings)));
	handles.push_back(dispatcher.Submit(2, MakeRequest("delay:2500:cancel-two", settings)));
	handles.push_back(dispatcher.Submit(3, MakeRequest("delay:2500:cancel-queued", settings)));
	std::this_thread::sleep_for(60ms);
	dispatcher.Cancel(handles[2]);
	dispatcher.Cancel(handles.front());
	dispatcher.Cancel();
	std::vector<TransportResult> results;
	std::vector<uint64_t> completion_order;
	CollectAll(handles, results, completion_order);
	handles.clear();
	return MakeOutput(results, completion_order, dispatcher.StatsSnapshot());
}

Json RunRetryRefused(const std::string &endpoint) {
	return RunSimple(endpoint, {"retry-before-upload"}, 700, 1048576, 1);
}

Json RunAdmissionCancellation(const std::string &endpoint) {
	auto settings = MakeSettings(endpoint, 1, 0, 5000);
	TransportDispatcher dispatcher(settings);
	RequestHandle active = dispatcher.Submit(1, MakeRequest("delay:2500:admission-active", settings));
	auto admission_cancelled = std::make_shared<std::atomic<bool>>(false);
	std::mutex mutex;
	std::condition_variable cv;
	bool submit_started = false;
	bool submit_done = false;
	std::optional<RequestHandle> blocked_handle;
	std::exception_ptr submit_error;
	std::thread submitter([&] {
		{
			std::lock_guard<std::mutex> guard(mutex);
			submit_started = true;
			cv.notify_all();
		}
		try {
			auto handle = dispatcher.Submit(2, MakeRequest("must-not-be-sent", settings), admission_cancelled);
			std::lock_guard<std::mutex> guard(mutex);
			blocked_handle.emplace(std::move(handle));
			submit_done = true;
			cv.notify_all();
		} catch (...) {
			std::lock_guard<std::mutex> guard(mutex);
			submit_error = std::current_exception();
			submit_done = true;
			cv.notify_all();
		}
	});
	{
		std::unique_lock<std::mutex> guard(mutex);
		cv.wait_for(guard, 1s, [&] { return submit_started; });
	}
	std::this_thread::sleep_for(80ms);
	bool blocked_before_token = false;
	{
		std::lock_guard<std::mutex> guard(mutex);
		blocked_before_token = !submit_done;
	}
	const auto release_started = std::chrono::steady_clock::now();
	admission_cancelled->store(true, std::memory_order_relaxed);
	bool resolved_after_token = false;
	{
		std::unique_lock<std::mutex> guard(mutex);
		resolved_after_token = cv.wait_for(guard, 1s, [&] { return submit_done; });
	}
	const auto token_wait_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                                     std::chrono::steady_clock::now() - release_started)
	                                                     .count());
	if (!resolved_after_token) {
		dispatcher.Cancel();
	}
	submitter.join();
	if (submit_error) {
		std::rethrow_exception(submit_error);
	}
	if (!blocked_handle) {
		throw std::runtime_error("cancelled admission did not return a handle");
	}
	const bool admission_handle_valid = blocked_handle->IsValid();
	auto admission_result = blocked_handle->completion.get();
	blocked_handle.reset();
	dispatcher.Cancel(active);
	auto active_result = active.completion.get();
	active = RequestHandle();
	std::vector<TransportResult> results{std::move(active_result), std::move(admission_result)};
	std::vector<uint64_t> completion_order{results[0].slot, results[1].slot};
	Json observations{{"blocked_before_token", blocked_before_token},
	                  {"resolved_after_token", resolved_after_token},
	                  {"token_wait_ms", token_wait_ms},
	                  {"admission_handle_valid", admission_handle_valid}};
	return MakeOutput(results, completion_order, dispatcher.StatsSnapshot(), std::move(observations));
}

Json RunQueuedCancellation(const std::string &endpoint) {
	auto settings = MakeSettings(endpoint, 1, 2, 5000);
	TransportDispatcher dispatcher(settings);
	std::vector<RequestHandle> handles;
	handles.push_back(dispatcher.Submit(1, MakeRequest("delay:500:queued-active", settings)));
	handles.push_back(dispatcher.Submit(2, MakeRequest("must-be-cancelled-in-queue", settings)));
	handles.push_back(dispatcher.Submit(3, MakeRequest("queued-survivor", settings)));
	dispatcher.Cancel(handles[1]);
	std::vector<TransportResult> results;
	std::vector<uint64_t> completion_order;
	CollectAll(handles, results, completion_order);
	handles.clear();
	return MakeOutput(results, completion_order, dispatcher.StatsSnapshot());
}

Json RunClosedAdmission(const std::string &endpoint) {
	auto settings = MakeSettings(endpoint, 1, 0);
	TransportDispatcher dispatcher(settings);
	dispatcher.Cancel();
	const auto started = std::chrono::steady_clock::now();
	RequestHandle handle = dispatcher.Submit(1, MakeRequest("closed-admission", settings));
	const auto submit_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                              std::chrono::steady_clock::now() - started)
	                                              .count());
	auto result = handle.completion.get();
	handle = RequestHandle();
	std::vector<TransportResult> results{std::move(result)};
	std::vector<uint64_t> completion_order{results.front().slot};
	return MakeOutput(results, completion_order, dispatcher.StatsSnapshot(), Json{{"submit_ms", submit_ms}});
}

Json RunDestroyOutstanding(const std::string &endpoint) {
	RequestCompletion outstanding;
	const auto started = std::chrono::steady_clock::now();
	{
		auto settings = MakeSettings(endpoint, 1, 0, 10000);
		TransportDispatcher dispatcher(settings);
		RequestHandle handle = dispatcher.Submit(1, MakeRequest("delay:4000:destroy-outstanding", settings));
		outstanding = std::move(handle.completion);
		std::this_thread::sleep_for(60ms);
	}
	const auto teardown_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                                 std::chrono::steady_clock::now() - started)
	                                                 .count());
	auto destroyed_result = outstanding.get();
	auto settings = MakeSettings(endpoint, 1, 0, 5000);
	TransportDispatcher replacement(settings);
	RequestHandle replacement_handle = replacement.Submit(2, MakeRequest("after-destroy", settings));
	auto replacement_result = replacement_handle.completion.get();
	replacement_handle = RequestHandle();
	std::vector<TransportResult> results{std::move(destroyed_result), std::move(replacement_result)};
	std::vector<uint64_t> completion_order{results[0].slot, results[1].slot};
	return MakeOutput(results, completion_order, replacement.StatsSnapshot(), Json{{"teardown_ms", teardown_ms}});
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 3) {
		std::cerr << "usage: transport_test_driver <mode> <endpoint>\n";
		return 2;
	}
	const std::string mode = argv[1];
	const std::string endpoint = argv[2];
	try {
		CurlGlobalGuard curl_global;
		Json output;
		if (mode == "concurrency") {
			output = RunConcurrency(endpoint);
		} else if (mode == "keepalive") {
			output = RunKeepAlive(endpoint, 6);
		} else if (mode == "reuse_close") {
			output = RunReusedConnectionClose(endpoint);
		} else if (mode == "out_of_order") {
			output = RunOutOfOrder(endpoint);
		} else if (mode == "not_found") {
			output = RunSimple(endpoint, {"missing-model", "unsupported-route"}, 5000, 1048576, 1);
		} else if (mode == "http_error") {
			output = RunSimple(endpoint, {"http-500"}, 5000, 1048576, 1);
		} else if (mode == "malformed") {
			output = RunSimple(endpoint, {"malformed"}, 5000, 1048576, 1);
		} else if (mode == "oversize") {
			output = RunSimple(endpoint, {"oversize"}, 5000, 512, 1);
		} else if (mode == "timeout") {
			output = RunSimple(endpoint, {"delay:300:timeout"}, 70, 1048576, 1);
		} else if (mode == "retry_refused") {
			output = RunRetryRefused(endpoint);
		} else if (mode == "resolve_failure") {
			output = RunSimple(endpoint, {"resolve-failure"}, 2000, 1048576, 0);
		} else if (mode == "admission_cancel") {
			output = RunAdmissionCancellation(endpoint);
		} else if (mode == "queued_cancel") {
			output = RunQueuedCancellation(endpoint);
		} else if (mode == "closed_admission") {
			output = RunClosedAdmission(endpoint);
		} else if (mode == "destroy_outstanding") {
			output = RunDestroyOutstanding(endpoint);
		} else if (mode == "cancel") {
			output = RunCancel(endpoint);
		} else {
			std::cerr << "unknown transport test mode: " << mode << '\n';
			return 2;
		}
		std::cout << output.dump() << '\n';
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "transport_test_driver: " << error.what() << '\n';
		return 1;
	}
}
