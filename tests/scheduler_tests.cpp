#include "lost_update.hpp"
#include "threadscope/explore.hpp"
#include "threadscope/trace.hpp"
#include <filesystem>
#include <fstream>

#include <array>
#include <chrono>
#include <iostream>
#include <latch>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using namespace threadscope;
using namespace std::chrono_literals;
#define CHECK(expression) do { if (!(expression)) throw std::runtime_error( \
    std::string("check failed at line ") + std::to_string(__LINE__) + ": " #expression); } while (false)
template<class Function> void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
}
Config schedule(std::vector<ThreadId> choices, bool exact = false) {
    Config config;
    config.schedule = std::move(choices);
    config.exact_schedule = exact;
    return config;
}
std::string replace(std::string text, const std::string& from, const std::string& to) {
    const auto position = text.find(from);
    CHECK(position != std::string::npos);
    text.replace(position, from.size(), to);
    return text;
}

void serial_increment() {
    const auto result = examples::lost_update(schedule({0, 0, 1, 1}));
    CHECK(result.status == Status::completed);
    CHECK(result.events.size() == 6);
    CHECK(result.events[0].runnable == (std::vector<ThreadId>{0, 1}));
    CHECK(result.events[0].states[2] == ThreadState::blocked);
    CHECK(result.events[1].observation.after == 1);
    CHECK(result.events[2].observation.before == 1);
    CHECK(result.events[3].observation.after == 2);
    CHECK(result.events.back().observation.assertion_passed == true);
    CHECK(result.events.back().observation.before == 2);
    CHECK(result.final_states == (std::vector<ThreadState>(3, ThreadState::terminated)));
}
void lost_update() {
    const auto result = examples::lost_update(schedule({0, 1, 0, 1}));
    CHECK(result.status == Status::assertion_failed);
    CHECK(result.events[0].observation.before == 0);
    CHECK(result.events[1].observation.before == 0);
    CHECK(result.events[2].observation.after == 1);
    CHECK(result.events[3].observation.before == 1);
    CHECK(result.events[3].observation.after == 1);
    CHECK(result.events.back().observation.before == 1);
    CHECK(result.events.back().observation.after == 1); // Assertion doesn't mutate state.
    CHECK(result.events.back().observation.expected == 2);
    CHECK(result.events.back().observation.assertion_passed == false);
}
void exhaustive_interleavings() {
    const auto search = explore([](const Config& config) { return examples::lost_update(config); });
    CHECK(search.exhausted);
    CHECK(search.runs.size() == 6);
    std::set<std::vector<ThreadId>> choices;
    std::size_t failures = 0;
    for (const auto& run : search.runs) {
        std::vector<ThreadId> path;
        for (const auto& event : run.events) path.push_back(event.thread);
        CHECK(choices.insert(path).second);
        if (run.status == Status::assertion_failed) ++failures;
        else CHECK(run.status == Status::completed);
    }
    CHECK(failures == 4);
    // The known combinatorial set, rather than just a count.
    CHECK(choices == (std::set<std::vector<ThreadId>>{
        {0, 0, 1, 1, 2, 2}, {0, 1, 0, 1, 2, 2}, {0, 1, 1, 0, 2, 2},
        {1, 0, 0, 1, 2, 2}, {1, 0, 1, 0, 2, 2}, {1, 1, 0, 0, 2, 2}}));
}
void rmw_fix() {
    const auto search = explore([](const Config& config) { return examples::lost_update(config, true); });
    CHECK(search.exhausted);
    CHECK(search.runs.size() == 2);
    for (const auto& run : search.runs) {
        CHECK(run.status == Status::completed);
        CHECK(run.events.back().observation.before == 2);
    }
}
void bounded_search() {
    const auto search = explore([](const Config& config) { return examples::lost_update(config); }, {}, 2);
    CHECK(!search.exhausted);
    CHECK(search.runs.size() == 2);
    Config bounded;
    bounded.max_steps = 1;
    const auto bounded_tree = explore([](const Config& config) { return examples::lost_update(config); }, bounded);
    CHECK(bounded_tree.exhausted); // Only the depth-one frontier, not program correctness.
    CHECK(bounded_tree.runs.size() == 2);
    for (const auto& run : bounded_tree.runs) CHECK(run.status == Status::step_limit);
}
void deterministic_replay() {
    const auto original = examples::lost_update(schedule({1, 0, 1, 0}));
    Trace trace{"lost-update/v1", 1000, 1000ms, original};
    for (int i = 0; i < 100; ++i) CHECK(examples::lost_update(replay_config(trace)) == original);
}
void arrival_timing_is_irrelevant() {
    auto run = [](bool delay_first) {
        Scheduler scheduler;
        for (int i = 0; i < 2; ++i) {
            scheduler.add_thread([i, delay_first](Context& context) {
                if ((i == 0) == delay_first) std::this_thread::sleep_for(2ms);
                context.yield("first");
                std::this_thread::sleep_for(1ms);
                context.yield("second");
            });
        }
        return scheduler.run(schedule({1, 0, 1, 0}, true));
    };
    CHECK(run(true) == run(false));
}
void replay_divergence() {
    Trace trace{"lost-update/v1", 1000, 1000ms, examples::lost_update()};
    trace.result.events[0].observation.before = 99;
    const auto result = examples::lost_update(replay_config(trace));
    CHECK(result.status == Status::replay_mismatch);
    CHECK(result.events.size() == 1);
}
void unavailable_choices() {
    CHECK(examples::lost_update(schedule({63})).status == Status::replay_mismatch);
    CHECK(examples::lost_update(schedule({2})).status == Status::replay_mismatch); // Checker is blocked.
}
void replay_lengths() {
    CHECK(examples::lost_update(schedule({0}, true)).status == Status::replay_mismatch);
    CHECK(examples::lost_update(schedule({0, 0, 1, 1, 2, 2}, true)).status == Status::completed);
    CHECK(examples::lost_update(schedule({0, 0, 1, 1, 2, 2, 0}, true)).status == Status::replay_mismatch);
    CHECK(examples::lost_update(schedule({0, 1, 0, 1, 2, 2, 0}, true)).status == Status::replay_mismatch);
}
void failure_snapshot_precedes_cancellation() {
    auto scenario = [](const Config& config) {
        AtomicInt value("value", 0);
        Scheduler scheduler;
        scheduler.add_thread([&](Context& context) { context.assert_equal(value, 1, "early failure"); });
        scheduler.add_thread([](Context& context) { context.join_all({2}); });
        for (int i = 2; i < 32; ++i)
            scheduler.add_thread([](Context& context) { context.yield("still pending at failure"); });
        return scheduler.run(config);
    };
    auto expected_states = std::vector<ThreadState>(32, ThreadState::runnable);
    expected_states[0] = ThreadState::terminated;
    expected_states[1] = ThreadState::blocked;
    const auto original = scenario({});
    CHECK(original.status == Status::assertion_failed);
    CHECK(original.events.size() == 1);
    CHECK(original.final_states == expected_states);
    const Trace trace{"early-failure/v1", 1000, 1000ms, original};
    for (int i = 0; i < 25; ++i) CHECK(scenario(replay_config(trace)) == original);
}
void startup_errors_are_not_arrival_ordered() {
    auto scenario = [](bool delay_zero) {
        Scheduler scheduler;
        for (int i = 0; i < 2; ++i) scheduler.add_thread([i, delay_zero](Context&) {
            if ((i == 0) == delay_zero) std::this_thread::sleep_for(10ms);
            throw std::runtime_error("worker " + std::to_string(i));
        });
        return scheduler.run();
    };
    const auto zero_delayed = scenario(true);
    const auto one_delayed = scenario(false);
    CHECK(zero_delayed.status == Status::worker_error);
    CHECK(zero_delayed.message == "worker 0");
    CHECK(zero_delayed.final_states == (std::vector<ThreadState>{
        ThreadState::terminated, ThreadState::terminated}));
    CHECK(zero_delayed == one_delayed);
}
void replay_timeout_keeps_environmental_status() {
    Scheduler original;
    original.add_thread([](Context& context) { context.yield(); });
    const auto original_result = original.run(); // Don't give the positive control a 2 ms deadline.
    const Trace trace{"slow-start/v1", 1000, 2ms, original_result};
    CHECK(trace.result.status == Status::completed);
    Scheduler delayed;
    delayed.add_thread([](Context& context) {
        std::this_thread::sleep_for(25ms);
        context.yield();
    });
    const auto result = delayed.run(replay_config(trace));
    CHECK(result.status == Status::timeout);
    CHECK(result.events.empty());
    CHECK(result.message == "cooperative run deadline exceeded");
    Scheduler throwing;
    throwing.add_thread([](Context&) { throw std::runtime_error("local worker error"); });
    const auto error = throwing.run(replay_config(trace));
    CHECK(error.status == Status::worker_error);
    CHECK(error.message == "local worker error");
}
void exception_what_does_not_hold_monitor() {
    std::latch entered_what(1), release_what(1);
    struct GateException : std::exception {
        std::latch& entered;
        std::latch& release;
        GateException(std::latch& first, std::latch& second) : entered(first), release(second) {}
        const char* what() const noexcept override {
            entered.count_down();
            release.wait();
            return "gated exception";
        }
    };
    struct OnExit {
        std::latch& release;
        ~OnExit() { release.count_down(); }
    };
    Scheduler scheduler;
    scheduler.add_thread([&](Context&) { throw GateException(entered_what, release_what); });
    scheduler.add_thread([&](Context& context) {
        entered_what.wait();
        OnExit signal{release_what};
        context.yield(); // Cancellation must acquire the monitor and release what().
    });
    Config config;
    config.timeout = 100ms;
    const auto result = scheduler.run(config);
    CHECK(result.status == Status::timeout);
    CHECK(result.events.empty());
}
void mutex_blocking_and_wakeup() {
    Scheduler scheduler;
    ControlledMutex mutex("mutex");
    for (int i = 0; i < 2; ++i) {
        scheduler.add_thread([&](Context& context) {
            context.lock(mutex);
            context.yield("critical section");
            context.unlock(mutex);
        });
    }
    const auto result = scheduler.run();
    CHECK(result.status == Status::completed);
    CHECK(result.events.size() == 6);
    CHECK(result.events[1].states[1] == ThreadState::blocked);
    CHECK(result.events[1].runnable == (std::vector<ThreadId>{0}));
    CHECK(result.events[3].thread == 1);
    CHECK(result.events[3].operation == "lock");
}
void lock_protected_increment() {
    auto scenario = [](const Config& config) {
        Scheduler scheduler;
        ControlledMutex mutex("mutex");
        AtomicInt value("counter");
        for (int i = 0; i < 2; ++i) scheduler.add_thread([&](Context& context) {
            context.lock(mutex);
            const auto local = context.load(value);
            context.store(value, local + 1);
            context.unlock(mutex);
        });
        scheduler.add_thread([&](Context& context) {
            context.join_all({0, 1});
            context.assert_equal(value, 2, "protected increments");
        });
        return scheduler.run(config);
    };
    const auto search = explore(scenario);
    CHECK(search.exhausted);
    for (const auto& run : search.runs) CHECK(run.status == Status::completed);
}
void deadlock_detection() {
    auto scenario = [](const Config& config) {
        Scheduler scheduler;
        ControlledMutex first("first"), second("second");
        scheduler.add_thread([&](Context& context) { context.lock(first); context.lock(second); });
        scheduler.add_thread([&](Context& context) { context.lock(second); context.lock(first); });
        return scheduler.run(config);
    };
    const auto result = scenario(schedule({0, 1}));
    CHECK(result.status == Status::deadlock);
    CHECK(result.events.size() == 2);
    CHECK(result.final_states == (std::vector<ThreadState>{ThreadState::blocked, ThreadState::blocked}));
    const auto trace = decode_trace(encode_trace({"deadlock-test/v1", 1000, 1000ms, result}));
    CHECK(scenario(replay_config(trace)) == result);
    CHECK(scenario(schedule({0, 1, 0}, true)).status == Status::replay_mismatch);
    CHECK(scenario(schedule({0, 1, 0})).status == Status::replay_mismatch);
    CHECK(scenario(schedule({1})).status == Status::deadlock); // A valid forced prefix may be shorter than the run.
}
void step_limit_cancels_waiters() {
    Scheduler scheduler;
    scheduler.add_thread([](Context& context) { while (true) context.yield(); });
    Config config;
    config.max_steps = 3;
    const auto result = scheduler.run(config);
    CHECK(result.status == Status::step_limit);
    CHECK(result.events.size() == 3);
    Config boundary;
    boundary.max_steps = 6;
    CHECK(examples::lost_update(boundary).status == Status::completed);
    boundary.max_steps = 5;
    CHECK(examples::lost_update(boundary).status == Status::step_limit);
}
void timeout_of_bounded_local_code() {
    Scheduler scheduler;
    scheduler.add_thread([](Context& context) {
        std::this_thread::sleep_for(25ms); // Finite preparation; cancellation is cooperative.
        context.yield();
    });
    Config config;
    config.timeout = 2ms;
    const auto result = scheduler.run(config);
    CHECK(result.status == Status::timeout);
    CHECK(result.events.empty());
    CHECK(result.final_states.size() == 1);
    // The driver might be descheduled until the sleeper submits its request.
    // Timeout diagnostics do not promise a reproducible OS progress snapshot.
    CHECK(result.final_states[0] == ThreadState::running || result.final_states[0] == ThreadState::runnable);
}
void worker_exception() {
    Scheduler scheduler;
    scheduler.add_thread([](Context&) { throw std::runtime_error("deliberate worker error"); });
    const auto result = scheduler.run();
    CHECK(result.status == Status::worker_error);
    CHECK(result.message == "deliberate worker error");
}
void invalid_unlock() {
    Scheduler scheduler;
    ControlledMutex mutex("unowned");
    scheduler.add_thread([&](Context& context) { context.unlock(mutex); });
    const auto result = scheduler.run();
    CHECK(result.status == Status::worker_error);
    CHECK(result.events.size() == 1);
    CHECK(result.events[0].observation.error.has_value());
}
void actual_worker_threads_and_stable_ids() {
    Scheduler scheduler;
    std::array<std::thread::id, 2> workers;
    for (ThreadId id = 0; id < 2; ++id) {
        CHECK(scheduler.add_thread([&, id](Context& context) {
            CHECK(context.id() == id);
            workers[id] = std::this_thread::get_id();
            context.yield();
        }) == id);
    }
    CHECK(scheduler.run().status == Status::completed);
    CHECK(workers[0] != std::this_thread::get_id());
    CHECK(workers[1] != std::this_thread::get_id());
    CHECK(workers[0] != workers[1]);
}
void atomic_arithmetic_boundary() {
    AtomicInt value("integer", std::numeric_limits<int>::max());
    Scheduler scheduler;
    scheduler.add_thread([&](Context& context) { context.fetch_add(value, 1); });
    const auto result = scheduler.run();
    CHECK(result.status == Status::completed);
    CHECK(value.snapshot() == std::numeric_limits<int>::min());
    CHECK(result.events[0].observation.after == std::numeric_limits<int>::min());
}
void lifecycle_validation() {
    Scheduler empty;
    CHECK(empty.run().status == Status::completed);
    rejects([&] { empty.run(); });
    rejects([&] { empty.add_thread([](Context&) {}); });
    Scheduler invalid;
    rejects([&] { invalid.add_thread({}); });
    Config config;
    config.max_steps = 0;
    rejects([&] { invalid.run(config); });
    config.max_steps = 1;
    config.timeout = 0ms;
    rejects([&] { invalid.run(config); });
    config.timeout = 1000ms;
    config.expected_events = std::vector<Event>{};
    rejects([&] { invalid.run(config); });
    config.expected_events.reset();
    config.timeout = 60'001ms;
    rejects([&] { invalid.run(config); });
    config.timeout = 1000ms;
    config.max_steps = 1'000'001;
    rejects([&] { invalid.run(config); });
}
void invalid_join() {
    Scheduler scheduler;
    scheduler.add_thread([](Context& context) { context.join_all({0}); });
    CHECK(scheduler.run().status == Status::worker_error);
}
void context_rejects_foreign_native_thread() {
    Scheduler scheduler;
    std::atomic<bool> refused{false};
    scheduler.add_thread([&](Context& context) {
        std::thread outsider([&] {
            try { context.yield(); }
            catch (const std::logic_error&) { refused.store(true); }
        });
        outsider.join();
    });
    const auto result = scheduler.run();
    CHECK(refused.load());
    CHECK(result.status == Status::completed && result.events.empty());
}
void trace_outcomes_cannot_contradict_events() {
    Trace original{"lost-update/v1",1000,1000ms,examples::lost_update(schedule({0,1,0,1}))};
    auto changed = original;
    changed.result.status = Status::completed;
    rejects([&] { decode_trace(encode_trace(changed)); });
    changed = original;
    changed.result.status = Status::deadlock;
    rejects([&] { decode_trace(encode_trace(changed)); });
    changed = original;
    changed.result.events.back().observation.assertion_passed = true;
    rejects([&] { decode_trace(encode_trace(changed)); });
    changed = {"lost-update/v1",1000,1000ms,examples::lost_update(schedule({0,0,1,1}))};
    changed.result.final_states[0] = ThreadState::running;
    rejects([&] { decode_trace(encode_trace(changed)); });
    auto config = schedule({}); config.max_steps = 1;
    changed = {"lost-update/v1",1000,1000ms,examples::lost_update(config)};
    rejects([&] { decode_trace(encode_trace(changed)); });
}
void invalid_trace_save_preserves_file() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("threadscope-save-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(std::filesystem::create_directory(directory));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
    const auto path = directory / "original.json";
    Trace original{"lost-update/v1",1000,1000ms,examples::lost_update(schedule({0,0,1,1}))};
    save_trace(path,original);
    auto changed = original;
    changed.result.events[0].step = 99;
    rejects([&] { save_trace(path,changed); });
    CHECK(load_trace(path) == original);
}
void ambiguous_and_deep_json_is_rejected() {
    Trace original{"lost-update/v1",1000,1000ms,examples::lost_update(schedule({0,0,1,1}))};
    auto encoded = encode_trace(original);
    encoded.erase(encoded.find_last_of('}'));
    rejects([&] { decode_trace(encoded + ",\"program\":\"lost-update/v1\"}"); });
    rejects([&] { decode_trace(encoded + ",\"extra\":" + std::string(100,'[') + "0" + std::string(100,']') + "}"); });
}
void json_roundtrip_and_validation() {
    Trace trace{"lost-update/v1", 1000, 1000ms, examples::lost_update(schedule({0, 1, 0, 1}))};
    trace.result.events[0].label = "quotes: \" slash: \\ newline: \n snowman: ☃";
    const auto json = encode_trace(trace);
    CHECK(decode_trace(json) == trace);
    rejects([&] { decode_trace("{broken"); });
    rejects([&] { decode_trace(replace(json, "\"schema_version\": 1", "\"schema_version\": 2")); });
    rejects([&] { decode_trace(replace(json, "\"thread\": 0", "\"thread\": -1")); });
    rejects([&] { decode_trace(replace(json, "\"thread\": 0", "\"thread\": 0.5")); });
    rejects([&] { decode_trace(replace(json, "\"step\": 0", "\"step\": 1")); });
    rejects([&] { decode_trace(replace(json, "\"max_steps\": 1000", "\"max_steps\": 0")); });
    rejects([&] { decode_trace(replace(json, "\"after\": 0", "\"after\": 4294967296")); });
    rejects([&] { decode_trace(replace(json, "\"state\": \"runnable\"", "\"state\": \"running\"")); });
    rejects([&] { decode_trace(std::string(8 * 1024 * 1024 + 1, ' ')); });
    trace.result.events[0].label = std::string(8 * 1024 * 1024, 'x');
    rejects([&] { encode_trace(trace); });
}
}

int main(int argc, char** argv) {
    // Isolate this hang regression so CTest can enforce an outer process limit.
    if (argc == 2 && std::string(argv[1]) == "--exception-monitor") {
        try { exception_what_does_not_hold_monitor(); return 0; }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    if (argc != 1) return 2;
    const std::vector<std::pair<std::string, void(*)()>> tests = {
        {"serial increment", serial_increment}, {"lost update", lost_update},
        {"exhaustive interleavings", exhaustive_interleavings}, {"RMW fix", rmw_fix},
        {"bounded search", bounded_search}, {"deterministic replay x100", deterministic_replay},
        {"arrival timing", arrival_timing_is_irrelevant}, {"replay divergence", replay_divergence},
        {"unavailable choices", unavailable_choices}, {"replay lengths", replay_lengths},
        {"failure snapshot before cancellation", failure_snapshot_precedes_cancellation},
        {"startup error ordering", startup_errors_are_not_arrival_ordered},
        {"replay timeout classification", replay_timeout_keeps_environmental_status},
        {"mutex blocking/wakeup", mutex_blocking_and_wakeup}, {"protected increment", lock_protected_increment},
        {"deadlock detection", deadlock_detection}, {"step limit cancellation", step_limit_cancels_waiters},
        {"cooperative timeout", timeout_of_bounded_local_code}, {"worker exception", worker_exception},
        {"invalid unlock", invalid_unlock}, {"real threads/stable IDs", actual_worker_threads_and_stable_ids},
        {"atomic overflow boundary", atomic_arithmetic_boundary}, {"lifecycle validation", lifecycle_validation},
        {"invalid join", invalid_join}, {"JSON roundtrip/validation", json_roundtrip_and_validation},
        {"context native-thread ownership", context_rejects_foreign_native_thread},
        {"trace outcome consistency", trace_outcomes_cannot_contradict_events},
        {"invalid save preserves original", invalid_trace_save_preserves_file},
        {"ambiguous/deep JSON", ambiguous_and_deep_json_is_rejected}
    };
    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << tests.size() << " cases, " << failures << " failures\n";
    return failures ? 1 : 0;
}
