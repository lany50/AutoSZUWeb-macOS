#include "monitor_cycle.h"
#include "monitor_schedule.h"
#include "auth_attempt.h"
#include "wake_signal.h"
#include <cstdio>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
    int failures = 0;
    void Check(bool condition, const char* message)
    {
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
        if (!condition) ++failures;
    }
}

int main()
{
    Observation::Campus campus;
    campus.portal = Observation::Portal::Teaching;
    Observation::InternetResult current, direct;
    Auth::RetryPolicy retry;
    long long now = 100;
    int portalCalls = 0, currentCalls = 0, directCalls = 0;
    bool cancelled = false, cancelDuringPortal = false;
    Monitor::Sources sources{
        [&] { ++currentCalls; return current; },
        [&] { ++portalCalls; if (cancelDuringPortal) cancelled = true; return campus; },
        [&](const Observation::Campus&) { ++directCalls; return direct; },
        [&] { return now; }, [&] { return cancelled; }
    };
    const auto cycle = [&](bool enabled = true, bool manual = false, bool refresh = false) {
        return Monitor::Observe(enabled, manual, refresh, campus, retry, sources);
    };
    Check(cycle(false).action == Monitor::Action::Paused && currentCalls == 0,
        "disabled monitoring performs no network operations");
    direct.dnsFailures = 3;
    Check(cycle().action == Monitor::Action::Uncertain,
        "DNS failure with reachable campus never requests authentication");
    direct.dnsFailures = 0;
    Check(cycle().action == Monitor::Action::Uncertain,
        "all external probes timing out remains uncertain");
    campus.session = Observation::Session::Online;
    Check(cycle().action == Monitor::Action::Uncertain,
        "online portal session with broken upstream remains uncertain");
    campus.session = Observation::Session::Offline;
    Check(cycle().action == Monitor::Action::Authenticate,
        "explicit offline portal session allows automatic authentication");
    campus.portal = Observation::Portal::Dormitory;
    campus.session = Observation::Session::Unknown;
    direct.state = Observation::Internet::Captive;
    Check(cycle().action == Monitor::Action::Uncertain,
        "unverified redirect cannot authorize credential submission");
    direct.campusIntercept = true;
    Check(cycle().action == Monitor::Action::Authenticate,
        "verified physical campus redirect permits authentication");
    direct.campusIntercept = false;
    direct.state = Observation::Internet::Online;
    Check(cycle().action == Monitor::Action::PathProblem,
        "working direct path avoids an unnecessary login");
    current.state = Observation::Internet::Online;
    portalCalls = 0; directCalls = 0;
    Check(cycle().action == Monitor::Action::Healthy && portalCalls == 0 && directCalls == 0,
        "stable online cycle skips portal and direct probes");
    Check(cycle(true, false, true).action == Monitor::Action::Healthy && portalCalls == 1,
        "network event refreshes campus evidence once");
    Check(cycle(false, true).action == Monitor::Action::Authenticate,
        "explicit manual request remains available with automatic mode disabled");
    current = {}; direct = {}; campus.session = Observation::Session::Offline;
    retry.Fail(Auth::Failure::Transport, now);
    Check(cycle().action == Monitor::Action::Cooling, "injected clock observes retry cooldown");
    current.state = Observation::Internet::Online;
    Check(cycle().action == Monitor::Action::Healthy,
        "external recovery is observed during an authentication cooldown");
    current = {};
    now += 5;
    Check(cycle().action == Monitor::Action::Authenticate, "injected clock releases cooldown at deadline");
    retry.Reset(); cancelled = true;
    currentCalls = 0;
    Check(cycle().cancelled && currentCalls == 0, "settings event cancels a cycle before probing");
    cancelled = false; cancelDuringPortal = true; directCalls = 0;
    Check(cycle().cancelled && directCalls == 0, "network event during portal read discards cycle results");
    cancelDuringPortal = false; cancelled = false;
    Monitor::UncertainSchedule uncertainty;
    Check(uncertainty.Next() == 2 && uncertainty.Next() == 5 && uncertainty.Next() == 30 && uncertainty.Next() == 30,
        "uncertain failures follow two-second five-second then thirty-second checks");
    uncertainty.Reset();
    Check(uncertainty.Next() == 2, "new network restarts finite uncertainty rechecks");
    const auto generation = WakeSignal::Generation();
    WakeSignal::Notify(WakeSignal::Settings);
    WakeSignal::Take();
    Check(WakeSignal::Generation() != generation, "consuming wake events preserves cancellation generation");

    char directory[] = "/tmp/autoszu-auth-lock-XXXXXX";
    if (!mkdtemp(directory)) return 2;
    {
        Auth::Attempt first(directory);
        Check(first.Acquired() && Status::Get().authInProgress, "authentication lock owns status until verification ends");
        Status::RequestManualAuth();
        Check(!Status::ManualAuthPending(), "repeated clicks during authentication are merged");
        Auth::Attempt second(directory);
        Check(!second.Acquired(), "same-process concurrent authentication is rejected");
        const pid_t child = fork();
        if (child == 0)
        {
            Status::EndAuth();
            Auth::Attempt cli(directory);
            _exit(cli.Acquired() ? 1 : 0);
        }
        int status = -1;
        if (child > 0) waitpid(child, &status, 0);
        Check(child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "CLI process cannot authenticate while resident owns verification lock");
    }
    Check(!Status::Get().authInProgress, "authentication scope releases UI busy status");
    {
        Auth::Attempt next(directory);
        Check(next.Acquired(), "authentication lock is reusable after completion");
    }
    std::filesystem::remove_all(directory);
    return failures ? 1 : 0;
}
