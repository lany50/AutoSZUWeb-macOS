#include "srun.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int failures = 0;
    void Check(bool passed, const char* description)
    {
        std::cout << (passed ? "PASS " : "FAIL ") << description << '\n';
        if (!passed) ++failures;
    }
    curl_socket_t ForbidSocket(void*, curlsocktype, struct curl_sockaddr*)
    { std::abort(); }
}

namespace Observation
{
    curl_slist* ConfigureDirect(CURL* curl, const Environment&, const std::string&, const std::string&)
    {
        auto* addresses = curl_slist_append(nullptr, "net.szu.edu.cn:443:127.0.0.1");
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_RESOLVE, addresses);
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, ForbidSocket);
        return addresses;
    }
}

int main()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    Observation::Campus campus;
    campus.portal = Observation::Portal::Teaching;
    campus.env.interfaceName = "lo0";
    campus.env.localIp = "127.0.0.1";
    campus.serverIp = "127.0.0.1";
    const std::string account = "TEST-ACCOUNT-NEVER-SEND";
    const std::string password = "synthetic-password-never-send";
    for (int cancelAt = 0; cancelAt <= 3; ++cancelAt)
    {
        bool cancelled = false;
        std::vector<std::string> stages;
        SrunTesting::SetHttpTransport([&](const std::string& url, std::string& body) {
            std::string stage;
            if (url == "https://net.szu.edu.cn/")
            {
                stage = "homepage";
                body = "<html>ac_id=1</html>";
            }
            else if (url.find("/cgi-bin/get_challenge?") != std::string::npos)
            {
                stage = "challenge";
                body = R"(_({"error":"ok","challenge":"0123456789abcdef0123456789abcdef","client_ip":"192.0.2.5"}))";
            }
            else if (url.find("/cgi-bin/srun_portal?") != std::string::npos)
            {
                stage = "login";
                body = R"(_({"error":"ok","res":"ok"}))";
            }
            else std::abort();
            stages.push_back(stage);
            if (cancelAt > 0 && static_cast<int>(stages.size()) == cancelAt) cancelled = true;
            return CURLE_OK;
        });
        std::string message;
        Auth::Failure failure = Auth::Failure::None;
        const bool accepted = SrunLogin(account, password, message, failure, campus,
            [&] { return cancelled; });
        if (cancelAt == 0)
            Check(accepted && failure == Auth::Failure::None && stages.size() == 3,
                "synthetic SRun success exercises homepage, challenge, and login without any network");
        if (cancelAt == 1)
            Check(!accepted && failure == Auth::Failure::Cancelled && stages.size() == 1,
                "cancellation during homepage prevents the account-bearing challenge request");
        if (cancelAt == 2)
            Check(!accepted && failure == Auth::Failure::Cancelled && stages.size() == 2,
                "cancellation after challenge prevents the login submission");
        if (cancelAt == 3)
            Check(!accepted && failure == Auth::Failure::Cancelled && stages.size() == 3,
                "cancellation after a successful login response discards the obsolete success");
    }
    SrunTesting::SetHttpTransport({});
    curl_global_cleanup();
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
