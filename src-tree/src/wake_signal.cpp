#include "wake_signal.h"
#include <condition_variable>
#include <mutex>
#include <atomic>
namespace WakeSignal
{
    namespace
    {
        std::mutex mutex;
        std::condition_variable cv;
        uint32_t pending = 0;
        std::atomic<uint64_t> generation{0};
    }
    void Notify(uint32_t reasons)
    {
        if (reasons & (Network | Wake | Settings)) ++generation;
        { std::lock_guard<std::mutex> lock(mutex); pending |= reasons; }
        cv.notify_all();
    }
    uint32_t Wait(std::chrono::milliseconds duration)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, duration, [] { return pending != 0; });
        const auto result = pending; pending = 0; return result;
    }
    uint32_t Take()
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto result = pending; pending = 0; return result;
    }
    uint64_t Generation() { return generation.load(); }
}
