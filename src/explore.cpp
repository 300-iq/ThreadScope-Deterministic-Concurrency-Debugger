#include "threadscope/explore.hpp"
#include <algorithm>
#include <stdexcept>

namespace threadscope {
Exploration explore(const std::function<RunResult(const Config&)>& scenario,
                    const Config& limits, std::size_t max_schedules,
                    const std::function<void(const RunResult&)>& on_result) {
    if (max_schedules == 0 || !scenario)
        throw std::invalid_argument("exploration requires a scenario and positive schedule limit");
    if (limits.max_steps == 0 || limits.max_steps > 1'000'000 ||
        limits.timeout.count() <= 0 || limits.timeout.count() > 60'000)
        throw std::invalid_argument("limits require 1..1000000 steps and 1..60000 ms");
    if (!limits.schedule.empty() || limits.exact_schedule || limits.expected_events || limits.expected_prefix || limits.branch_checkpoint)
        throw std::invalid_argument("exploration requires unforced scheduling");

    // Reference immutable decisions in previous runs, instead of copying all
    // alternative prefixes. This spine stores at most one frame per step.
    struct Frame { std::size_t run; std::size_t step; std::size_t choice; };
    std::vector<Frame> spine;
    std::vector<ThreadId> prefix;
    Exploration result;
    while (true) {
        auto config = limits;
        config.schedule = prefix;
        auto run = scenario(config); // Contract: constructs clean initial state.
        const bool environmental = run.status == Status::timeout || run.status == Status::worker_error;
        if (!environmental && run.status != Status::replay_mismatch) {
            if (run.events.size() > limits.max_steps) {
                run.status = Status::replay_mismatch;
                run.message = "exploration configuration divergence: scheduling step bound exceeded";
            }
            for (std::size_t i = 0; i < run.events.size(); ++i) {
                const auto& event = run.events[i];
                if (event.states.size() > 64 || event.states.size() != run.final_states.size())
                    throw std::invalid_argument("scenario returned an invalid thread table");
                std::vector<ThreadId> enabled;
                for (std::size_t id = 0; id < event.states.size(); ++id) {
                    if (event.states[id] == ThreadState::running)
                        throw std::invalid_argument("scenario returned a non-quiescent decision trace");
                    if (event.states[id] == ThreadState::runnable) enabled.push_back(static_cast<ThreadId>(id));
                }
                if (event.step != i || event.runnable.empty() ||
                    event.thread >= event.states.size() || event.runnable != enabled ||
                    !std::is_sorted(event.runnable.begin(), event.runnable.end()) ||
                    std::adjacent_find(event.runnable.begin(), event.runnable.end()) != event.runnable.end() ||
                    std::find(event.runnable.begin(), event.runnable.end(), event.thread) == event.runnable.end())
                    throw std::invalid_argument("scenario returned an invalid decision trace");
                // DFS advances from the selected index. A non-minimum default
                // suffix would permanently skip lower choices in that subtree.
                if (run.status != Status::replay_mismatch && i >= prefix.size() &&
                    event.thread != event.runnable.front()) {
                    run.status = Status::replay_mismatch;
                    run.message = "exploration configuration divergence at step " + std::to_string(i) +
                                  ": default choice is not the lowest runnable ID";
                }
            }
            for (std::size_t i = 0; run.status != Status::replay_mismatch && i < prefix.size(); ++i) {
                const auto& frame = spine[i];
                const auto& expected = result.runs[frame.run].events[frame.step];
                const bool missing = i >= run.events.size();
                bool changed = false;
                if (!missing) {
                    const auto& actual = run.events[i];
                    changed = actual.thread != prefix[i];
                    if (i + 1 < prefix.size()) changed |= actual != expected;
                    else changed |= actual.runnable != expected.runnable || actual.states != expected.states ||
                                    actual.shared_state_before != expected.shared_state_before ||
                                    actual.wait_for != expected.wait_for;
                }
                if (missing || changed) {
                    run.status = Status::replay_mismatch;
                    run.message = "exploration prefix divergence at step " + std::to_string(i);
                    break;
                }
            }
        }
        const auto status = run.status;
        result.step_bound_reached |= status == Status::step_limit;
        if (on_result) on_result(run); // Workers joined; persist before advancing.
        const auto run_index = result.runs.size();
        result.runs.push_back(std::move(run));
        if (environmental || status == Status::replay_mismatch) return result;

        const auto& events = result.runs.back().events;
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (i < prefix.size()) spine[i].run = run_index;
            else {
                const auto position = std::find(events[i].runnable.begin(), events[i].runnable.end(), events[i].thread);
                spine.push_back({run_index, i, static_cast<std::size_t>(position - events[i].runnable.begin())});
            }
        }
        // Pop exhausted subtrees and advance the deepest remaining choice.
        while (!spine.empty()) {
            auto& frame = spine.back();
            const auto& runnable = result.runs[frame.run].events[frame.step].runnable;
            if (++frame.choice < runnable.size()) break;
            spine.pop_back();
        }
        if (spine.empty()) { result.exhausted = true; return result; }
        if (result.runs.size() >= max_schedules) {
            result.schedule_bound_reached = true;
            return result;
        }
        prefix.clear();
        for (const auto& frame : spine)
            prefix.push_back(result.runs[frame.run].events[frame.step].runnable[frame.choice]);
    }
}
}
