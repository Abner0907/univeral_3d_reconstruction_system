#include "u3d/ThreadPool.h"

#include <algorithm>

namespace u3d {

ThreadPool::ThreadPool(int threads)
{
    if (threads <= 0)
        threads = int(std::max(1u, std::thread::hardware_concurrency()));
    for (int i = 1; i < threads; ++i)
        m_workers.emplace_back([this, i] { workerLoop(i); });
}

ThreadPool::~ThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop.store(true);
    }
    m_wake.notify_all();
    for (std::thread &t : m_workers)
        t.join();
}

void ThreadPool::runChunks(int threadIndex)
{
    for (;;) {
        const int begin = m_next.fetch_add(m_grain);
        if (begin >= m_n)
            return;
        (*m_fn)(begin, std::min(m_n, begin + m_grain), threadIndex);
    }
}

namespace {
// Busy-wait this many polls before sleeping: training dispatches many short jobs per
// iteration, and waking a sleeping core costs far more than the jobs themselves.
constexpr int kSpinPolls = 20000;
} // namespace

void ThreadPool::workerLoop(int index)
{
    unsigned seen = 0;
    for (;;) {
        bool ready = false;
        for (int spin = 0; spin < kSpinPolls; ++spin) {
            if (m_stop.load(std::memory_order_acquire) || m_generation.load(std::memory_order_acquire) != seen) {
                ready = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!ready) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stop.load() || m_generation.load() != seen; });
        }
        if (m_stop.load())
            return;
        seen = m_generation.load(std::memory_order_acquire);
        runChunks(index);
        if (m_active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_done.notify_one();
        }
    }
}

void ThreadPool::parallelFor(int n, const std::function<void(int, int, int)> &fn, int grain)
{
    if (n <= 0)
        return;
    grain = std::max(1, grain);
    if (m_workers.empty() || n <= grain) {
        for (int b = 0; b < n; b += grain)
            fn(b, std::min(n, b + grain), 0);
        return;
    }
    m_fn = &fn;
    m_n = n;
    m_grain = grain;
    m_next.store(0);
    m_active.store(int(m_workers.size()));
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    m_wake.notify_all();
    runChunks(0);
    for (int spin = 0; spin < kSpinPolls && m_active.load(std::memory_order_acquire) != 0; ++spin)
        std::this_thread::yield();
    if (m_active.load(std::memory_order_acquire) != 0) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [&] { return m_active.load() == 0; });
    }
    m_fn = nullptr;
}

} // namespace u3d
