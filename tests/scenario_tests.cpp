#include "scenarios.hpp"
#include "threadscope/explore.hpp"
#include "threadscope/trace.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace threadscope;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #x); } while (false)
template<class Function> void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
}
Trace trace_for(const examples::Scenario& scenario, const RunResult& result) {
    return {std::string(scenario.program), 1000, std::chrono::milliseconds(1000), result};
}
std::vector<ThreadId> schedule(const RunResult& result) {
    std::vector<ThreadId> ids;
    for (const auto& event : result.events) ids.push_back(event.thread);
    return ids;
}

void failing_and_corrected_scenarios() {
    struct Expected { const char* name; std::size_t runs; std::size_t assertions; std::size_t deadlocks; };
    for (const auto expected : {Expected{"lost-update",6,4,0}, Expected{"lost-update-rmw",2,0,0},
                               Expected{"mutex-deadlock",6,0,2}, Expected{"mutex-ordered",2,0,0}}) {
        const auto& scenario = examples::scenario_named(expected.name);
        Config config;
        config.max_steps = 8;
        const auto search = explore(scenario.run, config, 100);
        CHECK(search.exhausted && !search.step_bound_reached && !search.schedule_bound_reached);
        CHECK(search.runs.size() == expected.runs);
        std::size_t assertions = 0, deadlocks = 0;
        for (const auto& run : search.runs) {
            assertions += run.status == Status::assertion_failed;
            deadlocks += run.status == Status::deadlock;
            CHECK(run.status == Status::completed || run.status == Status::assertion_failed || run.status == Status::deadlock);
            if (run.status == Status::completed) {
                CHECK(std::all_of(run.final_states.begin(), run.final_states.end(), [](auto state) {
                    return state == ThreadState::terminated;
                }));
                CHECK(run.wait_for && run.wait_for->empty());
            }
        }
        CHECK(assertions == expected.assertions && deadlocks == expected.deadlocks);
    }
}

void opposite_order_cycle_and_replay() {
    const auto& scenario = examples::scenario_named("mutex-deadlock");
    Config config;
    config.schedule = {0,1};
    const auto run = scenario.run(config);
    CHECK(run.status == Status::deadlock && schedule(run) == config.schedule);
    CHECK(run.final_states == (std::vector<ThreadState>{ThreadState::blocked, ThreadState::blocked}));
    CHECK(run.shared_state->at(0).name == "first" && run.shared_state->at(0).value == 0);
    CHECK(run.shared_state->at(1).name == "second" && run.shared_state->at(1).value == 1);
    CHECK(run.wait_for == (WaitForGraph{{0,1,WaitKind::mutex,"second"}, {1,0,WaitKind::mutex,"first"}}));
    CHECK(has_wait_cycle(*run.wait_for));
    CHECK(run.events[0].type() == EventType::lock_acquire);
    CHECK(run.events[0].wait_for->empty());
    const auto trace = trace_for(scenario, run);
    CHECK(decode_trace(encode_trace(trace)) == trace);
    for (int i = 0; i < 20; ++i) {
        const auto replayed = replay_trace(decode_trace(encode_trace(trace)), scenario.run);
        CHECK(replayed == run);
        CHECK(encode_trace(trace_for(scenario, replayed)) == encode_trace(trace));
    }
}

void consistent_order_blocks_and_resumes() {
    const auto& scenario = examples::scenario_named("mutex-ordered");
    const auto run = scenario.run({});
    CHECK(run.status == Status::completed);
    CHECK(schedule(run) == (std::vector<ThreadId>{0,0,0,0,1,1,1,1}));
    CHECK(run.events[0].states == (std::vector<ThreadState>{ThreadState::runnable, ThreadState::runnable}));
    CHECK(run.events[1].states == (std::vector<ThreadState>{ThreadState::runnable, ThreadState::blocked}));
    CHECK(run.events[1].wait_for == (WaitForGraph{{1,0,WaitKind::mutex,"first"}}));
    CHECK(run.events[3].type() == EventType::lock_release);
    CHECK(run.events[3].shared_state_after->at(0).value == std::nullopt);
    CHECK(run.events[4].states == (std::vector<ThreadState>{ThreadState::terminated, ThreadState::runnable}));
    CHECK(run.events[4].wait_for->empty());
    for (const auto& event : run.events) CHECK(!has_wait_cycle(*event.wait_for));
    CHECK(!run.shared_state->at(0).value && !run.shared_state->at(1).value);
    Config reverse;
    reverse.schedule = {1};
    CHECK(schedule(scenario.run(reverse)) == (std::vector<ThreadId>{1,1,1,1,0,0,0,0}));
}

void lock_waits_do_not_hold_the_execution_permit() {
    ControlledMutex first("first"), second("second");
    Scheduler scheduler;
    scheduler.track(first); scheduler.track(second);
    scheduler.add_thread([&](Context& thread) { thread.lock(first); thread.lock(second); });
    scheduler.add_thread([&](Context& thread) { thread.lock(second); thread.lock(first); });
    scheduler.add_thread([](Context& thread) { for (int i = 0; i < 3; ++i) thread.yield(); });
    Config config;
    config.schedule = {0,1};
    const auto run = scheduler.run(config);
    CHECK(run.status == Status::deadlock);
    CHECK(schedule(run) == (std::vector<ThreadId>{0,1,2,2,2}));
    CHECK(run.final_states[2] == ThreadState::terminated);
    for (std::size_t i = 2; i < run.events.size(); ++i) {
        CHECK(run.events[i].runnable == (std::vector<ThreadId>{2}));
        CHECK(has_wait_cycle(*run.events[i].wait_for));
    }
    // A cycle does not imply a globally deadlocked run while a third worker can
    // still progress. At a two-step cutoff its next operation remains runnable.
    auto bounded = [](const Config& limits) {
        ControlledMutex a("a"), b("b");
        Scheduler engine;
        engine.track(a); engine.track(b);
        engine.add_thread([&](Context& thread) { thread.lock(a); thread.lock(b); });
        engine.add_thread([&](Context& thread) { thread.lock(b); thread.lock(a); });
        engine.add_thread([](Context& thread) { thread.yield(); });
        return engine.run(limits);
    };
    config.max_steps = 2;
    const auto limited = bounded(config);
    CHECK(limited.status == Status::step_limit && limited.final_states[2] == ThreadState::runnable);
    CHECK(has_wait_cycle(*limited.wait_for));
    CHECK(replay_trace({"cycle-with-progress/v1",2,config.timeout,limited}, bounded) == limited);
}

void retained_owners_recursive_locks_and_joins() {
    ControlledMutex mutex("retained");
    Scheduler scheduler;
    scheduler.track(mutex);
    scheduler.add_thread([&](Context& thread) { thread.lock(mutex); });
    scheduler.add_thread([&](Context& thread) { thread.lock(mutex); });
    const auto run = scheduler.run();
    CHECK(run.status == Status::deadlock && run.final_states[0] == ThreadState::terminated);
    CHECK(run.wait_for == (WaitForGraph{{1,0,WaitKind::mutex,"retained"}}));
    CHECK(!has_wait_cycle(*run.wait_for)); // A retained owner need not form a cycle.
    ControlledMutex recursive("recursive");
    Scheduler recursion;
    recursion.track(recursive);
    recursion.add_thread([&](Context& thread) { thread.lock(recursive); thread.lock(recursive); });
    const auto self = recursion.run();
    CHECK(self.status == Status::deadlock);
    CHECK(self.wait_for == (WaitForGraph{{0,0,WaitKind::mutex,"recursive"}}));
    CHECK(has_wait_cycle(*self.wait_for));
    Scheduler joins;
    joins.add_thread([](Context& thread) { thread.yield(); });
    joins.add_thread([](Context& thread) { thread.yield(); });
    joins.add_thread([](Context& thread) { thread.join_all({1,0,1}); });
    const auto joined = joins.run();
    CHECK(joined.status == Status::completed);
    CHECK(joined.events[0].wait_for == (WaitForGraph{{2,0,WaitKind::join,""}, {2,1,WaitKind::join,""}}));
    CHECK(joined.events[1].wait_for == (WaitForGraph{{2,1,WaitKind::join,""}}));
    CHECK(joined.events[2].type() == EventType::join && joined.events[2].wait_for->empty());
}

void replay_validates_dependencies_and_accepts_legacy() {
    const auto& ordered = examples::scenario_named("mutex-ordered");
    auto trace = trace_for(ordered, ordered.run({}));
    trace.result.events[1].wait_for->clear();
    const auto divergent = replay_trace(trace, ordered.run);
    CHECK(divergent.status == Status::replay_mismatch && divergent.events.size() == 1);
    CHECK(divergent.message.find("wait-for graph") != std::string::npos);
    CHECK(divergent.shared_state->at(1).value == std::nullopt); // Second acquisition was not granted.
    const auto& broken = examples::scenario_named("mutex-deadlock");
    Config config;
    config.schedule = {0,1};
    trace = trace_for(broken, broken.run(config));
    trace.result.wait_for->pop_back();
    CHECK(replay_trace(trace, broken.run).status == Status::replay_mismatch);
    // The final validator must also catch a factory ignoring expected events.
    trace = trace_for(ordered, ordered.run({}));
    CHECK(replay_trace(trace, [&](const Config&) {
        auto result = trace.result;
        result.events[1].wait_for->clear();
        return result;
    }).status == Status::replay_mismatch);
    // Older v1 histories have neither graphs nor full shared snapshots.
    trace = trace_for(broken, broken.run(config));
    const auto expected = trace.result;
    trace.result.wait_for.reset(); trace.result.shared_state.reset();
    for (auto& event : trace.result.events) {
        event.wait_for.reset(); event.shared_state_before.reset(); event.shared_state_after.reset();
    }
    CHECK(replay_trace(decode_trace(encode_trace(trace)), broken.run) == expected);
}

void invalid_wait_graphs_and_event_types() {
    const auto& scenario = examples::scenario_named("mutex-deadlock");
    Config config;
    config.schedule = {0,1};
    const auto original = trace_for(scenario, scenario.run(config));
    auto trace = original;
    trace.result.events[0].wait_for.reset();
    rejects([&] { decode_trace(encode_trace(trace)); });
    trace = original;
    trace.result.wait_for->at(0).waits_for = 63;
    rejects([&] { decode_trace(encode_trace(trace)); });
    trace = original;
    trace.result.wait_for->at(0).waits_for = 0;
    rejects([&] { decode_trace(encode_trace(trace)); });
    trace = original;
    trace.result.wait_for->at(0).kind = WaitKind::join;
    rejects([&] { decode_trace(encode_trace(trace)); });
    trace = original;
    trace.result.wait_for->push_back(trace.result.wait_for->back());
    rejects([&] { decode_trace(encode_trace(trace)); });
    trace = original;
    std::reverse(trace.result.wait_for->begin(), trace.result.wait_for->end());
    rejects([&] { decode_trace(encode_trace(trace)); });
    trace = original;
    trace.result.events[0].wait_for = WaitForGraph{{0,1,WaitKind::join,""}}; // Source is runnable.
    rejects([&] { decode_trace(encode_trace(trace)); });
    auto encoded = encode_trace(original);
    const auto position = encoded.find("\"type\": \"lock_acquire\"");
    CHECK(position != std::string::npos);
    encoded.replace(position, std::string("\"type\": \"lock_acquire\"").size(), "\"type\": \"lock_release\"");
    rejects([&] { decode_trace(encoded); });
    CHECK(!has_wait_cycle({{0,1,WaitKind::join,""}, {1,2,WaitKind::join,""}}));
    CHECK(has_wait_cycle({{0,1,WaitKind::join,""}, {1,2,WaitKind::join,""}, {2,0,WaitKind::join,""}}));
    rejects([] { has_wait_cycle({{64,0,WaitKind::mutex,"invalid"}}); });
    CHECK(event_type("load") == EventType::shared_state && event_type("store") == EventType::shared_state);
    CHECK(event_type("fetch_add") == EventType::shared_state && event_type("assert_equal") == EventType::assertion);
}

void repeated_searches_and_replays() {
    for (const auto& scenario : examples::scenarios()) {
        const auto expected = explore(scenario.run);
        for (int repetition = 0; repetition < 5; ++repetition) {
            const auto actual = explore(scenario.run);
            CHECK(actual == expected);
            for (std::size_t i = 0; i < actual.runs.size(); ++i) {
                const auto trace = trace_for(scenario, actual.runs[i]);
                CHECK(encode_trace(trace) == encode_trace(trace_for(scenario, expected.runs[i])));
                for (int replay = 0; replay < 3; ++replay)
                    CHECK(replay_trace(decode_trace(encode_trace(trace)), scenario.run) == actual.runs[i]);
            }
        }
    }
}

void mixed_wait_graph_ignores_startup_timing() {
    // Delay either worker's initial request. Assertions concern logical order
    // and complete results, never a particular native wakeup/arrival order.
    auto scenario = [](ThreadId delayed, const Config& config) {
        ControlledMutex mutex("mixed");
        Scheduler scheduler;
        scheduler.track(mutex);
        scheduler.add_thread([&](Context& thread) {
            if (delayed == 0) std::this_thread::sleep_for(std::chrono::milliseconds(3));
            thread.lock(mutex);
            thread.join_all({1});
            thread.unlock(mutex);
        });
        scheduler.add_thread([&](Context& thread) {
            if (delayed == 1) std::this_thread::sleep_for(std::chrono::milliseconds(3));
            thread.lock(mutex);
            thread.unlock(mutex);
        });
        return scheduler.run(config);
    };
    Config config;
    config.timeout = std::chrono::milliseconds(5000);
    const auto expected = scenario(0, config);
    CHECK(expected.status == Status::deadlock);
    CHECK(schedule(expected) == (std::vector<ThreadId>{0}));
    CHECK(expected.wait_for == (WaitForGraph{{0,1,WaitKind::join,""}, {1,0,WaitKind::mutex,"mixed"}}));
    CHECK(has_wait_cycle(*expected.wait_for));
    const Trace trace{"mixed-waits/v1",config.max_steps,config.timeout,expected};
    for (int repetition = 0; repetition < 10; ++repetition) {
        const auto delayed = static_cast<ThreadId>(repetition % 2);
        const auto actual = scenario(delayed, config);
        CHECK(actual == expected);
        CHECK(encode_trace({trace.program,config.max_steps,config.timeout,actual}) == encode_trace(trace));
        CHECK(replay_trace(decode_trace(encode_trace(trace)), [&](const Config& replay) {
            return scenario(1 - delayed, replay);
        }) == expected);
    }
}

void maximum_thread_id_and_dense_join_graph() {
    // Every worker waits on all 63 others. No operation can execute, so the
    // terminal graph has 4032 edges and must include ID 63 without truncation.
    auto scenario = [](const Config& config) {
        Scheduler scheduler;
        for (ThreadId id = 0; id < 64; ++id) scheduler.add_thread([](Context& thread) {
            std::vector<ThreadId> targets;
            for (ThreadId target = 0; target < 64; ++target)
                if (target != thread.id()) targets.push_back(target);
            thread.join_all(std::move(targets));
        });
        return scheduler.run(config);
    };
    Config config;
    config.timeout = std::chrono::milliseconds(5000);
    const auto result = scenario(config);
    CHECK(result.status == Status::deadlock && result.events.empty());
    CHECK(result.final_states == std::vector<ThreadState>(64, ThreadState::blocked));
    CHECK(result.wait_for->size() == 64 * 63 && has_wait_cycle(*result.wait_for));
    CHECK(result.wait_for->front() == (WaitDependency{0,1,WaitKind::join,""}));
    CHECK(result.wait_for->back() == (WaitDependency{63,62,WaitKind::join,""}));
    const Trace trace{"dense-join/v1",config.max_steps,config.timeout,result};
    CHECK(decode_trace(encode_trace(trace)) == trace);
    CHECK(replay_trace(decode_trace(encode_trace(trace)), scenario) == result);
    CHECK(!has_wait_cycle({{0,1,WaitKind::join,""}, {0,2,WaitKind::join,""},
                           {1,3,WaitKind::join,""}, {2,3,WaitKind::join,""}}));
}

void what_if_counter_and_replay() {
    const auto& scenario = examples::scenario_named("lost-update");
    Config config;
    config.schedule = {0,1,0,1};
    const auto original = trace_for(scenario, scenario.run(config));
    const auto saved = encode_trace(original);
    CHECK(original.result.status == Status::assertion_failed);
    const auto changed_config = branch_config(original, 1, 0);
    CHECK(changed_config.schedule == (std::vector<ThreadId>{0,0}));
    const auto passing = scenario.run(changed_config);
    CHECK(passing.status == Status::completed && passing.shared_state->at(0).value == 2);
    CHECK(schedule(passing) == (std::vector<ThreadId>{0,0,1,1,2,2}));
    const auto passing_trace = trace_for(scenario, passing);
    const auto reverse = branch_config(passing_trace, 1, 1);
    for (int i = 0; i < 20; ++i) {
        CHECK(scenario.run(changed_config) == passing);
        CHECK(scenario.run(reverse) == original.result);
        CHECK(replay_trace(passing_trace, scenario.run) == passing);
        CHECK(replay_trace(original, scenario.run) == original.result);
    }
    CHECK(encode_trace(original) == saved);
    const auto invalid = scenario.run(branch_config(original, 1, 2));
    CHECK(invalid.status == Status::replay_mismatch && invalid.events.size() == 1);
    CHECK(invalid.message.find("T2 is not runnable at step 1; runnable=[0,1]") != std::string::npos);
    rejects([&] { branch_config(original, original.result.events.size(), 0); });
    rejects([&] { branch_config(original, 1, 64); });
    // Prefix request mismatches are rejected before the operation executes.
    auto changed = changed_config;
    changed.expected_prefix->at(0).label = "stale label";
    auto result = scenario.run(changed);
    CHECK(result.status == Status::replay_mismatch && result.events.empty());
    CHECK(result.shared_state->at(0).value == 0);
    changed = changed_config;
    changed.expected_prefix->at(0).observation.before = 99;
    result = scenario.run(changed);
    CHECK(result.status == Status::replay_mismatch && result.events.size() == 1);
    changed = changed_config;
    changed.branch_checkpoint->shared_state_before->at(0).value = 99;
    result = scenario.run(changed);
    CHECK(result.status == Status::replay_mismatch && result.events.size() == 1);
    CHECK(result.shared_state->at(0).value == 0); // Alternative store was never granted.
    changed = changed_config;
    changed.branch_checkpoint->runnable = {0};
    CHECK(scenario.run(changed).status == Status::replay_mismatch);
    changed = changed_config;
    changed.expected_prefix.reset();
    rejects([&] { scenario.run(changed); });
    Config bounded;
    bounded.max_steps = 2;
    const Trace bound_trace{std::string(scenario.program),2,bounded.timeout,scenario.run(bounded)};
    result = scenario.run(branch_config(bound_trace, 1, 1));
    CHECK(result.status == Status::step_limit && schedule(result) == (std::vector<ThreadId>{0,1}));
    CHECK(replay_trace({bound_trace.program,2,bounded.timeout,result}, scenario.run) == result);
}

void what_if_locks_and_checkpoint() {
    const auto& scenario = examples::scenario_named("mutex-deadlock");
    Config config;
    config.schedule = {0,1};
    const auto original = trace_for(scenario, scenario.run(config));
    const auto branch = branch_config(original, 1, 0);
    const auto passing = scenario.run(branch);
    CHECK(passing.status == Status::completed && schedule(passing) == (std::vector<ThreadId>{0,0,0,0,1,1,1,1}));
    CHECK(replay_trace(trace_for(scenario, passing), scenario.run) == passing);
    auto stale = branch;
    stale.branch_checkpoint->wait_for->push_back({1,0,WaitKind::mutex,"first"});
    const auto result = scenario.run(stale);
    CHECK(result.status == Status::replay_mismatch && result.events.size() == 1);
    CHECK(result.shared_state->at(1).value == std::nullopt);
    // A branch at step zero must validate a real checkpoint even with no prefix.
    const auto first = scenario.run(branch_config(original, 0, 1));
    CHECK(first.status == Status::deadlock && schedule(first) == (std::vector<ThreadId>{1,0}));
}

void what_if_ignores_native_arrival_order() {
    auto scenario = [](ThreadId delayed, const Config& config) {
        AtomicInt counter("counter",0);
        Scheduler scheduler;
        scheduler.track(counter);
        for (ThreadId id = 0; id < 2; ++id) scheduler.add_thread([&, id](Context& thread) {
            if (id == delayed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const auto local = thread.load(counter);
            if (id == delayed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            thread.store(counter,local + 1);
        });
        scheduler.add_thread([&](Context& thread) {
            thread.join_all({0,1});
            thread.assert_equal(counter,2,"counter == 2");
        });
        return scheduler.run(config);
    };
    Config config;
    config.timeout = std::chrono::milliseconds(5000);
    config.schedule = {0,1,0,1};
    const Trace original{"skew-counter/v1",config.max_steps,config.timeout,scenario(0,config)};
    CHECK(original.result.status == Status::assertion_failed);
    const auto branch = branch_config(original,1,0);
    const auto passing = scenario(0,branch);
    CHECK(passing.status == Status::completed);
    for (int repetition = 0; repetition < 12; ++repetition) {
        const auto delayed = static_cast<ThreadId>(repetition % 2);
        CHECK(scenario(delayed,branch) == passing);
        CHECK(replay_trace(original,[&](const Config& replay) { return scenario(delayed,replay); }) == original.result);
    }
}
}

int main() {
    const std::vector<std::pair<std::string, void(*)()>> tests{
        {"failing and corrected scenarios", failing_and_corrected_scenarios},
        {"opposite lock-order cycle", opposite_order_cycle_and_replay},
        {"consistent order blocks and resumes", consistent_order_blocks_and_resumes},
        {"lock waits release the execution permit", lock_waits_do_not_hold_the_execution_permit},
        {"retained owners, recursive locks and joins", retained_owners_recursive_locks_and_joins},
        {"wait-for replay and legacy traces", replay_validates_dependencies_and_accepts_legacy},
        {"invalid dependency graphs and event types", invalid_wait_graphs_and_event_types},
        {"repeated scenario search and replay", repeated_searches_and_replays},
        {"mixed waits with delayed startup", mixed_wait_graph_ignores_startup_timing},
        {"maximum thread ID and dense graph", maximum_thread_id_and_dense_join_graph},
        {"what-if counter, bounds, invalid choices and replay", what_if_counter_and_replay},
        {"what-if locks and branch checkpoint", what_if_locks_and_checkpoint},
        {"what-if ignores native arrival order", what_if_ignores_native_arrival_order}
    };
    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << tests.size() << " scenario cases, " << failures << " failures\n";
    return failures ? 1 : 0;
}
