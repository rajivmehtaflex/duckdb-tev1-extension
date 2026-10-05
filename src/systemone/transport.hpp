#pragma once

#include "systemone/config.hpp"
#include "systemone/request.hpp"
#include "systemone/response.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <functional>
#include <memory>
#include <string>

namespace duckdb {
namespace systemone {

constexpr std::size_t TRANSPORT_EVENT_LOOP_COUNT = 2;

enum class TransportErrorCategory : uint8_t {
	NONE,
	CANCELLED,
	DISPATCHER_CLOSED,
	CONNECTION_REFUSED,
	CONNECTION_FAILURE,
	TIMEOUT,
	MODEL_NOT_FOUND,
	UNSUPPORTED_ROUTE,
	HTTP_STATUS,
	RESPONSE_TOO_LARGE,
	INVALID_RESPONSE,
	CURL_FAILURE,
};

struct TransportResult {
	uint64_t slot = 0;
	bool success = false;
	TransportErrorCategory error = TransportErrorCategory::NONE;
	std::string diagnostic;
	nlohmann::ordered_json response;
	long http_status = 0;
	uint64_t response_bytes = 0;
	uint64_t queue_wait_ms = 0;
	uint64_t request_duration_ms = 0;
	uint64_t retries = 0;
};

class RequestCompletion {
public:
	RequestCompletion() = default;
	RequestCompletion(std::future<TransportResult> future_p, std::shared_ptr<void> admission_reservation_p = nullptr);
	RequestCompletion(RequestCompletion &&other) noexcept;
	RequestCompletion &operator=(RequestCompletion &&other) noexcept;
	RequestCompletion(const RequestCompletion &) = delete;
	RequestCompletion &operator=(const RequestCompletion &) = delete;

	bool valid() const;
	void wait();
	template <class Rep, class Period>
	std::future_status wait_for(const std::chrono::duration<Rep, Period> &duration) {
		return future.wait_for(duration);
	}
	TransportResult get();

private:
	// The permit travels with the future-like wrapper and reserves the response
	// slot until this completion/result state is released.
	std::future<TransportResult> future;
	std::shared_ptr<void> admission_reservation;
};

class RequestHandle {
public:
	RequestHandle() = default;
	RequestHandle(RequestHandle &&other) noexcept;
	RequestHandle &operator=(RequestHandle &&other) noexcept;
	RequestHandle(const RequestHandle &) = delete;
	RequestHandle &operator=(const RequestHandle &) = delete;

	bool IsValid() const;
	uint64_t Id() const;
	RequestCompletion completion;

private:
	friend class TransportDispatcher;
	explicit RequestHandle(uint64_t id_p, RequestCompletion completion_p);
	uint64_t id = 0;
};

struct TransportStats {
	uint64_t queued = 0;
	uint64_t active = 0;
	uint64_t completed = 0;
	uint64_t queue_wait_ms = 0;
	uint64_t request_duration_ms = 0;
	uint64_t completed_pending = 0;
	uint64_t timeout_count = 0;
	uint64_t retry_count = 0;
	uint64_t error_count = 0;
	uint64_t cancelled_count = 0;
	uint64_t response_bytes = 0;
	std::array<uint64_t, 12> errors_by_category = {};
};

// Two event-loop threads own one CURLM handle each. Active and waiting work is
// bounded by max_inflight and queue_capacity; handle-owned admission permits
// also bound completed, unconsumed results by their sum.
// Submit may block while waiting for admission; it never holds the dispatcher
// mutex while waiting. The optional token cancels only that admission wait.
class TransportDispatcher {
public:
	explicit TransportDispatcher(SettingsSnapshot settings);
	~TransportDispatcher();
	TransportDispatcher(const TransportDispatcher &) = delete;
	TransportDispatcher &operator=(const TransportDispatcher &) = delete;

	RequestHandle Submit(uint64_t slot, OwnedRequest request,
	                     std::shared_ptr<std::atomic<bool>> admission_cancelled = nullptr,
	                     std::function<bool()> admission_cancel_check = {});
	void Cancel(const RequestHandle &handle) noexcept;
	void Cancel() noexcept;
	TransportStats StatsSnapshot() const;

private:
	class Impl;
	std::shared_ptr<Impl> impl;
};

} // namespace systemone
} // namespace duckdb
