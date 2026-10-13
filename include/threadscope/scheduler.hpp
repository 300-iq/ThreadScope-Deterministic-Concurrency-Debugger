#pragma once

#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace threadscope {

using ThreadId = std::uint32_t;
enum class ThreadState { running, runnable, blocked, terminated };
enum class Status { completed, assertion_failed, deadlock, step_limit, timeout,
                    replay_mismatch, worker_error };
enum class ResourceKind { atomic_int, mutex };
enum class EventType { shared_state, lock_acquire, lock_release, assertion, join, yield };
enum class WaitKind { mutex, join };
std::string to_string(ThreadState state);
std::string to_string(Status status);
std::string to_string(ResourceKind kind);
std::string to_string(EventType type);
std::string to_string(WaitKind kind);
EventType event_type(const std::string& operation);

struct WaitDependency {
    ThreadId thread{};
    ThreadId waits_for{}; // Mutex owner or unfinished join target.
    WaitKind kind{};
    std::string resource; // Mutex name; empty for logical joins.
    auto operator<=>(const WaitDependency&) const = default;
};
using WaitForGraph = std::vector<WaitDependency>; // Sorted, unique directed edges.
bool has_wait_cycle(const WaitForGraph& graph);

struct ResourceSnapshot {
    std::string name;
    ResourceKind kind{};
    std::optional<int> value; // A mutex's value is its owner ID, or null.
    bool operator==(const ResourceSnapshot&) const = default;
};
using SharedState = std::vector<ResourceSnapshot>; // Sorted by resource name.

struct Observation {
    std::optional<int> before;
    std::optional<int> after;
    std::optional<bool> assertion_passed;
    std::optional<std::string> error;
    std::optional<int> expected = std::nullopt;
    bool operator==(const Observation&) const = default;
};

struct Event {
    std::size_t step{};
    ThreadId thread{};
    std::vector<ThreadId> runnable;
    std::vector<ThreadState> states; // Index is the stable logical thread ID.
    std::string operation;
    std::string resource;
    std::string label;
    Observation observation;
    std::optional<SharedState> shared_state_before = std::nullopt;
    std::optional<SharedState> shared_state_after = std::nullopt;
    // Dependencies at the same quiescent point as states, before selection.
    std::optional<WaitForGraph> wait_for = std::nullopt;
    EventType type() const { return event_type(operation); }
    bool operator==(const Event&) const = default;
};

struct RunResult {
    Status status{Status::completed};
    std::string message;
    std::vector<Event> events;
    // States at the stop decision, before cancellation/join cleanup.
    std::vector<ThreadState> final_states;
    std::optional<SharedState> shared_state = std::nullopt;
    std::optional<WaitForGraph> wait_for = std::nullopt;
    bool operator==(const RunResult&) const = default;
};

struct Config {
    std::size_t max_steps{1000};
    std::chrono::milliseconds timeout{1000};
    // A forced prefix; the default suffix always selects the lowest runnable ID.
    std::vector<ThreadId> schedule;
    bool exact_schedule{false};
    // Strict replay also compares every event, including state observations.
    std::optional<std::vector<Event>> expected_events;
    // What-if runs validate unchanged events and the branch checkpoint before
    // granting the new choice. The suffix remains lowest-runnable-ID first.
    std::optional<std::vector<Event>> expected_prefix;
    std::optional<Event> branch_checkpoint;
};

class AtomicInt {
public:
    explicit AtomicInt(std::string name, int initial = 0)
        : name_(std::move(name)), value_(initial) {}
    // Inspect outside a run. Tasks use Context::load to keep reads scheduled.
    int snapshot() const noexcept { return value_.load(std::memory_order_relaxed); }
private:
    friend class Context;
    friend class Scheduler;
    std::string name_;
    std::atomic<int> value_;
};

// A logical mutex: no worker blocks inside std::mutex::lock. The scheduler
// enables its lock operation only when ownership is available.
class ControlledMutex {
public:
    explicit ControlledMutex(std::string name) : name_(std::move(name)) {}
private:
    friend class Context;
    friend class Scheduler;
    static constexpr ThreadId unowned = std::numeric_limits<ThreadId>::max();
    std::string name_;
    std::atomic<ThreadId> owner_{unowned};
};

class Scheduler;
class Context {
public:
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ThreadId id() const noexcept { return id_; }
    int load(AtomicInt& value, std::string label = {});
    void store(AtomicInt& value, int desired, std::string label = {});
    int fetch_add(AtomicInt& value, int increment, std::string label = {});
    void assert_equal(AtomicInt& value, int expected, std::string label);
    void yield(std::string label = {});
    void lock(ControlledMutex& mutex);
    void unlock(ControlledMutex& mutex);
    void join_all(std::vector<ThreadId> threads);
private:
    friend class Scheduler;
    Context(Scheduler& scheduler, ThreadId id) : scheduler_(scheduler), id_(id) {}
    Scheduler& scheduler_;
    ThreadId id_;
};

// Single use. Tasks and resources must outlive run(). Worker-local computation
// must be bounded and race-free, and must return to the framework cooperatively.
class Scheduler {
public:
    using Task = std::function<void(Context&)>;
    Scheduler();
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    ThreadId add_thread(Task task);
    // Register all relevant resources before run(). Names must be unique.
    // Untracked callers retain the original operation-observation trace format.
    void track(AtomicInt& value);
    void track(ControlledMutex& mutex);
    RunResult run(const Config& config = {});
private:
    friend class Context;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Observation perform(ThreadId id, std::string operation, std::string resource,
                        std::string label, std::function<Observation()> action,
                        std::function<bool()> ready = [] { return true; },
                        ControlledMutex* waiting_mutex = nullptr,
                        std::vector<ThreadId> join_targets = {});
    bool terminated(ThreadId id) const;
    std::size_t thread_count() const;
};

} // namespace threadscope
