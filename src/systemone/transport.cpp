#include "systemone/transport.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace duckdb {
namespace systemone {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t EVENT_LOOP_COUNT = 2;
constexpr std::size_t ERROR_CATEGORY_COUNT = 12;
static_assert(static_cast<std::size_t>(TransportErrorCategory::CURL_FAILURE) + 1 == ERROR_CATEGORY_COUNT,
              "Update the transport error counter when adding an error category");

uint64_t Milliseconds(Clock::duration duration) {
	if (duration <= Clock::duration::zero()) {
		return 0;
	}
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

uint64_t SaturatingAdd(uint64_t current, uint64_t increment) {
	if (std::numeric_limits<uint64_t>::max() - current < increment) {
		return std::numeric_limits<uint64_t>::max();
	}
	return current + increment;
}

TransportResult ErrorResult(uint64_t slot, TransportErrorCategory category, std::string diagnostic) {
	TransportResult result;
	result.slot = slot;
	result.error = category;
	result.diagnostic = std::move(diagnostic);
	return result;
}

std::string Lowercase(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

std::string ExtractServerError(const std::string &body) {
	auto parsed = nlohmann::ordered_json::parse(body, nullptr, false);
	if (parsed.is_discarded() || !parsed.is_object()) {
		return {};
	}
	const auto error = parsed.find("error");
	if (error == parsed.end() || !error->is_string()) {
		return {};
	}
	return error->get<std::string>();
}

TransportErrorCategory ClassifyHttpError(long status, const std::string &body) {
	const auto message = Lowercase(ExtractServerError(body));
	const bool says_model_missing = message.find("model") != std::string::npos &&
	                                (message.find("not found") != std::string::npos ||
	                                 message.find("does not exist") != std::string::npos ||
	                                 message.find("missing") != std::string::npos);
	if (says_model_missing) {
		return TransportErrorCategory::MODEL_NOT_FOUND;
	}
	const bool says_route_unsupported = message.find("unsupported") != std::string::npos ||
	                                    message.find("not supported") != std::string::npos ||
	                                    message.find("route") != std::string::npos ||
	                                    message.find("endpoint") != std::string::npos ||
	                                    message.find("api version") != std::string::npos;
	if (says_route_unsupported) {
		return TransportErrorCategory::UNSUPPORTED_ROUTE;
	}
	return TransportErrorCategory::HTTP_STATUS;
}

std::string DiagnosticFor(TransportErrorCategory category, long status = 0) {
	switch (category) {
	case TransportErrorCategory::CANCELLED:
		return "SystemOne request was cancelled";
	case TransportErrorCategory::DISPATCHER_CLOSED:
		return "SystemOne dispatcher is closed";
	case TransportErrorCategory::CONNECTION_REFUSED:
		return "SystemOne endpoint refused the connection";
	case TransportErrorCategory::CONNECTION_FAILURE:
		return "could not connect to the SystemOne endpoint";
	case TransportErrorCategory::TIMEOUT:
		return "SystemOne request timed out";
	case TransportErrorCategory::MODEL_NOT_FOUND:
		return "SystemOne model was not found";
	case TransportErrorCategory::UNSUPPORTED_ROUTE:
		return "SystemOne endpoint or API route is unsupported";
	case TransportErrorCategory::HTTP_STATUS:
		return "SystemOne endpoint returned HTTP status " + std::to_string(status);
	case TransportErrorCategory::RESPONSE_TOO_LARGE:
		return "SystemOne response exceeds ollama_max_response_bytes";
	case TransportErrorCategory::INVALID_RESPONSE:
		return "SystemOne response is invalid";
	case TransportErrorCategory::CURL_FAILURE:
		return "libcurl could not complete the SystemOne request";
	case TransportErrorCategory::NONE:
		return {};
	}
	return "SystemOne request failed";
}

} // namespace

RequestCompletion::RequestCompletion(std::future<TransportResult> future_p,
	                                   std::shared_ptr<void> admission_reservation_p)
	: future(std::move(future_p)), admission_reservation(std::move(admission_reservation_p)) {
}

RequestCompletion::RequestCompletion(RequestCompletion &&other) noexcept = default;

RequestCompletion &RequestCompletion::operator=(RequestCompletion &&other) noexcept = default;

bool RequestCompletion::valid() const {
	return future.valid();
}

void RequestCompletion::wait() {
	future.wait();
}

TransportResult RequestCompletion::get() {
	return future.get();
}

RequestHandle::RequestHandle(uint64_t id_p, RequestCompletion completion_p)
	: completion(std::move(completion_p)), id(id_p) {
}

RequestHandle::RequestHandle(RequestHandle &&other) noexcept
	: completion(std::move(other.completion)), id(std::exchange(other.id, 0)) {
}

RequestHandle &RequestHandle::operator=(RequestHandle &&other) noexcept {
	if (this != &other) {
		completion = std::move(other.completion);
		id = std::exchange(other.id, 0);
	}
	return *this;
}

bool RequestHandle::IsValid() const {
	return id != 0;
}

uint64_t RequestHandle::Id() const {
	return id;
}

class TransportDispatcher::Impl : public std::enable_shared_from_this<TransportDispatcher::Impl> {
public:
	explicit Impl(SettingsSnapshot settings_p) : settings(std::move(settings_p)) {
		for (auto &loop : loops) {
			loop.multi = curl_multi_init();
			if (loop.multi == nullptr) {
				for (auto &allocated : loops) {
					if (allocated.multi != nullptr) {
						curl_multi_cleanup(allocated.multi);
						allocated.multi = nullptr;
					}
				}
				throw std::runtime_error("could not create a libcurl event loop");
			}
			const auto max_connections = static_cast<long>(std::min<uint64_t>(
			    std::max<uint64_t>(settings.max_inflight, 1), static_cast<uint64_t>(std::numeric_limits<long>::max())));
			curl_multi_setopt(loop.multi, CURLMOPT_MAXCONNECTS, max_connections);
		}
		try {
			for (std::size_t index = 0; index < EVENT_LOOP_COUNT; ++index) {
				loops[index].thread = std::thread([this, index]() { EventLoop(index); });
			}
		} catch (...) {
			{
				std::lock_guard<std::mutex> guard(mutex);
				closing = true;
			}
			loop_cv.notify_all();
			WakeCurlLoops();
			for (auto &loop : loops) {
				if (loop.thread.joinable()) {
					loop.thread.join();
				} else if (loop.multi != nullptr) {
					curl_multi_cleanup(loop.multi);
					loop.multi = nullptr;
				}
			}
			throw;
		}
	}

	~Impl() {
		CancelAll();
		for (auto &loop : loops) {
			if (loop.thread.joinable()) {
				loop.thread.join();
			}
			if (loop.multi != nullptr) {
				curl_multi_cleanup(loop.multi);
				loop.multi = nullptr;
			}
		}
	}

	RequestHandle Submit(uint64_t slot, OwnedRequest request,
	                     const std::shared_ptr<std::atomic<bool>> &admission_cancelled) {
		auto job = std::make_shared<Job>(this, slot, std::move(request));
		auto completion = job->promise.get_future();
		auto permit_taken = std::make_shared<bool>(false);
		std::weak_ptr<Impl> weak_owner = shared_from_this();
		std::shared_ptr<void> admission_reservation(permit_taken.get(), [permit_taken, weak_owner](void *) {
			if (*permit_taken) {
				if (auto owner = weak_owner.lock()) {
					owner->ReleaseAdmissionPermit();
				}
			}
		});
		std::unique_lock<std::mutex> lock(mutex);
		while (!closing && !IsAdmissionCancelled(admission_cancelled) && !CanAdmitLocked()) {
			admission_cv.wait_for(lock, std::chrono::milliseconds(10));
		}
		if (IsAdmissionCancelled(admission_cancelled)) {
			lock.unlock();
			job->promise.set_value(ErrorResult(slot, TransportErrorCategory::CANCELLED,
			                                   "SystemOne request admission was cancelled"));
			return RequestHandle(0, RequestCompletion(std::move(completion)));
		}
		if (closing) {
			lock.unlock();
			job->promise.set_value(ErrorResult(slot, TransportErrorCategory::DISPATCHER_CLOSED,
			                                   DiagnosticFor(TransportErrorCategory::DISPATCHER_CLOSED)));
			return RequestHandle(0, RequestCompletion(std::move(completion)));
		}

		job->id = next_id++;
		if (job->id == 0) {
			job->id = next_id++;
		}
		job->enqueued_at = Clock::now();
		jobs.emplace(job->id, job);
		if (active_slots < settings.max_inflight) {
			ReserveForLoopLocked(job);
		} else {
			job->location = JobLocation::WAITING;
			pending.push_back(job);
			stats.queued = static_cast<uint64_t>(pending.size());
		}
		++accepted_unreleased;
		*permit_taken = true;
		const auto id = job->id;
		lock.unlock();
		loop_cv.notify_all();
		WakeCurlLoops();
		return RequestHandle(id, RequestCompletion(std::move(completion), std::move(admission_reservation)));
	}

	void Cancel(const RequestHandle &handle) noexcept {
		if (handle.Id() == 0) {
			return;
		}
		std::shared_ptr<Job> finish_now;
		TransportResult result;
		{
			std::lock_guard<std::mutex> guard(mutex);
			const auto found = jobs.find(handle.Id());
			if (found == jobs.end()) {
				return;
			}
			auto job = found->second;
			job->cancel_requested.store(true, std::memory_order_relaxed);
			if (job->location == JobLocation::WAITING) {
				EraseJob(pending, job);
				stats.queued = static_cast<uint64_t>(pending.size());
				job->location = JobLocation::FINISHED;
				jobs.erase(found);
				result = BuildResult(job, TransportErrorCategory::CANCELLED,
				                     DiagnosticFor(TransportErrorCategory::CANCELLED));
				RecordCompletionLocked(result);
				finish_now = std::move(job);
			} else if (job->location == JobLocation::INBOX) {
				EraseJob(loops[job->loop_index].inbox, job);
				ReleaseActiveSlotLocked();
				job->location = JobLocation::FINISHED;
				jobs.erase(found);
				result = BuildResult(job, TransportErrorCategory::CANCELLED,
				                     DiagnosticFor(TransportErrorCategory::CANCELLED));
				RecordCompletionLocked(result);
				finish_now = std::move(job);
				PromotePendingLocked();
			}
		}
		if (finish_now != nullptr) {
			SetPromise(finish_now, std::move(result));
			admission_cv.notify_all();
		}
		loop_cv.notify_all();
		WakeCurlLoops();
	}

	void CancelAll() noexcept {
		std::vector<std::pair<std::shared_ptr<Job>, TransportResult>> finish_now;
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (closing) {
				return;
			}
			closing = true;
			finish_now.reserve(pending.size() + loops[0].inbox.size() + loops[1].inbox.size());
			while (!pending.empty()) {
				auto job = pending.front();
				pending.pop_front();
				job->location = JobLocation::FINISHED;
				job->cancel_requested.store(true, std::memory_order_relaxed);
				jobs.erase(job->id);
				auto result = BuildResult(job, TransportErrorCategory::CANCELLED,
				                          DiagnosticFor(TransportErrorCategory::CANCELLED));
				RecordCompletionLocked(result);
				finish_now.emplace_back(std::move(job), std::move(result));
			}
			stats.queued = 0;
			for (auto &loop : loops) {
				while (!loop.inbox.empty()) {
					auto job = loop.inbox.front();
					loop.inbox.pop_front();
					job->location = JobLocation::FINISHED;
					job->cancel_requested.store(true, std::memory_order_relaxed);
					jobs.erase(job->id);
					ReleaseActiveSlotLocked();
					auto result = BuildResult(job, TransportErrorCategory::CANCELLED,
					                          DiagnosticFor(TransportErrorCategory::CANCELLED));
					RecordCompletionLocked(result);
					finish_now.emplace_back(std::move(job), std::move(result));
				}
			}
			for (auto &entry : jobs) {
				entry.second->cancel_requested.store(true, std::memory_order_relaxed);
			}
		}
		for (auto &entry : finish_now) {
			SetPromise(entry.first, std::move(entry.second));
		}
		admission_cv.notify_all();
		loop_cv.notify_all();
		WakeCurlLoops();
	}

	TransportStats StatsSnapshot() const {
		std::lock_guard<std::mutex> guard(mutex);
		auto result = stats;
		const auto admitted_in_dispatcher = active_slots + static_cast<uint64_t>(pending.size());
		result.completed_pending = accepted_unreleased > admitted_in_dispatcher
		                               ? accepted_unreleased - admitted_in_dispatcher
		                               : 0;
		return result;
	}

private:
	enum class JobLocation : uint8_t {
		NEW,
		WAITING,
		INBOX,
		STARTING,
		ACTIVE,
		FINISHED,
	};

	struct Job {
		Job(Impl *owner_p, uint64_t slot_p, OwnedRequest request_p)
		    : owner(owner_p), slot(slot_p), request(std::move(request_p)) {
		}

		Impl *owner;
		uint64_t id = 0;
		uint64_t slot;
		OwnedRequest request;
		std::promise<TransportResult> promise;
		std::atomic<bool> cancel_requested {false};
		JobLocation location = JobLocation::NEW;
		std::size_t loop_index = 0;
		Clock::time_point enqueued_at;
		Clock::time_point started_at;
		bool has_started = false;
		uint64_t queue_wait_ms = 0;
		uint64_t retries = 0;
		uint64_t total_response_bytes = 0;
		uint64_t attempt_response_bytes = 0;
		std::string response_body;
		bool response_too_large = false;
		bool response_buffer_error = false;
		CURL *easy = nullptr;
		curl_slist *headers = nullptr;
		std::array<char, CURL_ERROR_SIZE> error_buffer {};
	};

	struct LoopState {
		CURLM *multi = nullptr;
		std::thread thread;
		std::deque<std::shared_ptr<Job>> inbox;
	};

	bool IsAdmissionCancelled(const std::shared_ptr<std::atomic<bool>> &token) const {
		return token != nullptr && token->load(std::memory_order_relaxed);
	}

	bool CanAdmitLocked() const {
		const auto accepted_limit = SaturatingAdd(settings.max_inflight, settings.queue_capacity);
		return accepted_unreleased < accepted_limit &&
		       (active_slots < settings.max_inflight || static_cast<uint64_t>(pending.size()) < settings.queue_capacity);
	}

	void ReleaseAdmissionPermit() noexcept {
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (accepted_unreleased > 0) {
				--accepted_unreleased;
			}
		}
		admission_cv.notify_all();
	}

	void ReserveForLoopLocked(const std::shared_ptr<Job> &job) {
		const std::size_t loop_index = next_loop++ % EVENT_LOOP_COUNT;
		job->loop_index = loop_index;
		job->location = JobLocation::INBOX;
		loops[loop_index].inbox.push_back(job);
		++active_slots;
		stats.active = active_slots;
	}

	void PromotePendingLocked() {
		while (!closing && active_slots < settings.max_inflight && !pending.empty()) {
			auto job = pending.front();
			pending.pop_front();
			stats.queued = static_cast<uint64_t>(pending.size());
			ReserveForLoopLocked(job);
		}
	}

	void ReleaseActiveSlotLocked() {
		if (active_slots > 0) {
			--active_slots;
		}
		stats.active = active_slots;
	}

	template <class Container>
	static void EraseJob(Container &container, const std::shared_ptr<Job> &job) {
		const auto found = std::find(container.begin(), container.end(), job);
		if (found != container.end()) {
			container.erase(found);
		}
	}

	TransportResult BuildResult(const std::shared_ptr<Job> &job, TransportErrorCategory category,
	                            std::string diagnostic, long status = 0) const {
		auto result = ErrorResult(job->slot, category, std::move(diagnostic));
		result.http_status = status;
		result.response_bytes = job->total_response_bytes;
		result.queue_wait_ms = job->has_started ? job->queue_wait_ms : Milliseconds(Clock::now() - job->enqueued_at);
		result.request_duration_ms = job->has_started ? Milliseconds(Clock::now() - job->started_at) : 0;
		result.retries = job->retries;
		return result;
	}

	void RecordCompletionLocked(const TransportResult &result) {
		++stats.completed;
		stats.queue_wait_ms = SaturatingAdd(stats.queue_wait_ms, result.queue_wait_ms);
		stats.request_duration_ms = SaturatingAdd(stats.request_duration_ms, result.request_duration_ms);
		stats.response_bytes = SaturatingAdd(stats.response_bytes, result.response_bytes);
		stats.retry_count = SaturatingAdd(stats.retry_count, result.retries);
		if (result.error == TransportErrorCategory::TIMEOUT) {
			++stats.timeout_count;
		}
		if (result.error == TransportErrorCategory::CANCELLED) {
			++stats.cancelled_count;
		} else if (result.error != TransportErrorCategory::NONE &&
		           result.error != TransportErrorCategory::DISPATCHER_CLOSED) {
			++stats.error_count;
		}
		const auto category_index = static_cast<std::size_t>(result.error);
		if (result.error != TransportErrorCategory::NONE && category_index < stats.errors_by_category.size()) {
			++stats.errors_by_category[category_index];
		}
	}

	static void SetPromise(const std::shared_ptr<Job> &job, TransportResult result) noexcept {
		try {
			job->promise.set_value(std::move(result));
		} catch (...) {
			// A job is resolved under the dispatcher mutex exactly once. This is
			// defensive against a broken promise state, and must not escape a CURL
			// callback or event-loop thread.
		}
	}

	void CompleteFromLoop(const std::shared_ptr<Job> &job, TransportResult result) {
		bool should_set = false;
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (job->location == JobLocation::FINISHED) {
				return;
			}
			if (job->location == JobLocation::ACTIVE || job->location == JobLocation::STARTING ||
			    job->location == JobLocation::INBOX) {
				ReleaseActiveSlotLocked();
			}
			job->location = JobLocation::FINISHED;
			jobs.erase(job->id);
			RecordCompletionLocked(result);
			PromotePendingLocked();
			should_set = true;
		}
		if (should_set) {
			SetPromise(job, std::move(result));
		}
		admission_cv.notify_all();
		loop_cv.notify_all();
		WakeCurlLoops();
	}

	std::vector<std::shared_ptr<Job>> TakeInbox(std::size_t loop_index) {
		std::vector<std::shared_ptr<Job>> result;
		{
			std::lock_guard<std::mutex> guard(mutex);
			auto &inbox = loops[loop_index].inbox;
			result.reserve(inbox.size());
			while (!inbox.empty()) {
				auto job = inbox.front();
				inbox.pop_front();
				if (job->location == JobLocation::INBOX) {
					job->location = JobLocation::STARTING;
					result.push_back(std::move(job));
				}
			}
		}
		return result;
	}

	size_t WriteResponse(Job &job, const char *data, size_t size, size_t count) noexcept {
		if (size != 0 && count > std::numeric_limits<size_t>::max() / size) {
			job.response_buffer_error = true;
			return 0;
		}
		const auto bytes = size * count;
		if (bytes == 0) {
			return 0;
		}
		job.attempt_response_bytes = SaturatingAdd(job.attempt_response_bytes, static_cast<uint64_t>(bytes));
		job.total_response_bytes = SaturatingAdd(job.total_response_bytes, static_cast<uint64_t>(bytes));
		if (static_cast<uint64_t>(job.response_body.size()) > settings.max_response_bytes ||
		    static_cast<uint64_t>(bytes) > settings.max_response_bytes - static_cast<uint64_t>(job.response_body.size())) {
			job.response_too_large = true;
			return 0;
		}
		try {
			job.response_body.append(data, bytes);
		} catch (...) {
			job.response_buffer_error = true;
			return 0;
		}
		return bytes;
	}

	static size_t WriteResponseCallback(char *data, size_t size, size_t count, void *userdata) noexcept {
		auto *job = static_cast<Job *>(userdata);
		if (job == nullptr) {
			return 0;
		}
		// The response callback is invoked only by the owning event-loop thread.
		// Keep all response bytes in the request-owned buffer, with a hard cap.
		return job->owner->WriteResponse(*job, data, size, count);
	}

	void ResetAttempt(const std::shared_ptr<Job> &job) {
		job->response_body.clear();
		job->attempt_response_bytes = 0;
		job->response_too_large = false;
		job->response_buffer_error = false;
		job->error_buffer.fill('\0');
	}

	uint64_t RemainingRequestTimeoutMs(const std::shared_ptr<Job> &job) const {
		if (!job->has_started) {
			return settings.request_timeout_ms;
		}
		const auto elapsed = Clock::now() - job->started_at;
		auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
		if (std::chrono::duration_cast<Clock::duration>(elapsed_ms) < elapsed) {
			elapsed_ms += std::chrono::milliseconds(1);
		}
		const auto elapsed_count = elapsed_ms.count();
		if (elapsed_count <= 0) {
			return settings.request_timeout_ms;
		}
		const auto elapsed_unsigned = static_cast<uint64_t>(elapsed_count);
		if (elapsed_unsigned >= settings.request_timeout_ms) {
			return 0;
		}
		return settings.request_timeout_ms - elapsed_unsigned;
	}

	bool SetEasyOptions(const std::shared_ptr<Job> &job, uint64_t remaining_timeout_ms) {
		job->easy = curl_easy_init();
		if (job->easy == nullptr) {
			return false;
		}
		for (const char *header : {"Content-Type: application/json", "Accept: application/json",
		                           "Connection: keep-alive"}) {
			auto *new_headers = curl_slist_append(job->headers, header);
			if (new_headers == nullptr) {
				return false;
			}
			job->headers = new_headers;
		}
		const auto connect_timeout = static_cast<long>(std::min(settings.connect_timeout_ms, remaining_timeout_ms));
		const auto request_timeout = static_cast<long>(remaining_timeout_ms);
		if (job->request.body.size() > static_cast<std::size_t>(std::numeric_limits<curl_off_t>::max())) {
			return false;
		}
		bool options_ok = true;
		auto set_option = [&](CURLoption option, auto value) {
			if (curl_easy_setopt(job->easy, option, value) != CURLE_OK) {
				options_ok = false;
			}
		};
		set_option(CURLOPT_URL, settings.endpoint.c_str());
		set_option(CURLOPT_POST, 1L);
		set_option(CURLOPT_POSTFIELDS, job->request.body.data());
		set_option(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(job->request.body.size()));
		set_option(CURLOPT_HTTPHEADER, job->headers);
		set_option(CURLOPT_WRITEFUNCTION, &WriteResponseCallback);
		set_option(CURLOPT_WRITEDATA, job.get());
		set_option(CURLOPT_ERRORBUFFER, job->error_buffer.data());
		set_option(CURLOPT_FOLLOWLOCATION, 0L);
		set_option(CURLOPT_MAXREDIRS, 0L);
		set_option(CURLOPT_CONNECTTIMEOUT_MS, connect_timeout);
		set_option(CURLOPT_TIMEOUT_MS, request_timeout);
		set_option(CURLOPT_NOSIGNAL, 1L);
		set_option(CURLOPT_TCP_KEEPALIVE, 1L);
		set_option(CURLOPT_FORBID_REUSE, 0L);
		set_option(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
		set_option(CURLOPT_VERBOSE, 0L);
		return options_ok;
	}

	void CleanupEasy(const std::shared_ptr<Job> &job) {
		if (job->easy != nullptr) {
			curl_easy_cleanup(job->easy);
			job->easy = nullptr;
		}
		if (job->headers != nullptr) {
			curl_slist_free_all(job->headers);
			job->headers = nullptr;
		}
	}

	void StartJob(std::size_t loop_index, CURLM *multi, const std::shared_ptr<Job> &job,
	              std::unordered_map<CURL *, std::shared_ptr<Job>> &active) {
		if (job->cancel_requested.load(std::memory_order_relaxed)) {
			CompleteFromLoop(job, BuildResult(job, TransportErrorCategory::CANCELLED,
			                                  DiagnosticFor(TransportErrorCategory::CANCELLED)));
			return;
		}
		if (!job->has_started) {
			job->has_started = true;
			job->started_at = Clock::now();
			job->queue_wait_ms = Milliseconds(job->started_at - job->enqueued_at);
		}
		const auto remaining_timeout = RemainingRequestTimeoutMs(job);
		if (remaining_timeout == 0) {
			CompleteFromLoop(job, BuildResult(job, TransportErrorCategory::TIMEOUT,
			                                  DiagnosticFor(TransportErrorCategory::TIMEOUT)));
			return;
		}
		ResetAttempt(job);
		if (!SetEasyOptions(job, remaining_timeout)) {
			CleanupEasy(job);
			CompleteFromLoop(job, BuildResult(job, TransportErrorCategory::CURL_FAILURE,
			                                  DiagnosticFor(TransportErrorCategory::CURL_FAILURE)));
			return;
		}
		if (job->cancel_requested.load(std::memory_order_relaxed)) {
			CleanupEasy(job);
			CompleteFromLoop(job, BuildResult(job, TransportErrorCategory::CANCELLED,
			                                  DiagnosticFor(TransportErrorCategory::CANCELLED)));
			return;
		}
		if (curl_multi_add_handle(multi, job->easy) != CURLM_OK) {
			CleanupEasy(job);
			CompleteFromLoop(job, BuildResult(job, TransportErrorCategory::CURL_FAILURE,
			                                  DiagnosticFor(TransportErrorCategory::CURL_FAILURE)));
			return;
		}
		job->loop_index = loop_index;
		active.emplace(job->easy, job);
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (job->location == JobLocation::STARTING) {
				job->location = JobLocation::ACTIVE;
			}
		}
	}

	bool IsRetryablePreUpload(CURLcode result, bool upload_count_available, curl_off_t uploaded,
	                           const std::shared_ptr<Job> &job) const {
		if (job->retries >= settings.retry_count || !upload_count_available || uploaded != 0 ||
		    job->cancel_requested.load(std::memory_order_relaxed)) {
			return false;
		}
		switch (result) {
		case CURLE_COULDNT_RESOLVE_HOST:
		case CURLE_COULDNT_RESOLVE_PROXY:
		case CURLE_COULDNT_CONNECT:
		case CURLE_SSL_CONNECT_ERROR:
			return true;
		default:
			return false;
		}
	}

	TransportErrorCategory ClassifyCurlError(CURLcode result) const {
		if (result == CURLE_OPERATION_TIMEDOUT) {
			return TransportErrorCategory::TIMEOUT;
		}
		if (result == CURLE_COULDNT_CONNECT) {
			return TransportErrorCategory::CONNECTION_REFUSED;
		}
		if (result == CURLE_COULDNT_RESOLVE_HOST || result == CURLE_COULDNT_RESOLVE_PROXY ||
		    result == CURLE_SSL_CONNECT_ERROR) {
			return TransportErrorCategory::CONNECTION_FAILURE;
		}
		return TransportErrorCategory::CURL_FAILURE;
	}

	bool RetryOrFinish(std::size_t loop_index, CURLM *multi, CURL *easy, const std::shared_ptr<Job> &job,
	                   CURLcode curl_result, long http_status,
	                   std::unordered_map<CURL *, std::shared_ptr<Job>> &active) {
		curl_off_t uploaded = -1;
		const bool upload_count_available =
		    curl_easy_getinfo(easy, CURLINFO_SIZE_UPLOAD_T, &uploaded) == CURLE_OK;
		curl_multi_remove_handle(multi, easy);
		active.erase(easy);
		CleanupEasy(job);

		if (job->cancel_requested.load(std::memory_order_relaxed)) {
			CompleteFromLoop(job, BuildResult(job, TransportErrorCategory::CANCELLED,
			                                  DiagnosticFor(TransportErrorCategory::CANCELLED), http_status));
			return false;
		}
		if (job->response_too_large) {
			CompleteFromLoop(job, BuildResult(job, TransportErrorCategory::RESPONSE_TOO_LARGE,
			                                  DiagnosticFor(TransportErrorCategory::RESPONSE_TOO_LARGE), http_status));
			return false;
		}
		if (job->response_buffer_error) {
			CompleteFromLoop(job, BuildResult(job, TransportErrorCategory::CURL_FAILURE,
			                                  DiagnosticFor(TransportErrorCategory::CURL_FAILURE), http_status));
			return false;
		}
		if (curl_result != CURLE_OK && IsRetryablePreUpload(curl_result, upload_count_available, uploaded, job)) {
			++job->retries;
			ResetAttempt(job);
			{
				std::lock_guard<std::mutex> guard(mutex);
				if (job->location == JobLocation::ACTIVE) {
					job->location = JobLocation::STARTING;
				}
			}
			StartJob(loop_index, multi, job, active);
			return true;
		}
		if (curl_result != CURLE_OK) {
			const auto category = ClassifyCurlError(curl_result);
			CompleteFromLoop(job, BuildResult(job, category, DiagnosticFor(category), http_status));
			return false;
		}
		if (http_status < 200 || http_status >= 300) {
			const auto category = ClassifyHttpError(http_status, job->response_body);
			CompleteFromLoop(job, BuildResult(job, category, DiagnosticFor(category, http_status), http_status));
			return false;
		}
		try {
			auto parsed = ValidateResponse(job->response_body, job->request.questions);
			auto result = BuildResult(job, TransportErrorCategory::NONE, {}, http_status);
			result.success = true;
			result.response = std::move(parsed);
			CompleteFromLoop(job, std::move(result));
		} catch (const std::exception &) {
			CompleteFromLoop(job, BuildResult(job, TransportErrorCategory::INVALID_RESPONSE,
			                                  DiagnosticFor(TransportErrorCategory::INVALID_RESPONSE), http_status));
		}
		return false;
	}

	void RemoveCancelled(std::size_t loop_index, CURLM *multi,
	                     std::unordered_map<CURL *, std::shared_ptr<Job>> &active) {
		std::vector<std::pair<CURL *, std::shared_ptr<Job>>> cancelled;
		for (const auto &entry : active) {
			if (closing || entry.second->cancel_requested.load(std::memory_order_relaxed)) {
				cancelled.push_back(entry);
			}
		}
		for (auto &entry : cancelled) {
			curl_multi_remove_handle(multi, entry.first);
			active.erase(entry.first);
			CleanupEasy(entry.second);
			CompleteFromLoop(entry.second, BuildResult(entry.second, TransportErrorCategory::CANCELLED,
			                                           DiagnosticFor(TransportErrorCategory::CANCELLED)));
		}
		(void)loop_index;
	}

	void ProcessMessages(std::size_t loop_index, CURLM *multi,
	                     std::unordered_map<CURL *, std::shared_ptr<Job>> &active) {
		int messages_left = 0;
		while (auto *message = curl_multi_info_read(multi, &messages_left)) {
			if (message->msg != CURLMSG_DONE) {
				continue;
			}
			const auto found = active.find(message->easy_handle);
			if (found == active.end()) {
				curl_multi_remove_handle(multi, message->easy_handle);
				continue;
			}
			long status = 0;
			curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &status);
			auto job = found->second;
			RetryOrFinish(loop_index, multi, message->easy_handle, job, message->data.result, status, active);
		}
	}

	void EventLoop(std::size_t loop_index) {
		auto *multi = loops[loop_index].multi;
		std::unordered_map<CURL *, std::shared_ptr<Job>> active;
		bool fatal_multi_error = false;
		for (;;) {
			auto incoming = TakeInbox(loop_index);
			for (const auto &job : incoming) {
				StartJob(loop_index, multi, job, active);
			}
			RemoveCancelled(loop_index, multi, active);

			int running = 0;
			CURLMcode multi_result;
			do {
				multi_result = curl_multi_perform(multi, &running);
			} while (multi_result == CURLM_CALL_MULTI_PERFORM);
			if (multi_result != CURLM_OK) {
				fatal_multi_error = true;
				break;
			}
			ProcessMessages(loop_index, multi, active);
			RemoveCancelled(loop_index, multi, active);

			bool should_exit = false;
			{
				std::lock_guard<std::mutex> guard(mutex);
				should_exit = closing && active.empty() && loops[loop_index].inbox.empty();
			}
			if (should_exit) {
				break;
			}
			if (active.empty()) {
				std::unique_lock<std::mutex> lock(mutex);
				loop_cv.wait_for(lock, std::chrono::milliseconds(100), [this, loop_index]() {
					return closing || !loops[loop_index].inbox.empty();
				});
			} else {
				int descriptors = 0;
				const auto poll_result = curl_multi_poll(multi, nullptr, 0, 1000, &descriptors);
				if (poll_result != CURLM_OK) {
					fatal_multi_error = true;
					break;
				}
			}
		}

		if (fatal_multi_error) {
			// A failed CURLM cannot safely be reused. Close admission and cancel
			// queued work plus transfers owned by the other event loop; this loop
			// reports its own active transfers as CURL_FAILURE below.
			CancelAll();
		}
		for (auto &entry : active) {
			curl_multi_remove_handle(multi, entry.first);
			CleanupEasy(entry.second);
			const auto category = fatal_multi_error ? TransportErrorCategory::CURL_FAILURE
			                                        : TransportErrorCategory::CANCELLED;
			CompleteFromLoop(entry.second, BuildResult(entry.second, category, DiagnosticFor(category)));
		}
		active.clear();
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (loops[loop_index].multi == multi) {
				loops[loop_index].multi = nullptr;
			}
		}
		curl_multi_cleanup(multi);
	}

	void WakeCurlLoops() noexcept {
		std::lock_guard<std::mutex> guard(mutex);
		for (auto &loop : loops) {
			if (loop.multi != nullptr) {
				curl_multi_wakeup(loop.multi);
			}
		}
	}

	SettingsSnapshot settings;
	mutable std::mutex mutex;
	std::condition_variable admission_cv;
	std::condition_variable loop_cv;
	std::array<LoopState, EVENT_LOOP_COUNT> loops;
	std::deque<std::shared_ptr<Job>> pending;
	std::unordered_map<uint64_t, std::shared_ptr<Job>> jobs;
	TransportStats stats;
	uint64_t active_slots = 0;
	uint64_t next_id = 1;
	std::size_t next_loop = 0;
	std::atomic<bool> closing {false};
	uint64_t accepted_unreleased = 0;
};

TransportDispatcher::TransportDispatcher(SettingsSnapshot settings)
	: impl(std::make_shared<Impl>(std::move(settings))) {
}

TransportDispatcher::~TransportDispatcher() = default;

RequestHandle TransportDispatcher::Submit(uint64_t slot, OwnedRequest request,
	                                         std::shared_ptr<std::atomic<bool>> admission_cancelled) {
	return impl->Submit(slot, std::move(request), admission_cancelled);
}

void TransportDispatcher::Cancel(const RequestHandle &handle) noexcept {
	impl->Cancel(handle);
}

void TransportDispatcher::Cancel() noexcept {
	impl->CancelAll();
}

TransportStats TransportDispatcher::StatsSnapshot() const {
	return impl->StatsSnapshot();
}

} // namespace systemone
} // namespace duckdb
