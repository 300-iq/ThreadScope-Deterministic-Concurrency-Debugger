#include "lost_update.hpp"

namespace threadscope::examples {
RunResult lost_update(const Config& config, bool use_rmw) {
    AtomicInt counter("counter", 0);
    Scheduler scheduler;
    scheduler.track(counter);
    for (int i = 0; i < 2; ++i) {
        scheduler.add_thread([&](Context& thread) {
            if (use_rmw) {
                thread.fetch_add(counter, 1, "increment");
            } else {
                const int local = thread.load(counter, "read counter");
                thread.store(counter, local + 1, "write local + 1");
            }
        });
    }
    scheduler.add_thread([&](Context& checker) {
        checker.join_all({0, 1});
        checker.assert_equal(counter, 2, "counter == 2 after both increments");
    });
    return scheduler.run(config);
}
}
