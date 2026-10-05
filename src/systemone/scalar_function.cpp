#include "systemone/config.hpp"
#include "systemone/request.hpp"
#include "systemone/transport.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace duckdb {
namespace systemone {
namespace {

std::atomic<uint64_t> next_expression_id {1};
constexpr const char *QUERY_STATE_KEY = "ollama_systemone_query_state";

uint64_t NextExpressionId() {
	auto result = next_expression_id.fetch_add(1, std::memory_order_relaxed);
	while (result == 0) {
		result = next_expression_id.fetch_add(1, std::memory_order_relaxed);
	}
	return result;
}

uint64_t SaturatingAdd(uint64_t left, uint64_t right) {
	if (right > std::numeric_limits<uint64_t>::max() - left) {
		return std::numeric_limits<uint64_t>::max();
	}
	return left + right;
}

struct SystemOneBindData final : FunctionData {
	explicit SystemOneBindData(uint64_t expression_id_p) : expression_id(expression_id_p) {
	}

	uint64_t expression_id;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<SystemOneBindData>(expression_id);
	}

	bool Equals(const FunctionData &other_p) const override {
		return expression_id == other_p.Cast<SystemOneBindData>().expression_id;
	}
};

struct DispatcherRuntime {
	std::shared_ptr<TransportDispatcher> dispatcher;
	std::shared_ptr<const SettingsSnapshot> settings;
	uint64_t max_pending = 1;
};

class SystemOneQueryState final : public ClientContextState {
public:
	DispatcherRuntime GetOrCreateDispatcher(uint64_t expression_id, ClientContext &context) {
		std::lock_guard<std::mutex> guard(lock);
		auto found = dispatchers.find(expression_id);
		if (found != dispatchers.end()) {
			return found->second;
		}
		auto settings = std::make_shared<SettingsSnapshot>(ReadSettingsSnapshot(context));
		const auto max_pending = std::max<uint64_t>(1, SaturatingAdd(settings->max_inflight, settings->queue_capacity));
		DispatcherRuntime runtime {std::make_shared<TransportDispatcher>(*settings), std::move(settings), max_pending};
		dispatchers.emplace(expression_id, runtime);
		return runtime;
	}

	void QueryEnd(ClientContext &) override {
		std::vector<std::shared_ptr<TransportDispatcher>> to_cancel;
		{
			std::lock_guard<std::mutex> guard(lock);
			to_cancel.reserve(dispatchers.size());
			for (auto &entry : dispatchers) {
				to_cancel.push_back(entry.second.dispatcher);
			}
			dispatchers.clear();
		}
		for (auto &dispatcher : to_cancel) {
			dispatcher->Cancel();
		}
	}

private:
	std::mutex lock;
	std::unordered_map<uint64_t, DispatcherRuntime> dispatchers;
};

struct PendingRequest {
	idx_t slot;
	RequestHandle handle;
};

struct InvocationFailure {
	idx_t slot;
	std::string diagnostic;
	bool invalid_input = false;
};

std::string SlotDiagnostic(idx_t slot, const std::string &cause) {
	return "ollama_systemone slot " + std::to_string(slot + 1) + ": " + cause;
}

[[noreturn]] void ThrowFailure(const InvocationFailure &failure) {
	const auto diagnostic = SlotDiagnostic(failure.slot, failure.diagnostic);
	if (failure.invalid_input) {
		throw InvalidInputException(diagnostic);
	}
	throw IOException(diagnostic);
}

void OllamaSystemOneFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto count = args.size();
	if (count == 0) {
		return;
	}

	const auto &function_expression = state.expr.Cast<BoundFunctionExpression>();
	const auto &bind_data = function_expression.bind_info->Cast<SystemOneBindData>();
	auto &context = state.GetContext();
	auto query_state = context.registered_state->GetOrCreate<SystemOneQueryState>(QUERY_STATE_KEY);
	auto runtime = query_state->GetOrCreateDispatcher(bind_data.expression_id, context);

	UnifiedVectorFormat state_format;
	UnifiedVectorFormat config_format;
	args.data[0].ToUnifiedFormat(count, state_format);
	args.data[1].ToUnifiedFormat(count, config_format);
	const auto states = UnifiedVectorFormat::GetData<string_t>(state_format);
	const auto configs = UnifiedVectorFormat::GetData<string_t>(config_format);

	result.Flatten(count);
	auto output = FlatVector::GetData<string_t>(result);
	auto &output_validity = FlatVector::Validity(result);
	output_validity.EnsureWritable();

	std::unordered_map<std::string, ValidatedConfig> config_cache;
	std::vector<PendingRequest> pending;
	pending.reserve(static_cast<std::size_t>(std::min<uint64_t>(runtime.max_pending, 4096)));
	std::optional<InvocationFailure> failure;
	idx_t next_slot = 0;
	bool interrupted_during_admission = false;

	auto cancel_higher_slots = [&]() {
		if (!failure) {
			return;
		}
		for (auto &entry : pending) {
			if (entry.slot > failure->slot) {
				runtime.dispatcher->Cancel(entry.handle);
			}
		}
	};

	auto record_failure = [&](idx_t slot, std::string diagnostic, bool invalid_input) {
		if (!failure || slot < failure->slot) {
			failure = InvocationFailure {slot, std::move(diagnostic), invalid_input};
			cancel_higher_slots();
		}
	};

	auto store_response = [&](idx_t slot, const TransportResult &transport_result) {
		try {
			const auto response_json = transport_result.response.dump();
			output[slot] = StringVector::AddString(result, response_json);
			output_validity.SetValid(slot);
		} catch (const std::exception &error) {
			record_failure(slot, std::string("validated response could not be stored as JSON: ") + error.what(), false);
		}
	};

	auto collect_ready = [&]() {
		bool collected = false;
		for (idx_t index = 0; index < pending.size();) {
			auto &entry = pending[index];
			if (entry.handle.completion.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
				++index;
				continue;
			}

			collected = true;
			const auto slot = entry.slot;
			auto transport_result = entry.handle.completion.get();
			if (transport_result.success) {
				if (!failure || slot < failure->slot) {
					store_response(slot, transport_result);
				}
			} else if (transport_result.error != TransportErrorCategory::CANCELLED || !failure ||
			           slot < failure->slot) {
				const auto diagnostic = transport_result.diagnostic.empty() ? std::string("request failed")
				                                                           : transport_result.diagnostic;
				record_failure(slot, diagnostic, false);
			}
			pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
		}
		cancel_higher_slots();
		return collected;
	};

	auto cancel_and_drain = [&]() {
		for (auto &entry : pending) {
			runtime.dispatcher->Cancel(entry.handle);
		}
		while (!pending.empty()) {
			bool collected = false;
			for (idx_t index = 0; index < pending.size();) {
				auto &entry = pending[index];
				if (entry.handle.completion.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
					++index;
					continue;
				}
				try {
					(void)entry.handle.completion.get();
				} catch (...) {
				}
				pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
				collected = true;
			}
			if (!collected && !pending.empty()) {
				pending.front().handle.completion.wait_for(std::chrono::milliseconds(10));
			}
		}
	};

	auto throw_if_interrupted = [&]() {
		if (context.IsInterrupted() || interrupted_during_admission) {
			cancel_and_drain();
			throw InterruptException();
		}
	};

	while (next_slot < count || !pending.empty()) {
		throw_if_interrupted();
		collect_ready();
		throw_if_interrupted();

		if (failure) {
			if (pending.empty()) {
				break;
			}
			pending.front().handle.completion.wait_for(std::chrono::milliseconds(10));
			continue;
		}

		if (next_slot >= count || static_cast<uint64_t>(pending.size()) >= runtime.max_pending) {
			if (!pending.empty()) {
				pending.front().handle.completion.wait_for(std::chrono::milliseconds(10));
			}
			continue;
		}

		const auto slot = next_slot;
		const auto state_index = state_format.sel->get_index(slot);
		const auto config_index = config_format.sel->get_index(slot);
		const auto state_is_valid = state_format.validity.RowIsValid(state_index);
		const auto config_is_valid = config_format.validity.RowIsValid(config_index);
		if (!state_is_valid || !config_is_valid) {
			output_validity.SetInvalid(slot);
			++next_slot;
			continue;
		}

		const string_t state_value = states[state_index];
		const string_t config_value = configs[config_index];
		const std::string_view state_text(state_value.GetData(), state_value.GetSize());
		const std::string config_text(config_value.GetData(), config_value.GetSize());

		std::optional<OwnedRequest> owned_request;
		try {
			auto found = config_cache.find(config_text);
			if (found == config_cache.end()) {
				auto parsed = ParseConfig(config_text);
				found = config_cache.emplace(config_text, std::move(parsed)).first;
			}
			owned_request.emplace(BuildRequest(state_text, found->second, *runtime.settings));
		} catch (const std::invalid_argument &error) {
			record_failure(slot, error.what(), true);
			++next_slot;
			continue;
		}

		try {
			auto admission_cancelled = std::make_shared<std::atomic<bool>>(false);
			bool saw_ready_request = false;
			auto admission_cancel_check = [&]() {
				if (context.IsInterrupted()) {
					interrupted_during_admission = true;
					return true;
				}
				for (auto &entry : pending) {
					if (entry.handle.completion.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
						saw_ready_request = true;
						return true;
					}
				}
				return false;
			};
			auto handle = runtime.dispatcher->Submit(static_cast<uint64_t>(slot), std::move(*owned_request),
			                                         std::move(admission_cancelled), admission_cancel_check);
			if (!handle.IsValid()) {
				auto rejected = handle.completion.get();
				if (context.IsInterrupted() || interrupted_during_admission) {
					throw_if_interrupted();
				}
				if (saw_ready_request) {
					collect_ready();
					continue;
				}
				const auto diagnostic = rejected.diagnostic.empty() ? std::string("dispatcher is closed")
				                                                  : rejected.diagnostic;
				record_failure(slot, diagnostic, false);
				++next_slot;
				continue;
			}
			pending.push_back({slot, std::move(handle)});
			++next_slot;
		} catch (const InterruptException &) {
			throw_if_interrupted();
			throw;
		} catch (const std::exception &error) {
			record_failure(slot, error.what(), false);
			++next_slot;
		}
	}

	throw_if_interrupted();
	collect_ready();
	if (failure) {
		ThrowFailure(*failure);
	}
}

unique_ptr<FunctionData> BindSystemOneFunction(ClientContext &, ScalarFunction &,
                                               vector<unique_ptr<Expression>> &) {
	return make_uniq<SystemOneBindData>(NextExpressionId());
}

} // namespace

void RegisterScalarFunction(ExtensionLoader &loader) {
	ScalarFunction function("ollama_systemone", {LogicalType::VARCHAR, LogicalType::JSON()}, LogicalType::JSON(),
	                        OllamaSystemOneFunction, BindSystemOneFunction);
	function.SetVolatile();
	function.SetFallible();
	loader.RegisterFunction(function);
}

} // namespace systemone
} // namespace duckdb
