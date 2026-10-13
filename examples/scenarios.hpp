#pragma once
#include "threadscope/scheduler.hpp"
#include <span>
#include <string_view>

namespace threadscope::examples {
struct Scenario {
    std::string_view name;
    std::string_view program;
    RunResult (*run)(const Config&);
    std::string_view description;
};
// Every registered runner constructs fresh resources, tasks, and scheduler.
std::span<const Scenario> scenarios();
const Scenario& scenario_named(std::string_view name);
const Scenario& scenario_program(std::string_view program);
}
