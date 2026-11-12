#pragma once

#include "monitor_policy.h"
#include <functional>

namespace Monitor
{
    struct Sources
    {
        std::function<Observation::InternetResult()> current;
        std::function<Observation::Campus()> campus;
        std::function<Observation::InternetResult(const Observation::Campus&)> direct;
        std::function<long long()> now;
        Operation::Cancelled cancelled;
    };

    struct Cycle
    {
        Observation::Campus campus;
        Observation::InternetResult current;
        Observation::InternetResult direct;
        Action action = Action::Waiting;
        bool cancelled = false;
    };

    // The resident loop and fixtures share this observation/decision sequence.
    inline Cycle Observe(bool enabled, bool manual, bool refreshCampus,
        const Observation::Campus& cached, const Auth::RetryPolicy& retry, const Sources& sources)
    {
        Cycle cycle;
        cycle.campus = cached;
        const auto stale = [&] { return sources.cancelled && sources.cancelled(); };
        if (!enabled && !manual) { cycle.action = Action::Paused; return cycle; }
        if (stale()) { cycle.cancelled = true; return cycle; }
        cycle.current = sources.current();
        if (stale() || cycle.current.cancelled) { cycle.cancelled = true; return cycle; }
        const bool online = cycle.current.state == Observation::Internet::Online;
        if (manual || refreshCampus || !online) cycle.campus = sources.campus();
        if (stale()) { cycle.cancelled = true; return cycle; }
        if (!online && cycle.campus.Confirmed() && !manual) cycle.direct = sources.direct(cycle.campus);
        if (stale() || cycle.direct.cancelled) { cycle.cancelled = true; return cycle; }
        cycle.action = Decide(enabled, manual, cycle.campus, cycle.current, cycle.direct, retry, sources.now());
        return cycle;
    }
}
