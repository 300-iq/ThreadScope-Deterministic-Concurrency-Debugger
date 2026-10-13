#include "scenarios.hpp"
#include "threadscope/explore.hpp"
#include "threadscope/trace.hpp"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace threadscope;
std::size_t number(std::string_view text, std::size_t maximum) {
    std::size_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value > maximum)
        throw std::invalid_argument("invalid numeric argument: " + std::string(text));
    return value;
}
std::vector<ThreadId> parse_schedule(std::string_view text) {
    std::vector<ThreadId> schedule;
    while (true) {
        const auto comma = text.find(',');
        schedule.push_back(static_cast<ThreadId>(number(text.substr(0, comma), 63)));
        if (schedule.size() > 1'000'000) throw std::invalid_argument("schedule too long");
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return schedule;
}
int exit_code(Status status) {
    if (status == Status::completed) return 0;
    if (status == Status::assertion_failed || status == Status::deadlock) return 1;
    return 2;
}
void print_waits(const WaitForGraph& graph, std::string_view indent = "") {
    for (const auto& edge : graph) {
        std::cout << indent << "T" << edge.thread << " waits for T" << edge.waits_for;
        if (edge.kind == WaitKind::mutex) std::cout << " holding lock " << edge.resource;
        else std::cout << " to terminate (join)";
        std::cout << '\n';
    }
}
void summary(const RunResult& result) {
    std::cout << "status=" << to_string(result.status) << " steps=" << result.events.size() << " schedule=";
    bool first = true;
    for (const auto& event : result.events) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << event.thread;
    }
    for (const auto& event : result.events) {
        if (event.operation == "assert_equal") std::cout << ' ' << event.resource << '=' << *event.observation.before;
    }
    std::cout << '\n';
    if (!result.message.empty()) std::cout << result.message << '\n';
    std::cout << "threads:";
    for (std::size_t i = 0; i < result.final_states.size(); ++i)
        std::cout << " T" << i << '=' << to_string(result.final_states[i]);
    std::cout << '\n';
    if (result.shared_state) {
        for (const auto& resource : *result.shared_state) {
            if (resource.kind == ResourceKind::mutex) {
                std::cout << "lock " << resource.name << " owner=";
                if (resource.value) std::cout << 'T' << *resource.value;
                else std::cout << "none";
                std::cout << '\n';
            }
        }
    }
    if (result.wait_for) {
        print_waits(*result.wait_for);
        if (has_wait_cycle(*result.wait_for)) {
            std::cout << "Modeled wait-for cycle detected.\n";
            if (result.status != Status::deadlock)
                std::cout << "Other workers may still be runnable; this result is " << to_string(result.status) << ".\n";
        }
    }
}
void usage() {
    std::cout << "ThreadScope: deterministic instrumented scenarios\n"
        "  threadscope run <scenario> [--schedule 0,1,0,1] [--trace FILE]\n"
        "  threadscope explore <scenario> --max-schedules N --max-steps N [--out DIR] [--all-traces]\n"
        "  threadscope replay FILE\n"
        "  threadscope branch FILE --step N --thread ID [--trace NEW_FILE]\n"
        "Branch verifies the original, preserves choices before step N, then selects ID.\n"
        "Run/branch suffix policy: always select the lowest runnable thread ID.\n"
        "Run/explore limits: --max-steps N --timeout-ms N\n"
        "Defaults: lost-update, 100 schedules, 1000 steps, 1000 ms per schedule.\n"
        "Compatibility aliases: --max-runs, --fixed (lost-update-rmw).\n"
        "Exit codes: 0 completed/no failures; 1 assertion/deadlock found; 2 error or incomplete run/search.\n"
        "Scenarios:\n";
    for (const auto& scenario : examples::scenarios())
        std::cout << "  " << scenario.name << ": " << scenario.description << '\n';
}
}

int main(int argc, char** argv) {
    using namespace threadscope;
    try {
        if (argc < 2 || std::string_view(argv[1]) == "--help") { usage(); return argc < 2 ? 2 : 0; }
        const std::string command = argv[1];
        if (command == "branch") {
            if (argc < 3) throw std::invalid_argument("branch requires an original trace path");
            std::optional<std::size_t> step, thread;
            std::filesystem::path output;
            for (int i = 3; i < argc; ++i) {
                const std::string flag = argv[i];
                if (++i >= argc) throw std::invalid_argument("missing value for " + flag);
                if (flag == "--step" && !step) step = number(argv[i], 1'000'000);
                else if (flag == "--thread" && !thread) thread = number(argv[i], 63);
                else if (flag == "--trace" && output.empty()) output = argv[i];
                else throw std::invalid_argument("unsupported or duplicate branch option: " + flag);
            }
            if (!step || !thread) throw std::invalid_argument("branch requires --step and --thread");
            if (!output.empty() && (std::filesystem::weakly_canonical(output) == std::filesystem::weakly_canonical(argv[2]) ||
                (std::filesystem::exists(output) && std::filesystem::equivalent(output, argv[2]))))
                throw std::invalid_argument("branch output must preserve the original trace; choose a different file");
            const auto original = load_trace(argv[2]);
            const auto config = branch_config(original, *step, static_cast<ThreadId>(*thread));
            const auto& scenario = examples::scenario_program(original.program);
            const auto verified = replay_trace(original, scenario.run);
            if (verified.status == Status::replay_mismatch || verified.status == Status::timeout || verified.status == Status::worker_error) {
                std::cerr << "original replay could not be verified: " << verified.message << '\n';
                return 2;
            }
            std::cout << "Original replay verified; running a fresh branch.\n"
                << "Branch step=" << *step << " thread=" << *thread << " suffix_policy=lowest_runnable_id\n";
            const auto result = scenario.run(config);
            summary(result);
            if (!output.empty()) save_trace(output, {original.program, config.max_steps, config.timeout, result});
            return exit_code(result.status);
        }
        if (command == "replay") {
            if (argc != 3) throw std::invalid_argument("replay requires one trace path");
            const auto trace = load_trace(argv[2]);
            const auto& scenario = examples::scenario_program(trace.program);
            auto result = replay_trace(trace, scenario.run);
            if (result.status == Status::timeout || result.status == Status::worker_error) {
                std::cerr << "replay could not be verified: " << to_string(result.status) << '\n';
                summary(result);
                return 2;
            }
            if (result.status == Status::replay_mismatch) {
                std::cerr << "replay mismatch: " << result.message << '\n';
                return 2;
            }
            std::cout << "Replay verified: every event and final outcome match.\n";
            summary(result);
            return exit_code(result.status);
        }
        if (command != "run" && command != "explore") throw std::invalid_argument("unknown command: " + command);
        Config config;
        bool fixed = false;
        bool all_traces = false;
        std::filesystem::path trace_path;
        std::filesystem::path output = "artifacts";
        std::size_t max_schedules = 100;
        std::string scenario_name = "lost-update";
        int first_option = 2;
        if (argc > 2 && !std::string_view(argv[2]).starts_with('-')) {
            scenario_name = argv[2];
            first_option = 3;
        }
        for (int i = first_option; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--fixed") { fixed = true; continue; }
            if (flag == "--all-traces" && command == "explore") { all_traces = true; continue; }
            if (++i >= argc) throw std::invalid_argument("missing value for " + flag);
            const std::string_view argument = argv[i];
            if (flag == "--schedule" && command == "run") config.schedule = parse_schedule(argument);
            else if (flag == "--trace" && command == "run") trace_path = argument;
            else if (flag == "--out" && command == "explore") output = argument;
            else if ((flag == "--max-schedules" || flag == "--max-runs") && command == "explore")
                max_schedules = number(argument, 10'000);
            else if (flag == "--max-steps") config.max_steps = number(argument, 1'000'000);
            else if (flag == "--timeout-ms") config.timeout = std::chrono::milliseconds(number(argument, 60'000));
            else throw std::invalid_argument("unsupported option: " + flag);
        }
        if (fixed) {
            if (scenario_name == "lost-update") scenario_name = "lost-update-rmw";
            else if (scenario_name != "lost-update-rmw") throw std::invalid_argument("--fixed applies only to lost-update");
        }
        const auto& scenario = examples::scenario_named(scenario_name);
        const std::string program(scenario.program);
        std::cout << "Scenario " << scenario.name << ": " << scenario.description << '\n';
        if (command == "run") {
            const auto result = scenario.run(config);
            summary(result);
            for (const auto& event : result.events) {
                std::cout << "  " << event.step << " T" << event.thread << ' ' << to_string(event.type())
                          << " (" << event.operation << ") " << event.resource << " runnable=[";
                for (std::size_t i = 0; i < event.runnable.size(); ++i) {
                    if (i) std::cout << ',';
                    std::cout << event.runnable[i];
                }
                std::cout << ']';
                if (event.observation.before) std::cout << " before=" << *event.observation.before;
                if (event.observation.after) std::cout << " after=" << *event.observation.after;
                if (event.observation.expected) std::cout << " expected=" << *event.observation.expected;
                std::cout << '\n';
                if (event.wait_for) print_waits(*event.wait_for, "    ");
            }
            if (!trace_path.empty()) {
                save_trace(trace_path, {program, config.max_steps, config.timeout, result});
                std::cout << "Trace saved: " << trace_path.string() << '\n';
            }
            return exit_code(result.status);
        }
        std::size_t failures = 0;
        std::size_t schedules = 0;
        const auto exploration = explore(scenario.run, config, max_schedules, [&](const RunResult& result) {
            ++schedules;
            summary(result);
            if (all_traces) {
                const auto path = output / ("schedule-" + std::to_string(schedules) + ".json");
                save_trace(path, {program, config.max_steps, config.timeout, result});
                std::cout << "Trace saved: " << path.string() << '\n';
            }
            if (result.status == Status::assertion_failed || result.status == Status::deadlock) {
                ++failures;
                const auto path = output / ("failure-" + std::to_string(failures) + ".json");
                save_trace(path, {program, config.max_steps, config.timeout, result});
                std::cout << "Trace saved: " << path.string() << '\n';
            }
        });
        bool incomplete = !exploration.exhausted || exploration.step_bound_reached;
        for (const auto& result : exploration.runs)
            if (result.status != Status::completed && result.status != Status::assertion_failed && result.status != Status::deadlock)
                incomplete = true;
        std::cout << "runs=" << exploration.runs.size() << " failures=" << failures
                  << " frontier_exhausted=" << (exploration.exhausted ? "true" : "false")
                  << " incomplete=" << (incomplete ? "true" : "false")
                  << " schedule_bound_reached=" << (exploration.schedule_bound_reached ? "true" : "false")
                  << " step_bound_reached=" << (exploration.step_bound_reached ? "true" : "false") << '\n';
        if (incomplete) return 2;
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
