#include "scenarios.hpp"
#include "lost_update.hpp"
#include <array>
#include <stdexcept>

namespace threadscope::examples {
namespace {
RunResult increment(const Config& config) { return lost_update(config); }
RunResult increment_rmw(const Config& config) { return lost_update(config, true); }
RunResult mutex_deadlock(const Config& config) {
    ControlledMutex first("first"), second("second");
    Scheduler scheduler;
    scheduler.track(first);
    scheduler.track(second);
    scheduler.add_thread([&](Context& thread) {
        thread.lock(first); thread.lock(second);
        thread.unlock(second); thread.unlock(first);
    });
    scheduler.add_thread([&](Context& thread) {
        thread.lock(second); thread.lock(first);
        thread.unlock(first); thread.unlock(second);
    });
    return scheduler.run(config);
}
RunResult mutex_ordered(const Config& config) {
    ControlledMutex first("first"), second("second");
    Scheduler scheduler;
    scheduler.track(first);
    scheduler.track(second);
    for (int i = 0; i < 2; ++i) scheduler.add_thread([&](Context& thread) {
        thread.lock(first); thread.lock(second);
        thread.unlock(second); thread.unlock(first);
    });
    return scheduler.run(config);
}
const std::array<Scenario, 4> registry{{
    {"lost-update", "lost-update/v1", increment, "Separate atomic loads/stores can lose an increment."},
    {"lost-update-rmw", "lost-update-rmw/v1", increment_rmw, "Atomic fetch_add preserves both increments."},
    {"mutex-deadlock", "mutex-deadlock/v1", mutex_deadlock, "Opposite lock orders can form a wait-for cycle."},
    {"mutex-ordered", "mutex-ordered/v1", mutex_ordered, "Both workers acquire first, then second."}
}};
}
std::span<const Scenario> scenarios() { return registry; }
const Scenario& scenario_named(std::string_view name) {
    for (const auto& scenario : registry) if (scenario.name == name) return scenario;
    throw std::invalid_argument("unknown scenario: " + std::string(name));
}
const Scenario& scenario_program(std::string_view program) {
    for (const auto& scenario : registry) if (scenario.program == program) return scenario;
    throw std::invalid_argument("unsupported program identifier: " + std::string(program));
}
}
