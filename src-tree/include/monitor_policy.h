#pragma once
#include "auth_policy.h"
#include "network_observation.h"

namespace Monitor
{
    enum class Action { Paused, Healthy, Waiting, PathProblem, NeedUser, Cooling, Uncertain, Authenticate };
    inline Action Decide(bool enabled, bool manual, const Observation::Campus& campus,
                         const Observation::InternetResult& current,
                         const Observation::InternetResult& direct,
                         const Auth::RetryPolicy& retry, long long now)
    {
        if (!enabled && !manual) return Action::Paused;
        if (current.state == Observation::Internet::Online && !manual) return Action::Healthy;
        if (!campus.Confirmed()) return Action::Waiting;
        if (direct.state == Observation::Internet::Online && !manual) return Action::PathProblem;
        if (!manual && retry.blocked) return Action::NeedUser;
        if (!manual && !retry.CanAttempt(now)) return Action::Cooling;
        if (manual || campus.session == Observation::Session::Offline) return Action::Authenticate;
        if (campus.session != Observation::Session::Online && direct.campusIntercept)
            return Action::Authenticate;
        return Action::Uncertain;
    }

    class UncertainSchedule
    {
        unsigned checks = 0;
    public:
        void Reset() { checks = 0; }
        int Next()
        {
            if (checks == 0) { ++checks; return 2; }
            if (checks == 1) { ++checks; return 5; }
            return 30;
        }
    };
}
