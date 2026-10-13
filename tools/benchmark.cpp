#include "scenarios.hpp"
#include "threadscope/explore.hpp"
#include "threadscope/trace.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using namespace threadscope;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
std::size_t number(std::string_view text,std::size_t maximum) {
    std::size_t value{};
    const auto [end,error] = std::from_chars(text.data(),text.data()+text.size(),value);
    if (error != std::errc{} || end != text.data()+text.size() || value == 0 || value > maximum)
        throw std::invalid_argument("invalid positive benchmark bound");
    return value;
}
std::int64_t elapsed(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
}
// Same final counter behavior and three native workers as lost-update-rmw.
// No controlled permits, snapshots, event recording, or logical join operation.
void native_counter() {
    std::atomic<int> counter{0};
    int observed = 0;
    std::thread first, second, checker;
    try {
        first = std::thread([&] { counter.fetch_add(1,std::memory_order_relaxed); });
        second = std::thread([&] { counter.fetch_add(1,std::memory_order_relaxed); });
        checker = std::thread([&] {
            first.join(); second.join();
            observed = counter.load(std::memory_order_relaxed);
        });
        checker.join(); // Synchronizes the ordinary observed write with the caller.
    } catch (...) {
        // Construction can fail after another worker starts. Join the checker
        // first, if launched, because it owns joining the increment workers.
        if (checker.joinable()) checker.join();
        if (first.joinable()) first.join();
        if (second.joinable()) second.join();
        throw;
    }
    if (observed != 2) throw std::runtime_error("native counter baseline failed");
}
}
int main(int argc,char** argv) {
    try {
        Config config; config.max_steps = 8; config.timeout = std::chrono::milliseconds(5000);
        std::size_t trials = 10, schedules = 100, samples = 100;
        std::filesystem::path output;
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            if (++i >= argc) throw std::invalid_argument("missing benchmark option value");
            if (flag == "--trials") trials = number(argv[i],100);
            else if (flag == "--max-schedules") schedules = number(argv[i],10000);
            else if (flag == "--max-steps") config.max_steps = number(argv[i],1000000);
            else if (flag == "--timeout-ms") config.timeout = std::chrono::milliseconds(number(argv[i],60000));
            else if (flag == "--baseline-samples") samples = number(argv[i],10000);
            else if (flag == "--out") output = argv[i];
            else throw std::invalid_argument("unknown benchmark option");
        }
        if (output.empty()) throw std::invalid_argument("benchmark requires --out for actual traces");
        Json records = Json::array();
        // One warmup search per scenario, outside reported measurements.
        for (const auto& scenario : examples::scenarios()) (void)explore(scenario.run,config,schedules);
        for (std::size_t trial = 0; trial < trials; ++trial) {
            for (const auto& scenario : examples::scenarios()) {
                const auto start = Clock::now();
                const auto search = explore(scenario.run,config,schedules);
                const auto duration = elapsed(start); // No filesystem/JSON work inside timed region.
                Json traces = Json::array();
                std::size_t assertions = 0, deadlocks = 0, errors = 0;
                for (std::size_t run = 0; run < search.runs.size(); ++run) {
                    const auto& result = search.runs[run];
                    assertions += result.status == Status::assertion_failed ? 1U : 0U;
                    deadlocks += result.status == Status::deadlock ? 1U : 0U;
                    errors += result.status == Status::worker_error || result.status == Status::timeout || result.status == Status::replay_mismatch ? 1U : 0U;
                    const auto relative = std::filesystem::path("trial-"+std::to_string(trial)) / scenario.name /
                        ("schedule-"+std::to_string(run+1)+".json");
                    save_trace(output/relative,{std::string(scenario.program),config.max_steps,config.timeout,result});
                    traces.push_back(relative.generic_string());
                }
                records.push_back({{"trial",trial},{"scenario",scenario.name},{"explore_ns",duration},
                    {"schedules",search.runs.size()},{"assertions",assertions},{"deadlocks",deadlocks},{"errors",errors},
                    {"frontier_exhausted",search.exhausted},{"step_bound_reached",search.step_bound_reached},
                    {"schedule_bound_reached",search.schedule_bound_reached},{"traces",traces}});
            }
        }
        const auto& corrected = examples::scenario_named("lost-update-rmw");
        const auto controlled = [&] {
            const auto result = corrected.run(config);
            if (result.status != Status::completed) throw std::runtime_error("controlled counter baseline needs at least 4 steps and a sufficient timeout");
        };
        Json native_times = Json::array(), controlled_times = Json::array();
        if (config.max_steps >= 4) {
            for (int warmup = 0; warmup < 5; ++warmup) { native_counter(); controlled(); }
            for (std::size_t sample = 0; sample < samples; ++sample) {
                const auto timed_native = [&] { const auto start = Clock::now(); native_counter(); native_times.push_back(elapsed(start)); };
                const auto timed_controlled = [&] { const auto start = Clock::now(); controlled(); controlled_times.push_back(elapsed(start)); };
                // Alternate order to reduce fixed-order warm-cache bias.
                if (sample % 2 == 0) { timed_native(); timed_controlled(); }
                else { timed_controlled(); timed_native(); }
            }
        }
        std::cout << Json{{"schema_version",1},{"trials",trials},{"max_schedules",schedules},
            {"max_steps",config.max_steps},{"timeout_ms",config.timeout.count()},
            {"warmup_searches_per_scenario",1},{"records",records},
            {"overhead",{{"scenario","lost-update-rmw"},{"warmup_runs_per_mode",5},
                {"native_ns",native_times},{"controlled_ns",controlled_times}}}}.dump(2) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n'; return 2;
    }
}
