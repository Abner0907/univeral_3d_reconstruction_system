#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace u3d {

// Fixed-size worker pool with a blocking parallel-for. The calling thread takes part as
// thread index 0, so `size()` threads work in total.
class ThreadPool
{
public:
    // threads <= 0: use all hardware threads.
    explicit ThreadPool(int threads = 0);
    ~ThreadPool();
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    int size() const { return int(m_workers.size()) + 1; }

    // Calls fn(begin, end, threadIndex) on chunks of [0, n) of at most `grain` items.
    void parallelFor(int n, const std::function<void(int, int, int)> &fn, int grain = 1);

private:
    void workerLoop(int index);
    void runChunks(int threadIndex);

    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done;
    const std::function<void(int, int, int)> *m_fn = nullptr;
    int m_n = 0;
    int m_grain = 1;
    std::atomic<int> m_next{0};
    std::atomic<int> m_active{0};
    std::atomic<unsigned> m_generation{0};
    std::atomic<bool> m_stop{false};
};

} // namespace u3d
