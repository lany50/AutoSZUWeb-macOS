#pragma once
#include <algorithm>
namespace Monitor
{
    class AdaptiveSchedule
    {
        int streak = 0;
    public:
        void Reset() { streak = 0; }
        int Healthy(bool saving, int maximum)
        {
            maximum = std::max(30, std::min(120, maximum));
            const int delay = std::min(maximum, 10 << std::min(streak, 4));
            if (streak < 4) ++streak;
            return saving ? delay : 10;
        }
    };
}
