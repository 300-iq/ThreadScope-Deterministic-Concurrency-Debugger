#include "threadscope/trace.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace threadscope {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_trace_bytes = 8 * 1024 * 1024;

std::uint64_t natural(const Json& value, std::uint64_t maximum) {
    if (!value.is_number_integer() ||
        (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw std::invalid_argument("expected a non-negative integer");
    const auto number = value.get<std::uint64_t>();
    if (number > maximum) throw std::invalid_argument("integer exceeds trace limit");
    return number;
}
std::optional<int> optional_int(const Json& value) {
    if (value.is_null()) return {};
    if (!value.is_number_integer()) throw std::invalid_argument("observation must be an integer or null");
    if (value.is_number_unsigned()) {
        if (value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("observation out of int range");
    } else if (value.get<std::int64_t>() < std::numeric_limits<int>::min() ||
               value.get<std::int64_t>() > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("observation out of int range");
    }
    return value.get<int>();
}
template<class T> Json nullable(const std::optional<T>& value) {
    return value ? Json(*value) : Json(nullptr);
}
ThreadState parse_state(const Json& value) {
    const auto text = value.get<std::string>();
    for (const auto state : {ThreadState::running, ThreadState::runnable, ThreadState::blocked, ThreadState::terminated})
        if (text == to_string(state)) return state;
    throw std::invalid_argument("unknown thread state");
}
Status parse_status(const Json& value) {
    const auto text = value.get<std::string>();
    for (const auto status : {Status::completed, Status::assertion_failed, Status::deadlock,
                             Status::step_limit, Status::timeout, Status::replay_mismatch, Status::worker_error})
        if (text == to_string(status)) return status;
    throw std::invalid_argument("unknown outcome status");
}
Json states_json(const std::vector<ThreadState>& states) {
    auto array = Json::array();
    for (std::size_t i = 0; i < states.size(); ++i)
        array.push_back({{"id", i}, {"state", to_string(states[i])}});
    return array;
}
std::vector<ThreadState> parse_states(const Json& array) {
    if (!array.is_array() || array.size() > 64) throw std::invalid_argument("invalid thread table");
    std::vector<ThreadState> states;
    for (const auto& entry : array) {
        if (natural(entry.at("id"), 63) != states.size())
            throw std::invalid_argument("thread IDs must be contiguous and ordered");
        states.push_back(parse_state(entry.at("state")));
    }
    return states;
}
Json shared_json(const SharedState& state) {
    auto array = Json::array();
    for (const auto& resource : state)
        array.push_back({{"name", resource.name}, {"kind", to_string(resource.kind)}, {"value", nullable(resource.value)}});
    return array;
}
SharedState parse_shared(const Json& array, std::size_t threads) {
    if (!array.is_array()) throw std::invalid_argument("shared state must be an array");
    SharedState state;
    for (const auto& item : array) {
        ResourceSnapshot resource;
        resource.name = item.at("name").get<std::string>();
        if (resource.name.empty() || (!state.empty() && resource.name <= state.back().name))
            throw std::invalid_argument("shared resources must have unique sorted nonempty names");
        const auto kind = item.at("kind").get<std::string>();
        if (kind == "atomic_int") resource.kind = ResourceKind::atomic_int;
        else if (kind == "mutex") resource.kind = ResourceKind::mutex;
        else throw std::invalid_argument("unknown shared resource kind");
        resource.value = optional_int(item.at("value"));
        if (resource.kind == ResourceKind::atomic_int && !resource.value)
            throw std::invalid_argument("atomic resource value cannot be null");
        if (resource.kind == ResourceKind::mutex && resource.value &&
            (*resource.value < 0 || static_cast<std::size_t>(*resource.value) >= threads))
            throw std::invalid_argument("mutex owner must be a registered logical thread");
        state.push_back(std::move(resource));
    }
    return state;
}
bool same_layout(const SharedState& first, const SharedState& second) {
    if (first.size() != second.size()) return false;
    for (std::size_t i = 0; i < first.size(); ++i)
        if (first[i].name != second[i].name || first[i].kind != second[i].kind) return false;
    return true;
}
Json wait_json(const WaitForGraph& graph) {
    auto array = Json::array();
    for (const auto& edge : graph)
        array.push_back({{"thread", edge.thread}, {"waits_for", edge.waits_for},
                         {"kind", to_string(edge.kind)}, {"resource", edge.resource}});
    return array;
}
WaitForGraph parse_wait(const Json& array, const std::vector<ThreadState>& states,
                       const std::optional<SharedState>& shared) {
    if (!array.is_array() || array.size() > 4096)
        throw std::invalid_argument("invalid wait-for graph");
    WaitForGraph graph;
    for (const auto& item : array) {
        WaitDependency edge;
        edge.thread = static_cast<ThreadId>(natural(item.at("thread"), 63));
        edge.waits_for = static_cast<ThreadId>(natural(item.at("waits_for"), 63));
        if (edge.thread >= states.size() || edge.waits_for >= states.size() ||
            states[edge.thread] != ThreadState::blocked)
            throw std::invalid_argument("wait-for edges require a blocked source and registered target");
        const auto kind = item.at("kind").get<std::string>();
        edge.resource = item.at("resource").get<std::string>();
        if (kind == "mutex") {
            edge.kind = WaitKind::mutex;
            if (shared) {
                const auto resource = std::find_if(shared->begin(), shared->end(), [&](const auto& value) {
                    return value.name == edge.resource;
                });
                if (resource != shared->end() && (resource->kind != ResourceKind::mutex ||
                    resource->value != static_cast<int>(edge.waits_for)))
                    throw std::invalid_argument("wait-for mutex edge disagrees with recorded ownership");
            }
        } else if (kind == "join") {
            edge.kind = WaitKind::join;
            if (!edge.resource.empty() || edge.thread == edge.waits_for ||
                states[edge.waits_for] == ThreadState::terminated)
                throw std::invalid_argument("invalid logical join dependency");
        } else throw std::invalid_argument("unknown wait-for dependency kind");
        if (!graph.empty() && (edge <= graph.back() ||
            (edge.thread == graph.back().thread &&
             (edge.kind == WaitKind::mutex || graph.back().kind == WaitKind::mutex))))
            throw std::invalid_argument("wait-for edges must be unique and sorted; a mutex waiter has one owner");
        graph.push_back(std::move(edge));
    }
    return graph;
}
}

std::string encode_trace(const Trace& trace) {
    auto events = Json::array();
    std::vector<ThreadId> schedule;
    for (const auto& event : trace.result.events) {
        const auto& observation = event.observation;
        schedule.push_back(event.thread);
        events.push_back({{"step", event.step}, {"thread", event.thread}, {"runnable", event.runnable},
            {"threads", states_json(event.states)}, {"operation", event.operation},
            {"resource", event.resource}, {"label", event.label}, {"type", to_string(event.type())},
            {"observation", {{"before", nullable(observation.before)}, {"after", nullable(observation.after)},
                {"expected", nullable(observation.expected)}, {"assertion_passed", nullable(observation.assertion_passed)},
                {"error", nullable(observation.error)}}}});
        if (event.shared_state_before) events.back()["shared_state_before"] = shared_json(*event.shared_state_before);
        if (event.shared_state_after) events.back()["shared_state_after"] = shared_json(*event.shared_state_after);
        if (event.wait_for) events.back()["wait_for"] = wait_json(*event.wait_for);
    }
    Json document = {{"schema_version", 1}, {"program", trace.program},
        {"limits", {{"max_steps", trace.max_steps}, {"timeout_ms", trace.timeout.count()}}},
        {"schedule", schedule}, {"events", events},
        {"outcome", {{"status", to_string(trace.result.status)}, {"message", trace.result.message},
                     {"threads", states_json(trace.result.final_states)}}}};
    if (trace.result.shared_state) document["outcome"]["shared_state"] = shared_json(*trace.result.shared_state);
    if (trace.result.wait_for) document["outcome"]["wait_for"] = wait_json(*trace.result.wait_for);
    auto encoded = document.dump(2) + '\n';
    if (encoded.size() > max_trace_bytes) throw std::invalid_argument("trace exceeds 8 MiB limit");
    return encoded;
}

Trace decode_trace(const std::string& input) {
    if (input.size() > max_trace_bytes) throw std::invalid_argument("trace exceeds 8 MiB limit");
    try {
        std::vector<std::set<std::string>> keys;
        const auto document = Json::parse(input,[&](int depth,Json::parse_event_t event,Json& value) {
            if ((event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) && depth >= 32)
                throw std::invalid_argument("trace JSON nesting exceeds 32 containers");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
                throw std::invalid_argument("duplicate trace JSON object key");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        });
        if (natural(document.at("schema_version"), 1) != 1)
            throw std::invalid_argument("unsupported trace schema version");
        Trace trace;
        trace.program = document.at("program").get<std::string>();
        if (trace.program.empty()) throw std::invalid_argument("program identifier cannot be empty");
        trace.max_steps = static_cast<std::size_t>(natural(document.at("limits").at("max_steps"), 1'000'000));
        trace.timeout = std::chrono::milliseconds(natural(document.at("limits").at("timeout_ms"), 60'000));
        if (trace.max_steps == 0 || trace.timeout.count() == 0) throw std::invalid_argument("limits must be positive");
        const auto& outcome = document.at("outcome");
        trace.result.status = parse_status(outcome.at("status"));
        trace.result.message = outcome.at("message").get<std::string>();
        trace.result.final_states = parse_states(outcome.at("threads"));
        if (outcome.contains("shared_state"))
            trace.result.shared_state = parse_shared(outcome.at("shared_state"), trace.result.final_states.size());
        if (outcome.contains("wait_for"))
            trace.result.wait_for = parse_wait(outcome.at("wait_for"), trace.result.final_states, trace.result.shared_state);
        const auto& events = document.at("events");
        const auto& schedule = document.at("schedule");
        if (!events.is_array() || !schedule.is_array() || events.size() != schedule.size() || events.size() > trace.max_steps)
            throw std::invalid_argument("invalid schedule or event count");
        for (const auto& item : events) {
            Event event;
            event.step = static_cast<std::size_t>(natural(item.at("step"), trace.max_steps));
            event.thread = static_cast<ThreadId>(natural(item.at("thread"), 63));
            event.states = parse_states(item.at("threads"));
            if (event.step != trace.result.events.size() || event.states.size() != trace.result.final_states.size() ||
                event.thread >= event.states.size() || natural(schedule.at(event.step), 63) != event.thread)
                throw std::invalid_argument("inconsistent event step, schedule, or thread table");
            if (!item.at("runnable").is_array()) throw std::invalid_argument("runnable must be an array");
            std::vector<ThreadId> actual_runnable;
            for (std::size_t i = 0; i < event.states.size(); ++i) {
                if (event.states[i] == ThreadState::running) throw std::invalid_argument("event must be at a quiescent scheduling point");
                if (event.states[i] == ThreadState::runnable) actual_runnable.push_back(static_cast<ThreadId>(i));
            }
            for (const auto& candidate : item.at("runnable"))
                event.runnable.push_back(static_cast<ThreadId>(natural(candidate, 63)));
            if (event.runnable != actual_runnable ||
                std::find(event.runnable.begin(), event.runnable.end(), event.thread) == event.runnable.end())
                throw std::invalid_argument("selected thread must belong to the ordered runnable set");
            event.operation = item.at("operation").get<std::string>();
            const std::vector<std::string> operations{"load", "store", "fetch_add", "assert_equal", "yield", "lock", "unlock", "join"};
            if (std::find(operations.begin(), operations.end(), event.operation) == operations.end())
                throw std::invalid_argument("unknown operation kind");
            if (item.contains("type") && item.at("type").get<std::string>() != to_string(event.type()))
                throw std::invalid_argument("event type disagrees with operation");
            event.resource = item.at("resource").get<std::string>();
            event.label = item.at("label").get<std::string>();
            if (item.contains("shared_state_before") != item.contains("shared_state_after") ||
                item.contains("shared_state_before") != trace.result.shared_state.has_value())
                throw std::invalid_argument("shared snapshots must be present together on every event and outcome");
            if (item.contains("shared_state_before")) {
                event.shared_state_before = parse_shared(item.at("shared_state_before"), event.states.size());
                event.shared_state_after = parse_shared(item.at("shared_state_after"), event.states.size());
                if (!same_layout(*event.shared_state_before, *trace.result.shared_state) ||
                    !same_layout(*event.shared_state_after, *trace.result.shared_state))
                    throw std::invalid_argument("shared resource layout changed within a trace");
            }
            if (item.contains("wait_for") != trace.result.wait_for.has_value())
                throw std::invalid_argument("wait-for graphs must be present on every event and outcome");
            if (item.contains("wait_for"))
                event.wait_for = parse_wait(item.at("wait_for"), event.states, event.shared_state_before);
            const auto& observation = item.at("observation");
            event.observation.before = optional_int(observation.at("before"));
            event.observation.after = optional_int(observation.at("after"));
            event.observation.expected = optional_int(observation.at("expected"));
            if (!observation.at("assertion_passed").is_null())
                event.observation.assertion_passed = observation.at("assertion_passed").get<bool>();
            if (!observation.at("error").is_null())
                event.observation.error = observation.at("error").get<std::string>();
            if (event.operation == "assert_equal") {
                const auto& observed = event.observation;
                if (!observed.before || observed.before != observed.after || !observed.expected ||
                    !observed.assertion_passed || *observed.assertion_passed != (*observed.before == *observed.expected) || observed.error)
                    throw std::invalid_argument("assertion observation contradicts its values");
            } else if (event.observation.assertion_passed || event.observation.expected) {
                throw std::invalid_argument("only assertion operations carry assertion results");
            }
            trace.result.events.push_back(std::move(event));
        }
        const auto& result = trace.result;
        const auto failures = std::count_if(result.events.begin(),result.events.end(),[](const auto& event) {
            return event.observation.assertion_passed == false;
        });
        const bool stable = result.status == Status::completed || result.status == Status::assertion_failed ||
            result.status == Status::deadlock || result.status == Status::step_limit;
        if (stable && (std::find(result.final_states.begin(),result.final_states.end(),ThreadState::running) != result.final_states.end() ||
            std::any_of(result.events.begin(),result.events.end(),[](const auto& event) { return event.observation.error.has_value(); })))
            throw std::invalid_argument("stable outcome contradicts running workers or operation errors");
        if (stable && result.status != Status::assertion_failed && failures != 0)
            throw std::invalid_argument("outcome contradicts a recorded failed assertion");
        if (result.status == Status::completed && std::any_of(result.final_states.begin(),result.final_states.end(),[](auto state) { return state != ThreadState::terminated; }))
            throw std::invalid_argument("completed outcome requires all workers terminated");
        if (result.status == Status::assertion_failed && (failures != 1 || result.events.empty() ||
            result.events.back().operation != "assert_equal" || result.events.back().observation.assertion_passed != false ||
            !result.events.back().observation.before || !result.events.back().observation.expected))
            throw std::invalid_argument("assertion_failed requires a final failed assertion event");
        if (result.status == Status::deadlock && (std::find(result.final_states.begin(),result.final_states.end(),ThreadState::blocked) == result.final_states.end() ||
            std::any_of(result.final_states.begin(),result.final_states.end(),[](auto state) { return state != ThreadState::blocked && state != ThreadState::terminated; })))
            throw std::invalid_argument("deadlock requires unfinished blocked workers and no runnable worker");
        if (result.status == Status::step_limit && (result.events.size() != trace.max_steps ||
            std::find(result.final_states.begin(),result.final_states.end(),ThreadState::runnable) == result.final_states.end()))
            throw std::invalid_argument("step_limit requires the configured number of events and runnable unfinished work");
        return trace;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid trace JSON: ") + error.what());
    }
}

void save_trace(const std::filesystem::path& path, const Trace& trace) {
    const auto encoded = encode_trace(trace); // Validate before truncating an existing file.
    (void)decode_trace(encoded);
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    if (!file) throw std::runtime_error("cannot open trace for writing: " + path.string());
    file << encoded;
    file.close();
    if (!file) throw std::runtime_error("cannot write trace: " + path.string());
}
Trace load_trace(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open trace: " + path.string());
    // Read at most the size limit plus one, even for pipes or concurrently grown files.
    std::string input;
    char chunk[4096];
    while (file.read(chunk, sizeof(chunk)) || file.gcount() > 0) {
        input.append(chunk, static_cast<std::size_t>(file.gcount()));
        if (input.size() > max_trace_bytes) throw std::invalid_argument("trace exceeds 8 MiB limit");
    }
    if (file.bad()) throw std::runtime_error("cannot read trace: " + path.string());
    return decode_trace(input);
}
Config replay_config(const Trace& trace) {
    Config config;
    config.max_steps = trace.max_steps;
    config.timeout = trace.timeout;
    config.exact_schedule = true;
    config.expected_events = trace.result.events;
    for (const auto& event : trace.result.events) config.schedule.push_back(event.thread);
    return config;
}
Config branch_config(const Trace& trace, std::size_t step, ThreadId alternative) {
    const auto checked = decode_trace(encode_trace(trace));
    if (checked.result.status == Status::timeout || checked.result.status == Status::worker_error ||
        checked.result.status == Status::replay_mismatch)
        throw std::invalid_argument("branch requires a stable original trace");
    if (step >= checked.result.events.size())
        throw std::invalid_argument("branch step must identify a recorded scheduling decision, not the outcome");
    if (alternative >= 64) throw std::invalid_argument("branch thread ID must be 0..63");
    Config config;
    config.max_steps = checked.max_steps;
    config.timeout = checked.timeout;
    config.expected_prefix = std::vector<Event>(checked.result.events.begin(),
        checked.result.events.begin() + static_cast<std::vector<Event>::difference_type>(step));
    for (const auto& event : *config.expected_prefix) config.schedule.push_back(event.thread);
    config.schedule.push_back(alternative);
    config.branch_checkpoint = checked.result.events[step];
    return config;
}
RunResult replay_trace(const Trace& trace, const std::function<RunResult(const Config&)>& scenario) {
    if (!scenario) throw std::invalid_argument("replay requires a scenario factory");
    const auto checked = decode_trace(encode_trace(trace)); // Validate caller-editable in-memory traces too.
    if (checked.result.status == Status::timeout || checked.result.status == Status::worker_error ||
        checked.result.status == Status::replay_mismatch)
        throw std::invalid_argument("replay supports stable program outcomes, not environmental errors/timeouts");
    auto actual = scenario(replay_config(checked));
    if (actual.status == Status::timeout || actual.status == Status::worker_error || actual.status == Status::replay_mismatch)
        return actual;
    const auto& expected = checked.result;
    if (actual.events.size() == expected.events.size()) {
        for (std::size_t i = 0; i < expected.events.size(); ++i) {
            const auto& first = expected.events[i];
            const auto& second = actual.events[i];
            if (first.step != second.step || first.thread != second.thread || first.runnable != second.runnable ||
                first.states != second.states || first.operation != second.operation || first.resource != second.resource ||
                first.label != second.label || first.observation != second.observation ||
                (first.shared_state_before && first.shared_state_before != second.shared_state_before) ||
                (first.shared_state_after && first.shared_state_after != second.shared_state_after) ||
                (first.wait_for && first.wait_for != second.wait_for)) {
                actual.status = Status::replay_mismatch;
                actual.message = "divergence at step " + std::to_string(i) + ": recorded event";
                return actual;
            }
        }
    }
    if (actual.events.size() != expected.events.size() || actual.status != expected.status ||
        actual.message != expected.message || actual.final_states != expected.final_states ||
        (expected.shared_state && expected.shared_state != actual.shared_state) ||
        (expected.wait_for && expected.wait_for != actual.wait_for)) {
        actual.status = Status::replay_mismatch;
        actual.message = "divergence in final outcome or shared state";
    }
    return actual;
}
}
