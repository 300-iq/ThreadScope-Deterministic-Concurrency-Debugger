#pragma once
#include "threadscope/scheduler.hpp"

namespace threadscope {
struct Exploration {
    std::vector<RunResult> runs;
    bool exhausted{false};
    bool step_bound_reached{false};
    bool schedule_bound_reached{false};
    bool operator==(const Exploration&) const = default;
};
// Stateless bounded DFS: recreate all scenario resources for every invocation.
// Choices are visited in ascending logical ID order. No partial-order reduction.
// Factories must honor Config, including the lowest-ID unforced suffix policy.
// Detectable policy/bound/prefix violations stop with replay_mismatch; hidden
// state and elapsed-time compliance cannot be certified from returned events.
// on_result runs after workers are joined and before starting the next schedule.
Exploration explore(const std::function<RunResult(const Config&)>& scenario,
                    const Config& limits = {}, std::size_t max_schedules = 100,
                    const std::function<void(const RunResult&)>& on_result = {});
}
