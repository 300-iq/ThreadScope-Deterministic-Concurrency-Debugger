#include <atomic>
#include <cstdio>
#include <thread>
int main() {
    std::puts("sanitizer smoke: main reached");
    std::fflush(stdout);
    std::atomic<int> value{0};
    std::thread worker([&] { value.fetch_add(1); });
    worker.join();
    return value.load() == 1 ? 0 : 1;
}
