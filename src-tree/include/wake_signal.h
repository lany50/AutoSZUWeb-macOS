#pragma once
#include <chrono>
#include <cstdint>

namespace WakeSignal
{
    enum Reason : uint32_t { None = 0, Network = 1, Wake = 2, Settings = 4,
                              Manual = 8, Diagnostic = 16 };
    void Notify(uint32_t reasons);
    uint32_t Wait(std::chrono::milliseconds duration);
    uint32_t Take();
    // Changes invalidate in-flight network work even after another consumer takes the event.
    uint64_t Generation();
}
namespace NativeEvents { bool Start(); }
