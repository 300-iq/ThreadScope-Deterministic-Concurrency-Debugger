#include "scenarios.hpp"
#include "threadscope/explore.hpp"
#include "threadscope/trace.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace threadscope;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #x); } while (false)
template<class Function> void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
}
using Paths = std::vector<std::vector<ThreadId>>;
Paths paths(const Exploration& search) {
    Paths result;
    for (const auto& run : search.runs) {
        std::vector<ThreadId> path;
        for (const auto& event : run.events) path.push_back(event.thread);
        result.push_back(std::move(path));
    }
    return result;
}
const Paths increment_paths{
    {0,0,1,1,2,2}, {0,1,0,1,2,2}, {0,1,1,0,2,2},
    {1,0,0,1,2,2}, {1,0,1,0,2,2}, {1,1,0,0,2,2}};
Trace trace_for(const examples::Scenario& scenario, const RunResult& result, const Config& config = {}) {
    return {std::string(scenario.program), config.max_steps, config.timeout, result};
}

void dfs_order_and_boundaries() {
    const auto& scenario = examples::scenario_named("lost-update");
    const auto all = explore(scenario.run);
    CHECK(all.exhausted && !all.step_bound_reached && !all.schedule_bound_reached);
    CHECK(paths(all) == increment_paths);
    for (const std::size_t limit : {1U,2U,5U,6U,7U}) {
        const auto search = explore(scenario.run, {}, limit);
        const auto count = std::min(limit, increment_paths.size());
        CHECK(search.runs.size() == count);
        CHECK(paths(search) == Paths(increment_paths.begin(), increment_paths.begin() + static_cast<std::ptrdiff_t>(count)));
        CHECK(search.exhausted == (limit >= 6));
        CHECK(search.schedule_bound_reached == (limit < 6));
    }
    Config bound;
    bound.max_steps = 1;
    const auto shallow = explore(scenario.run, bound);
    CHECK(shallow.exhausted && shallow.step_bound_reached && !shallow.schedule_bound_reached);
    CHECK(paths(shallow) == (Paths{{0}, {1}}));
    for (const auto& result : shallow.runs) {
        CHECK(result.status == Status::step_limit);
        CHECK(replay_trace(trace_for(scenario, result, bound), scenario.run) == result);
    }
    bound.max_steps = 6;
    CHECK(!explore(scenario.run, bound).step_bound_reached); // Complete exactly at the bound.
}

void repeated_searches_are_clean_and_identical() {
    const auto& scenario = examples::scenario_named("lost-update");
    const auto expected = explore(scenario.run);
    for (int repetition = 0; repetition < 20; ++repetition) {
        const auto actual = explore(scenario.run);
        CHECK(actual == expected);
        for (std::size_t i = 0; i < actual.runs.size(); ++i) {
            const auto& result = actual.runs[i];
            CHECK(result.events.front().shared_state_before->at(0).value == 0);
            const auto trace = trace_for(scenario, result);
            CHECK(encode_trace(trace) == encode_trace(trace_for(scenario, expected.runs[i])));
            CHECK(replay_trace(decode_trace(encode_trace(trace)), scenario.run) == result);
        }
    }
}

void three_way_choices_follow_depth_first_order() {
    auto scenario = [](const Config& config) {
        Scheduler scheduler;
        for (int i = 0; i < 3; ++i) scheduler.add_thread([](Context& thread) { thread.yield(); });
        return scheduler.run(config);
    };
    const auto search = explore(scenario);
    CHECK(search.exhausted);
    CHECK(paths(search) == (Paths{{0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}}));
}

void variable_lengths_and_terminated_workers() {
    auto scenario = [](const Config& config) {
        AtomicInt shared("shared", 0);
        Scheduler scheduler;
        scheduler.track(shared);
        scheduler.add_thread([&](Context& thread) { thread.store(shared, 1); });
        scheduler.add_thread([&](Context& thread) {
            if (thread.load(shared) == 0) thread.yield("early");
            else { thread.yield("late 1"); thread.yield("late 2"); }
        });
        scheduler.add_thread([](Context&) {});
        return scheduler.run(config);
    };
    const auto search = explore(scenario);
    CHECK(search.exhausted);
    CHECK(paths(search) == (Paths{{0,1,1,1}, {1,0,1}, {1,1,0}}));
    for (const auto& run : search.runs) {
        CHECK(run.status == Status::completed);
        CHECK(run.events.front().shared_state_before->at(0).value == 0);
        CHECK(run.shared_state->at(0).value == 1);
        for (const auto& event : run.events) {
            CHECK(event.states[2] == ThreadState::terminated);
            CHECK(std::find(event.runnable.begin(), event.runnable.end(), 2) == event.runnable.end());
        }
        CHECK(replay_trace({"variable/v1",1000,std::chrono::milliseconds(1000),run}, scenario) == run);
    }
}

void dead_ends_are_saved_and_replayable() {
    const auto& scenario = examples::scenario_named("mutex-deadlock");
    const auto search = explore(scenario.run);
    CHECK(search.exhausted);
    // Independent enumeration of the two four-operation lock/unlock sequences.
    CHECK(paths(search) == (Paths{
        {0,0,0,0,1,1,1,1}, {0,0,0,1,0,1,1,1}, {0,1}, {1,0},
        {1,1,1,0,1,0,0,0}, {1,1,1,1,0,0,0,0}}));
    std::size_t deadlocks = 0;
    for (const auto& run : search.runs) {
        for (const auto& event : run.events) {
            CHECK(std::find(event.runnable.begin(), event.runnable.end(), event.thread) != event.runnable.end());
            CHECK(event.shared_state_before->size() == 2 && event.shared_state_after->size() == 2);
        }
        if (run.status == Status::deadlock) {
            ++deadlocks;
            CHECK(run.final_states == (std::vector<ThreadState>(2, ThreadState::blocked)));
            CHECK(run.shared_state->at(0).value == 0);
            CHECK(run.shared_state->at(1).value == 1);
        } else {
            CHECK(run.status == Status::completed);
            CHECK(!run.shared_state->at(0).value && !run.shared_state->at(1).value);
        }
        for (int i = 0; i < 10; ++i) CHECK(replay_trace(trace_for(scenario, run), scenario.run) == run);
    }
    CHECK(deadlocks == 2);
}

void replay_rejects_metadata_before_mutation() {
    const auto& scenario = examples::scenario_named("lost-update");
    const auto original = trace_for(scenario, scenario.run({}));
    auto changed = original;
    changed.result.events[0].runnable = {0};
    changed.result.events[0].states[1] = ThreadState::blocked;
    auto result = replay_trace(changed, scenario.run);
    CHECK(result.status == Status::replay_mismatch && result.events.empty());
    CHECK(result.message.find("runnable set") != std::string::npos);
    CHECK(result.shared_state->at(0).value == 0);
    changed = original;
    changed.result.events[1].operation = "yield";
    result = replay_trace(changed, scenario.run);
    CHECK(result.status == Status::replay_mismatch && result.events.size() == 1);
    CHECK(result.message.find("operation") != std::string::npos);
    CHECK(result.shared_state->at(0).value == 0); // Divergent store was not executed.
    changed = original;
    changed.result.events[0].shared_state_before->at(0).value = 99;
    result = replay_trace(changed, scenario.run);
    CHECK(result.status == Status::replay_mismatch && result.events.empty());
    changed = original;
    changed.result.events[0].shared_state_after->at(0).value = 99;
    result = replay_trace(changed, scenario.run);
    CHECK(result.status == Status::replay_mismatch && result.events.size() == 1);
    changed = original;
    changed.result.shared_state->at(0).value = 99;
    CHECK(replay_trace(changed, scenario.run).status == Status::replay_mismatch);
    changed = original;
    changed.result.final_states[0] = ThreadState::blocked;
    rejects([&] { replay_trace(changed, scenario.run); }); // Invalid completion is rejected before execution.
    // A runner ignoring Config must not fool the high-level replay validator.
    CHECK(replay_trace(original, [&](const Config&) {
        auto fake = original.result;
        fake.events[0].label = "wrong operation label";
        return fake;
    }).status == Status::replay_mismatch);
    CHECK(replay_trace(original,[&](const Config&) {
        auto fake = original.result;
        fake.final_states[0] = ThreadState::blocked;
        return fake;
    }).status == Status::replay_mismatch);
}

void legacy_v1_traces_remain_replayable() {
    const auto& scenario = examples::scenario_named("lost-update");
    Config config;
    config.schedule = {0,1,0,1};
    auto trace = trace_for(scenario, scenario.run(config));
    trace.result.shared_state.reset();
    for (auto& event : trace.result.events) { event.shared_state_before.reset(); event.shared_state_after.reset(); }
    const auto decoded = decode_trace(encode_trace(trace));
    CHECK(decoded == trace);
    const auto replayed = replay_trace(decoded, scenario.run);
    CHECK(replayed.status == Status::assertion_failed);
    CHECK(replayed.shared_state->at(0).value == 1);
    CHECK(replayed.events.size() == trace.result.events.size());
}

void invalid_snapshots_and_resource_registration() {
    const auto& scenario = examples::scenario_named("lost-update");
    auto trace = trace_for(scenario, scenario.run({}));
    trace.result.events[0].shared_state_after.reset();
    rejects([&] { decode_trace(encode_trace(trace)); });
    trace = trace_for(scenario, scenario.run({}));
    trace.result.shared_state->at(0).name = "different";
    rejects([&] { decode_trace(encode_trace(trace)); });
    trace = trace_for(scenario, scenario.run({}));
    trace.result.shared_state->at(0).value.reset();
    rejects([&] { decode_trace(encode_trace(trace)); });
    auto lock_trace = trace_for(examples::scenario_named("mutex-deadlock"), examples::scenario_named("mutex-deadlock").run({}));
    lock_trace.result.shared_state->at(0).value = 99;
    rejects([&] { decode_trace(encode_trace(lock_trace)); });
    AtomicInt value("same"), empty("");
    ControlledMutex mutex("same");
    Scheduler scheduler;
    scheduler.track(value);
    rejects([&] { scheduler.track(value); });
    rejects([&] { scheduler.track(mutex); });
    rejects([&] { scheduler.track(empty); });
    const auto zero_threads = scheduler.run();
    CHECK(zero_threads.shared_state->at(0).value == 0);
    CHECK(decode_trace(encode_trace({"empty/v1",1000,std::chrono::milliseconds(1000),zero_threads})).result == zero_threads);
    rejects([&] { scheduler.track(mutex); });
}

void callbacks_and_clean_state_contract() {
    const auto& scenario = examples::scenario_named("lost-update");
    std::size_t starts = 0, observations = 0;
    const auto search = explore([&](const Config& config) {
        CHECK(starts == observations); ++starts;
        return scenario.run(config);
    }, {}, 100, [&](const RunResult& result) { CHECK(!result.events.empty()); ++observations; });
    CHECK(search.exhausted && starts == 6 && observations == 6);
    starts = 0;
    rejects([&] { explore([&](const Config& config) { ++starts; return scenario.run(config); }, {}, 100,
                           [](const RunResult&) { throw std::runtime_error("trace write failed"); }); });
    CHECK(starts == 1);
    // Violating the fresh-state factory contract must not produce an exhaustive claim.
    AtomicInt leaked("leaked", 0);
    const auto invalid = explore([&](const Config& config) {
        Scheduler scheduler;
        scheduler.track(leaked);
        for (int i = 0; i < 2; ++i) scheduler.add_thread([&](Context& thread) { thread.fetch_add(leaked, 1); });
        return scheduler.run(config);
    });
    CHECK(!invalid.exhausted);
    CHECK(invalid.runs.back().status == Status::replay_mismatch);
    CHECK(invalid.runs.back().message.find("prefix divergence") != std::string::npos);
}

void limits_and_empty_program() {
    bool invoked = false;
    auto empty = [&](const Config& config) { invoked = true; Scheduler scheduler; return scheduler.run(config); };
    rejects([&] { explore(empty, {}, 0); });
    Config invalid;
    invalid.max_steps = 0;
    rejects([&] { explore(empty, invalid); });
    CHECK(!invoked);
    const auto empty_search = explore(empty, {}, 1);
    CHECK(empty_search.exhausted && !empty_search.schedule_bound_reached && empty_search.runs.size() == 1);
    Config bound;
    bound.max_steps = 64;
    const auto loops = explore([](const Config& config) {
        Scheduler scheduler;
        for (int i = 0; i < 2; ++i) scheduler.add_thread([](Context& thread) { while (true) thread.yield(); });
        return scheduler.run(config);
    }, bound, 3);
    CHECK(loops.runs.size() == 3 && loops.step_bound_reached && loops.schedule_bound_reached && !loops.exhausted);
    for (const auto& run : loops.runs) CHECK(run.events.size() == 64 && run.status == Status::step_limit);
}

void factories_cannot_silently_skip_choices() {
    // This factory is fresh and uses a real Scheduler, but accidentally overrides
    // the explorer's default choice. DFS must not claim that its one leaf covers
    // both possible orders of these independent workers.
    const auto wrong_policy = explore([](const Config& config) {
        Scheduler scheduler;
        for (int i = 0; i < 2; ++i) scheduler.add_thread([](Context& thread) { thread.yield(); });
        auto changed = config;
        if (changed.schedule.empty()) changed.schedule = {1};
        return scheduler.run(changed);
    });
    CHECK(!wrong_policy.exhausted && wrong_policy.runs.size() == 1);
    CHECK(wrong_policy.runs.back().status == Status::replay_mismatch);
    CHECK(wrong_policy.runs.back().message.find("lowest runnable ID") != std::string::npos);

    // Also check an override inside the unforced suffix, after a valid prefix.
    std::size_t starts = 0, rejected_callbacks = 0;
    const auto wrong_suffix = explore([&](const Config& config) {
        ++starts;
        Scheduler scheduler;
        for (int i = 0; i < 3; ++i) scheduler.add_thread([](Context& thread) { thread.yield(); });
        auto changed = config;
        if (changed.schedule.empty()) changed.schedule = {0,2};
        return scheduler.run(changed);
    }, {}, 100, [&](const RunResult& result) {
        CHECK(result.status == Status::replay_mismatch);
        ++rejected_callbacks;
    });
    CHECK(!wrong_suffix.exhausted && starts == 1 && rejected_callbacks == 1);
    CHECK(wrong_suffix.runs.back().message.find("step 1") != std::string::npos);
}

void factories_cannot_silently_override_bounds() {
    // An overridden limit also used to produce an exhaustive, untruncated
    // search despite executing more operations than the caller authorized.
    Config limit;
    limit.max_steps = 1;
    const auto wrong_bound = explore([](const Config& config) {
        Scheduler scheduler;
        scheduler.add_thread([](Context& thread) { thread.yield(); thread.yield(); });
        auto changed = config;
        changed.max_steps = 2;
        return scheduler.run(changed);
    }, limit);
    CHECK(!wrong_bound.exhausted && wrong_bound.runs.size() == 1);
    CHECK(wrong_bound.runs.back().status == Status::replay_mismatch);
    CHECK(wrong_bound.runs.back().message.find("step bound exceeded") != std::string::npos);
}
void factories_cannot_hide_runnable_choices() {
    rejects([] {
        explore([](const Config& config) {
            Scheduler scheduler;
            for (int i = 0; i < 2; ++i) scheduler.add_thread([](Context& context) { context.yield(); });
            auto result = scheduler.run(config);
            // Both states are runnable. Filtering the enabled set used to
            // falsely report one exhaustive leaf and skip the other order.
            result.events[0].runnable = {result.events[0].thread};
            return result;
        });
    });
}
void bounded_dfs_matches_independent_state_model() {
    struct Model { std::array<int,3> pc{}; int flag{}; };
    auto native = [](const Config& config) {
        AtomicInt flag("flag",0);
        Scheduler scheduler;
        scheduler.track(flag);
        scheduler.add_thread([&](Context& context) { if (context.load(flag) == 0) context.yield("zero branch"); });
        scheduler.add_thread([&](Context& context) { context.store(flag,1); });
        scheduler.add_thread([](Context& context) { context.yield("a"); context.yield("b"); });
        return scheduler.run(config);
    };
    for (std::size_t bound = 1; bound <= 6; ++bound) {
        Paths reference;
        std::function<void(Model,std::vector<ThreadId>)> enumerate = [&](Model model,std::vector<ThreadId> path) {
            std::vector<ThreadId> runnable;
            for (ThreadId id = 0; id < 3; ++id)
                if (model.pc[id] < (id == 1 ? 1 : 2)) runnable.push_back(id);
            if (runnable.empty() || path.size() == bound) { reference.push_back(std::move(path)); return; }
            for (const auto id : runnable) {
                auto next = model;
                if (id == 0 && model.pc[0] == 0) next.pc[0] = model.flag == 0 ? 1 : 2;
                else ++next.pc[id];
                if (id == 1) next.flag = 1;
                auto choices = path; choices.push_back(id);
                enumerate(next,std::move(choices));
            }
        };
        enumerate({},{});
        Config limits; limits.max_steps = bound; limits.timeout = std::chrono::milliseconds(5000);
        for (const auto cap : {std::size_t{1},std::size_t{2},reference.size()-1,reference.size(),reference.size()+1}) {
            const auto actual = explore(native,limits,cap);
            const auto count = std::min(cap,reference.size());
            const auto actual_paths = paths(actual);
            CHECK(actual_paths == Paths(reference.begin(),reference.begin()+static_cast<std::ptrdiff_t>(count)));
            CHECK(actual.exhausted == (cap >= reference.size()));
            CHECK(actual.schedule_bound_reached == (cap < reference.size()));
            CHECK(std::set(actual_paths.begin(),actual_paths.end()).size() == actual_paths.size());
        }
    }
}
}

int main() {
    const std::vector<std::pair<std::string, void(*)()>> tests{
        {"DFS order and bounds", dfs_order_and_boundaries}, {"repeated clean searches", repeated_searches_are_clean_and_identical},
        {"three-way DFS", three_way_choices_follow_depth_first_order}, {"variable lengths and termination", variable_lengths_and_terminated_workers},
        {"dead ends and replay", dead_ends_are_saved_and_replayable}, {"pre-operation divergence", replay_rejects_metadata_before_mutation},
        {"legacy v1 replay", legacy_v1_traces_remain_replayable}, {"invalid snapshots", invalid_snapshots_and_resource_registration},
        {"persistence callback and clean state", callbacks_and_clean_state_contract}, {"empty programs and limits", limits_and_empty_program},
        {"factory scheduling policy", factories_cannot_silently_skip_choices},
        {"factory scheduling bounds", factories_cannot_silently_override_bounds},
        {"factory runnable-set completeness", factories_cannot_hide_runnable_choices},
        {"independent variable-length bounded DFS model", bounded_dfs_matches_independent_state_model}
    };
    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << tests.size() << " exploration cases, " << failures << " failures\n";
    return failures ? 1 : 0;
}
