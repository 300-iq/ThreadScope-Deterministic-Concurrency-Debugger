#pragma once
#include "threadscope/scheduler.hpp"

namespace threadscope::examples {
// Two increment workers and a checker that waits for both workers to terminate.
RunResult lost_update(const Config& config = {}, bool use_rmw = false);
}
