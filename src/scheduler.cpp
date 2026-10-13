#include "threadscope/scheduler.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace threadscope {
namespace {
struct Cancelled {}; // Private control flow; never escapes a worker wrapper.

std::string metadata_divergence(const Event& expected, const Event& actual) {
    if (expected.step != actual.step || expected.thread != actual.thread) return "decision";
    if (expected.runnable != actual.runnable) return "runnable set";
    if (expected.states != actual.states) return "thread states";
    if (expected.wait_for && expected.wait_for != actual.wait_for) return "wait-for graph";
    if (expected.operation != actual.operation) return "operation";
    if (expected.resource != actual.resource) return "resource";
    if (expected.label != actual.label) return "label";
    if (expected.shared_state_before && expected.shared_state_before != actual.shared_state_before)
        return "shared state before operation";
    return {};
}
}

std::string to_string(EventType type) {
    switch (type) {
    case EventType::shared_state: return "shared_state";
    case EventType::lock_acquire: return "lock_acquire";
    case EventType::lock_release: return "lock_release";
    case EventType::assertion: return "assertion";
    case EventType::join: return "join";
    case EventType::yield: return "yield";
    }
    throw std::logic_error("invalid event type");
}
EventType event_type(const std::string& operation) {
    if (operation == "load" || operation == "store" || operation == "fetch_add") return EventType::shared_state;
    if (operation == "lock") return EventType::lock_acquire;
    if (operation == "unlock") return EventType::lock_release;
    if (operation == "assert_equal") return EventType::assertion;
    if (operation == "join") return EventType::join;
    if (operation == "yield") return EventType::yield;
    throw std::invalid_argument("unknown operation kind");
}
std::string to_string(WaitKind kind) {
    switch (kind) {
    case WaitKind::mutex: return "mutex";
    case WaitKind::join: return "join";
    }
    throw std::logic_error("invalid wait kind");
}
bool has_wait_cycle(const WaitForGraph& graph) {
    std::array<std::vector<ThreadId>, 64> successors;
    std::array<unsigned char, 64> color{};
    for (const auto& edge : graph) {
        if (edge.thread >= 64 || edge.waits_for >= 64)
            throw std::invalid_argument("wait-for graph thread ID exceeds limit");
        successors[edge.thread].push_back(edge.waits_for);
    }
    const auto visit = [&](auto&& self, ThreadId thread) -> bool {
        if (color[thread] == 1) return true;
        if (color[thread] == 2) return false;
        color[thread] = 1;
        for (const auto target : successors[thread]) if (self(self, target)) return true;
        color[thread] = 2;
        return false;
    };
    for (ThreadId thread = 0; thread < 64; ++thread) if (visit(visit, thread)) return true;
    return false;
}

std::string to_string(ResourceKind kind) {
    switch (kind) {
    case ResourceKind::atomic_int: return "atomic_int";
    case ResourceKind::mutex: return "mutex";
    }
    throw std::logic_error("invalid resource kind");
}

std::string to_string(ThreadState state) {
    switch (state) {
    case ThreadState::running: return "running";
    case ThreadState::runnable: return "runnable";
    case ThreadState::blocked: return "blocked";
    case ThreadState::terminated: return "terminated";
    }
    throw std::logic_error("invalid thread state");
}
std::string to_string(Status status) {
    switch (status) {
    case Status::completed: return "completed";
    case Status::assertion_failed: return "assertion_failed";
    case Status::deadlock: return "deadlock";
    case Status::step_limit: return "step_limit";
    case Status::timeout: return "timeout";
    case Status::replay_mismatch: return "replay_mismatch";
    case Status::worker_error: return "worker_error";
    }
    throw std::logic_error("invalid status");
}

struct Scheduler::Impl {
    struct Slot {
        std::thread::id worker;
        Task task;
        ThreadState state{ThreadState::running};
        std::function<bool()> ready;
        ControlledMutex* waiting_mutex{nullptr};
        std::vector<ThreadId> join_targets;
        bool granted{false};
        std::optional<std::string> error = std::nullopt;
        bool error_occurred{false};
        std::string operation;
        std::string resource;
        std::string label;
    };
    std::mutex monitor;
    std::condition_variable changed;
    std::vector<Slot> slots;
    bool used{false};
    bool stopping{false};
    std::optional<Status> failure;
    std::string failure_message;
    RunResult result;
    Event granted_event;
    std::optional<std::vector<Event>> expected;
    std::map<std::string, std::function<ResourceSnapshot()>> resources;

    std::optional<SharedState> snapshot() const {
        if (resources.empty()) return std::nullopt;
        SharedState state;
        for (const auto& [name, observe] : resources) {
            (void)name;
            state.push_back(observe());
        }
        return state;
    }

    WaitForGraph wait_graph() const {
        WaitForGraph graph;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const auto& slot = slots[i];
            if (slot.state != ThreadState::blocked) continue;
            const auto id = static_cast<ThreadId>(i);
            if (slot.waiting_mutex) {
                const auto owner = slot.waiting_mutex->owner_.load(std::memory_order_relaxed);
                if (owner != ControlledMutex::unowned)
                    graph.push_back({id, owner, WaitKind::mutex, slot.waiting_mutex->name_});
            } else {
                for (const auto target : slot.join_targets)
                    if (slots[target].state != ThreadState::terminated)
                        graph.push_back({id, target, WaitKind::join, ""});
            }
        }
        std::sort(graph.begin(), graph.end());
        graph.erase(std::unique(graph.begin(), graph.end()), graph.end());
        return graph;
    }

    bool quiescent() const {
        return std::all_of(slots.begin(), slots.end(), [](const auto& slot) {
            return slot.state != ThreadState::running;
        });
    }
    std::vector<ThreadId> refresh() {
        std::vector<ThreadId> runnable;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i];
            if (slot.state == ThreadState::runnable || slot.state == ThreadState::blocked) {
                slot.state = slot.ready() ? ThreadState::runnable : ThreadState::blocked;
                if (slot.state == ThreadState::runnable) runnable.push_back(static_cast<ThreadId>(i));
            }
        }
        return runnable;
    }
    std::vector<ThreadState> states() const {
        std::vector<ThreadState> result_states;
        for (const auto& slot : slots) result_states.push_back(slot.state);
        return result_states;
    }
};

Scheduler::Scheduler() : impl_(std::make_unique<Impl>()) {}
Scheduler::~Scheduler() = default;

ThreadId Scheduler::add_thread(Task task) {
    if (impl_->used) throw std::logic_error("cannot register a task after run");
    if (!task) throw std::invalid_argument("task must not be empty");
    if (impl_->slots.size() >= 64) throw std::invalid_argument("at most 64 logical threads supported");
    const auto id = static_cast<ThreadId>(impl_->slots.size());
    Impl::Slot slot;
    slot.task = std::move(task);
    impl_->slots.push_back(std::move(slot));
    return id;
}

void Scheduler::track(AtomicInt& value) {
    if (impl_->used) throw std::logic_error("register resources before run");
    if (value.name_.empty() || impl_->resources.contains(value.name_))
        throw std::invalid_argument("resource names must be nonempty and unique");
    impl_->resources.emplace(value.name_, [&value] {
        return ResourceSnapshot{value.name_, ResourceKind::atomic_int, value.snapshot()};
    });
}
void Scheduler::track(ControlledMutex& mutex) {
    if (impl_->used) throw std::logic_error("register resources before run");
    if (mutex.name_.empty() || impl_->resources.contains(mutex.name_))
        throw std::invalid_argument("resource names must be nonempty and unique");
    impl_->resources.emplace(mutex.name_, [&mutex] {
        const auto owner = mutex.owner_.load(std::memory_order_relaxed);
        const auto value = owner == ControlledMutex::unowned ? std::optional<int>{} : std::optional<int>{static_cast<int>(owner)};
        return ResourceSnapshot{mutex.name_, ResourceKind::mutex, value};
    });
}

RunResult Scheduler::run(const Config& config) {
    auto& engine = *impl_;
    if (engine.used) throw std::logic_error("scheduler is single use");
    if (config.max_steps == 0 || config.max_steps > 1'000'000 ||
        config.timeout.count() <= 0 || config.timeout.count() > 60'000)
        throw std::invalid_argument("limits require 1..1000000 steps and 1..60000 ms");
    if (config.expected_events &&
        (!config.exact_schedule || config.expected_events->size() != config.schedule.size()))
        throw std::invalid_argument("event replay requires an exact schedule of matching length");
    if (config.expected_prefix || config.branch_checkpoint) {
        if (!config.expected_prefix || !config.branch_checkpoint || config.exact_schedule || config.expected_events ||
            config.expected_prefix->size() != config.branch_checkpoint->step ||
            config.schedule.size() != config.branch_checkpoint->step + 1)
            throw std::invalid_argument("branch requires a matching expected prefix and one new scheduling choice");
    }
    engine.used = true;
    engine.expected = config.expected_events ? config.expected_events : config.expected_prefix;
    const auto deadline = std::chrono::steady_clock::now() + config.timeout;
    std::vector<std::thread> workers;
    workers.reserve(engine.slots.size());

    // Every exit path cancels waiters and joins all successfully launched threads.
    auto cleanup = [&] {
        {
            std::lock_guard guard(engine.monitor);
            engine.stopping = true;
        }
        engine.changed.notify_all();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    };
    try {
        for (std::size_t i = 0; i < engine.slots.size(); ++i) {
            workers.emplace_back([&, id = static_cast<ThreadId>(i)] {
                {
                    std::lock_guard guard(engine.monitor);
                    engine.slots[id].worker = std::this_thread::get_id();
                }
                Context context(*this, id);
                std::optional<std::string> error_message;
                bool error_occurred = false;
                try {
                    engine.slots[id].task(context);
                } catch (const Cancelled&) {
                } catch (const std::exception& error) {
                    error_occurred = true;
                    // what() is user-overridable code. Never call it while
                    // holding the monitor needed for cancellation and progress.
                    // Copying its text may itself throw (e.g. bad_alloc). Keep
                    // an allocation-free failure flag so that cannot terminate
                    // the worker process or silently turn the error into success.
                    try { error_message = error.what(); } catch (...) {}
                } catch (...) {
                    error_occurred = true;
                    try { error_message = "worker threw a non-standard exception"; } catch (...) {}
                }
                {
                    std::lock_guard guard(engine.monitor);
                    if (!engine.stopping) {
                        engine.slots[id].error = std::move(error_message);
                        engine.slots[id].error_occurred = error_occurred;
                    }
                    // Publish the error and actual logical termination together.
                    engine.slots[id].state = ThreadState::terminated;
                }
                engine.changed.notify_all();
            });
        }

        std::unique_lock guard(engine.monitor);
        while (true) {
            // Scheduling is permitted only after all workers have reached a
            // request or returned. Arrival order is never a scheduling input.
            if (!engine.changed.wait_until(guard, deadline, [&] {
                    return engine.quiescent();
                })) {
                engine.result.status = Status::timeout;
                engine.result.message = "cooperative run deadline exceeded";
                break;
            }
            // A predicate can become true while the driver is descheduled past
            // its deadline. Do not report completion/deadlock in that case.
            if (std::chrono::steady_clock::now() >= deadline) {
                engine.result.status = Status::timeout;
                engine.result.message = "cooperative run deadline exceeded";
                break;
            }
            const auto runnable = engine.refresh();
            if (!engine.failure) {
                // Startup/local exceptions may arrive concurrently. Select a
                // stable ID after the barrier, rather than the first arrival.
                for (const auto& slot : engine.slots) {
                    if (slot.error_occurred) {
                        engine.failure = Status::worker_error;
                        engine.failure_message = slot.error ? *slot.error : "worker error (diagnostic unavailable)";
                        break;
                    }
                }
            }
            if (engine.failure) {
                engine.result.status = *engine.failure;
                engine.result.message = engine.failure_message;
                if (engine.result.events.size() < config.schedule.size() &&
                    *engine.failure != Status::replay_mismatch &&
                    *engine.failure != Status::worker_error) {
                    engine.result.status = Status::replay_mismatch;
                    engine.result.message = "schedule contains unused choices after failure";
                }
                break;
            }
            const auto step = engine.result.events.size();
            const bool complete = std::all_of(engine.slots.begin(), engine.slots.end(), [](const auto& slot) {
                return slot.state == ThreadState::terminated;
            });
            if (complete) {
                if (step < config.schedule.size()) {
                    engine.result.status = Status::replay_mismatch;
                    engine.result.message = "schedule contains unused choices";
                }
                break;
            }
            if (runnable.empty()) {
                engine.result.status = Status::deadlock;
                engine.result.message = "all unfinished threads are blocked";
                break;
            }
            if (step >= config.max_steps) {
                engine.result.status = Status::step_limit;
                engine.result.message = "controlled operation limit reached";
                break;
            }
            if (config.exact_schedule && step >= config.schedule.size()) {
                engine.result.status = Status::replay_mismatch;
                engine.result.message = "schedule ended before program completion";
                break;
            }
            const auto selected = step < config.schedule.size() ? config.schedule[step] : runnable.front();
            if (std::find(runnable.begin(), runnable.end(), selected) == runnable.end()) {
                engine.result.status = Status::replay_mismatch;
                engine.result.message = "selected thread T" + std::to_string(selected) +
                    " is not runnable at step " + std::to_string(step) + "; runnable=[";
                for (std::size_t i = 0; i < runnable.size(); ++i) {
                    if (i) engine.result.message += ',';
                    engine.result.message += std::to_string(runnable[i]);
                }
                engine.result.message += ']';
                break;
            }
            const auto& request = engine.slots[selected];
            engine.granted_event = {step, selected, runnable, engine.states(),
                request.operation, request.resource, request.label, {}};
            engine.granted_event.shared_state_before = engine.snapshot();
            engine.granted_event.wait_for = engine.wait_graph();
            if (engine.expected && step < engine.expected->size()) {
                const auto field = metadata_divergence(engine.expected->at(step), engine.granted_event);
                if (!field.empty()) {
                    engine.result.status = Status::replay_mismatch;
                    engine.result.message = "divergence at step " + std::to_string(step) + ": " + field;
                    break; // Validate the request before allowing it to mutate state.
                }
            }
            if (config.branch_checkpoint && step == config.branch_checkpoint->step) {
                const auto& expected = *config.branch_checkpoint;
                const auto& actual = engine.granted_event;
                // Selected operation/observation intentionally changes. Check
                // only the complete pre-choice state, before granting a permit.
                if (expected.runnable != actual.runnable || expected.states != actual.states ||
                    (expected.shared_state_before && expected.shared_state_before != actual.shared_state_before) ||
                    (expected.wait_for && expected.wait_for != actual.wait_for)) {
                    engine.result.status = Status::replay_mismatch;
                    engine.result.message = "divergence at branch checkpoint " + std::to_string(step);
                    break;
                }
            }
            engine.slots[selected].granted = true;
            engine.slots[selected].state = ThreadState::running;
            engine.changed.notify_all();
        }
        if (((config.exact_schedule && engine.result.events.size() != config.schedule.size()) ||
             (engine.result.status == Status::deadlock && engine.result.events.size() < config.schedule.size())) &&
            engine.result.status != Status::replay_mismatch &&
            engine.result.status != Status::timeout && engine.result.status != Status::worker_error) {
            engine.result.status = Status::replay_mismatch;
            engine.result.message = "schedule contains unconsumed choices";
        }
        engine.result.final_states = engine.states();
        engine.result.shared_state = engine.snapshot();
        engine.result.wait_for = engine.wait_graph();
        engine.stopping = true;
        guard.unlock();
        cleanup();
    } catch (...) {
        cleanup();
        throw;
    }
    return engine.result;
}

Observation Scheduler::perform(ThreadId id, std::string operation, std::string resource,
                               std::string label, std::function<Observation()> action,
                               std::function<bool()> ready, ControlledMutex* waiting_mutex,
                               std::vector<ThreadId> join_targets) {
    auto& engine = *impl_;
    std::unique_lock guard(engine.monitor);
    auto& slot = engine.slots.at(id);
    if (slot.worker != std::this_thread::get_id())
        throw std::logic_error("instrumented operations must execute on their registered native worker thread");
    if (engine.stopping) throw Cancelled{};
    slot.operation = std::move(operation);
    slot.resource = std::move(resource);
    slot.label = std::move(label);
    slot.ready = std::move(ready);
    slot.waiting_mutex = waiting_mutex;
    slot.join_targets = std::move(join_targets);
    slot.state = ThreadState::runnable;
    engine.changed.notify_all();
    // A failure does not wake unselected workers into cancellation. Keep them
    // parked until the driver snapshots their requests at the stable barrier.
    engine.changed.wait(guard, [&] { return slot.granted || engine.stopping; });
    if (engine.stopping) throw Cancelled{};
    slot.granted = false;

    // This code runs on the selected real worker. Only bounded built-in actions
    // execute under the monitor; user task code always runs outside it.
    auto observation = action();
    auto event = engine.granted_event;
    event.observation = observation;
    event.shared_state_after = engine.snapshot();
    engine.result.events.push_back(event);
    const auto* expected = engine.expected && event.step < engine.expected->size() ? &engine.expected->at(event.step) : nullptr;
    if (expected && (event.observation != expected->observation ||
                     (expected->shared_state_after && expected->shared_state_after != event.shared_state_after))) {
        engine.failure = Status::replay_mismatch;
        engine.failure_message = "divergence at step " + std::to_string(event.step) +
            (event.observation != expected->observation ? ": operation observation" : ": shared state after operation");
    } else if (observation.error) {
        engine.failure = Status::worker_error;
        engine.failure_message = *observation.error;
    } else if (observation.assertion_passed == false) {
        engine.failure = Status::assertion_failed;
        engine.failure_message = event.label;
    }
    if (engine.failure) {
        engine.changed.notify_all();
        throw Cancelled{};
    }
    // Running until the worker submits its next request or actually returns.
    // The driver cannot make another decision during this local computation.
    slot.ready = {};
    slot.waiting_mutex = nullptr;
    slot.join_targets.clear();
    engine.changed.notify_all();
    return observation;
}

bool Scheduler::terminated(ThreadId id) const { return impl_->slots.at(id).state == ThreadState::terminated; }
std::size_t Scheduler::thread_count() const { return impl_->slots.size(); }

int Context::load(AtomicInt& value, std::string label) {
    const auto observation = scheduler_.perform(id_, "load", value.name_, std::move(label), [&] {
        const int observed = value.value_.load(std::memory_order_relaxed);
        return Observation{observed, observed, {}, {}};
    });
    return *observation.after;
}
void Context::store(AtomicInt& value, int desired, std::string label) {
    scheduler_.perform(id_, "store", value.name_, std::move(label), [&] {
        const int before = value.value_.load(std::memory_order_relaxed);
        value.value_.store(desired, std::memory_order_relaxed);
        return Observation{before, desired, {}, {}};
    });
}
int Context::fetch_add(AtomicInt& value, int increment, std::string label) {
    const auto observation = scheduler_.perform(id_, "fetch_add", value.name_, std::move(label), [&] {
        const int before = value.value_.fetch_add(increment, std::memory_order_relaxed);
        // Atomic signed arithmetic is defined to wrap; avoid ordinary signed
        // overflow when recording the result of the operation.
        return Observation{before, value.value_.load(std::memory_order_relaxed), {}, {}};
    });
    return *observation.before;
}
void Context::assert_equal(AtomicInt& value, int expected, std::string label) {
    scheduler_.perform(id_, "assert_equal", value.name_, std::move(label), [&] {
        const int observed = value.value_.load(std::memory_order_relaxed);
        return Observation{observed, observed, observed == expected, {}, expected};
    });
}
void Context::yield(std::string label) {
    scheduler_.perform(id_, "yield", "", std::move(label), [] { return Observation{}; });
}
void Context::lock(ControlledMutex& mutex) {
    scheduler_.perform(id_, "lock", mutex.name_, "", [&] {
        mutex.owner_.store(id_, std::memory_order_relaxed);
        return Observation{{}, static_cast<int>(id_), {}, {}};
    }, [&] { return mutex.owner_.load(std::memory_order_relaxed) == ControlledMutex::unowned; }, &mutex);
}
void Context::unlock(ControlledMutex& mutex) {
    scheduler_.perform(id_, "unlock", mutex.name_, "", [&] {
        if (mutex.owner_.load(std::memory_order_relaxed) != id_)
            return Observation{{}, {}, {}, "unlock by a thread that does not own " + mutex.name_};
        mutex.owner_.store(ControlledMutex::unowned, std::memory_order_relaxed);
        return Observation{static_cast<int>(id_), {}, {}, {}};
    });
}
void Context::join_all(std::vector<ThreadId> threads) {
    std::string resource;
    for (const auto thread : threads) {
        if (thread >= scheduler_.thread_count() || thread == id_)
            throw std::invalid_argument("join target must be another registered thread");
        if (!resource.empty()) resource += ',';
        resource += std::to_string(thread);
    }
    auto ready = [this, threads] {
        return std::all_of(threads.begin(), threads.end(), [&](ThreadId thread) {
            return scheduler_.terminated(thread);
        });
    };
    scheduler_.perform(id_, "join", std::move(resource), "", [] { return Observation{}; },
                       std::move(ready), nullptr, std::move(threads));
}

} // namespace threadscope
