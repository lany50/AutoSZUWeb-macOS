#include "web.h"
#include "srun.h"
#include "status.h"
#include "app_ui.h"
#include <iostream>
#include <cstdlib>

namespace
{
    Observation::Environment environment;
    Observation::Campus nextObservation;
    int authCalls = 0, observationCalls = 0, logCalls = 0, failures = 0;
    std::string logged;
    bool changeDuringDetection = false, changeDuringAuth = false;
    const std::string account = "TEST-ACCOUNT-DO-NOT-SEND";
    const std::string password = "TEST-PASSWORD-DO-NOT-SEND";
    void Check(bool ok, const char* text)
    { std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; if (!ok) ++failures; }
}
namespace Observation
{
    bool EnsureCurl() { return true; }
    Environment ReadEnvironment() { return environment; }
    Campus DetectCampus(const Environment&, const Operation::Cancelled&)
    {
        ++observationCalls;
        if (changeDuringDetection) environment.localIp = "192.0.2.99";
        return nextObservation;
    }
    // Any accidental ePortal request in these isolated tests aborts before it can send.
    curl_slist* ConfigureDirect(CURL*, const Environment&, const std::string&, const std::string&)
    { std::abort(); }
}
bool IsFirstBoot() { return false; }
namespace AppUI
{
    bool ShouldShowAuthenticationResult() { return false; }
    Button ShowMessage(const std::string&,const std::string&,bool) { return Button::Ok; }
}
void WriteAuthLog(const std::string&, bool, const std::string&, const std::string& text)
{ ++logCalls; logged = text; }
bool SrunLogin(const std::string& user, const std::string& pass, std::string& message,
               Auth::Failure& failure, const Observation::Campus&, const Operation::Cancelled&)
{
    ++authCalls;
    if (changeDuringAuth) environment.localIp = "192.0.2.99";
    message = "invalid credentials " + user + " " + pass;
    failure = Auth::Failure::Credentials;
    return false;
}
int main()
{
    environment.interfaceName = "en-test"; environment.localIp = "192.0.2.5"; environment.router = "192.0.2.1";
    Observation::Campus context; context.env = environment;
    auto result = Authenticate(account,password,context);
    Check(result.failure == Auth::Failure::NotCampus && authCalls == 0 && observationCalls == 0 && logCalls == 0,
        "unconfirmed network cannot transmit credentials or record an auth attempt");
    context.portal = Observation::Portal::Teaching;
    context.env.localIp = "192.0.2.6";
    result = Authenticate(account,password,context);
    Check(result.failure == Auth::Failure::NotCampus && authCalls == 0,
        "Wi-Fi change between detection and login prevents credential transmission");
    context.env = environment;
    nextObservation.env = environment;
    result = Authenticate(account,password,context);
    Check(result.failure == Auth::Failure::NotCampus && observationCalls == 1 && authCalls == 0,
        "stale campus evidence must be refreshed before login");
    nextObservation.portal = Observation::Portal::Teaching;
    result = Authenticate(account,password,context);
    Check(authCalls == 1 && result.failure == Auth::Failure::Credentials,
        "confirmed portal preserves typed authentication rejection");
    Check(logged.find(account) == std::string::npos && logged.find(password) == std::string::npos &&
        result.message.find(account) == std::string::npos && result.message.find(password) == std::string::npos,
        "server-echoed credentials are removed from logs and UI");
    Check(Status::Get().authCount == 1, "only the actual authentication invocation is counted");
    nextObservation.session = Observation::Session::Online;
    result = Authenticate(account,password,context,{},false);
    Check(result.alreadyOnline && !result.ok && authCalls == 1 && logCalls == 1,
        "session restored during portal refresh skips automatic credential submission");
    nextObservation.session = Observation::Session::Unknown;
    result = Authenticate(account,password,context,[] { return true; });
    Check(result.failure == Auth::Failure::Cancelled && authCalls == 1,
        "cancelled configuration prevents authentication before portal detection");
    changeDuringDetection = true;
    result = Authenticate(account,password,context);
    Check(result.failure == Auth::Failure::Cancelled && authCalls == 1 && logCalls == 1,
        "network change during portal detection prevents credential submission");
    changeDuringDetection = false; environment = context.env;
    changeDuringAuth = true;
    result = Authenticate(account,password,context);
    Check(result.failure == Auth::Failure::Cancelled && logCalls == 1 && Status::Get().authCount == 1,
        "response from an obsolete network cannot publish authentication state");
    return failures ? 1 : 0;
}
