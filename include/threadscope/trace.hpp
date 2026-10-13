#pragma once
#include "threadscope/scheduler.hpp"
#include <filesystem>

namespace threadscope {
struct Trace {
    std::string program;
    std::size_t max_steps{1000};
    std::chrono::milliseconds timeout{1000};
    RunResult result;
    bool operator==(const Trace&) const = default;
};
std::string encode_trace(const Trace& trace);
Trace decode_trace(const std::string& json);
void save_trace(const std::filesystem::path& path, const Trace& trace);
Trace load_trace(const std::filesystem::path& path);
Config replay_config(const Trace& trace);
// Validates recorded events and the terminal outcome. Missing optional shared
// snapshots in legacy v1 traces are not invented or compared.
RunResult replay_trace(const Trace& trace, const std::function<RunResult(const Config&)>& scenario);
// Call replay_trace first to verify the entire original history/outcome. This
// configuration additionally checks the common prefix in the fresh branch run.
Config branch_config(const Trace& trace, std::size_t step, ThreadId alternative);
}
