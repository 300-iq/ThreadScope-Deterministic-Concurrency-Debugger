#include "threadscope/scheduler.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
// Fault injection is confined to this isolated test executable and one worker.
thread_local bool reject_allocations = false;
std::atomic<int> rejected{0};
}

void* operator new(std::size_t size) {
    if (reject_allocations) {
        rejected.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc{};
    }
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    struct DiagnosticException : std::exception {
        const char* what() const noexcept override {
            reject_allocations = true;
            return "This deliberately long exception diagnostic exceeds small string storage and requires heap allocation.";
        }
    };
    threadscope::Scheduler scheduler;
    scheduler.add_thread([](threadscope::Context&) { throw DiagnosticException{}; });
    scheduler.add_thread([](threadscope::Context& context) { context.yield(); });
    const auto result = scheduler.run();
    if (rejected.load(std::memory_order_relaxed) == 0 ||
        result.status != threadscope::Status::worker_error ||
        result.message != "worker error (diagnostic unavailable)" || !result.events.empty() ||
        result.final_states != (std::vector<threadscope::ThreadState>{
            threadscope::ThreadState::terminated, threadscope::ThreadState::runnable})) {
        std::cerr << "diagnostic allocation failure was not contained and reported\n";
        return 1;
    }
    return 0;
}
