#pragma once

#include <curl/curl.h>
#include "operation.h"
#include <string>
#include <vector>

namespace Observation
{
    struct Environment
    {
        std::string interfaceName;
        std::string localIp;
        std::string router;
        std::string defaultInterface;
        std::vector<std::string> dnsServers;
        bool tunnelDefault = false;
        bool systemProxyKnown = false;
        bool systemProxyConfigured = false;
        std::string Key() const
        {
            std::string key = interfaceName + "/" + localIp + "/" + router + "/" + defaultInterface;
            for (const auto& dns : dnsServers) key += "/" + dns;
            return key;
        }
    };
    Environment ReadEnvironment();

    struct DnsResult
    {
        std::string ip;
        std::string error;
        bool Ok() const { return !ip.empty(); }
    };
    DnsResult ResolveDirect(const Environment& env, const std::string& host,
                            int timeoutMs = 900, const Operation::Cancelled& cancelled = {});

    struct HttpResult
    {
        CURLcode error = CURLE_FAILED_INIT;
        long status = 0;
        std::string body;
        std::string location;
        int elapsedMs = -1;
        bool Ok() const { return error == CURLE_OK; }
    };
    // No account/cookie/header secrets. Redirects disabled, body bounded to 64 KiB.
    HttpResult Get(const std::string& url, const Environment* direct = nullptr,
                   const std::string& host = {}, const std::string& ip = {},
                   int timeoutMs = 2500, const Operation::Cancelled& cancelled = {});
    // Bind only this process's request; never alter routes or proxy settings.
    curl_slist* ConfigureDirect(CURL* curl, const Environment& env,
                               const std::string& host = {},
                               const std::string& ip = {});
    bool EnsureCurl();

    enum class Portal { None, Dormitory, Teaching };
    enum class Session { Unknown, Online, Offline };
    struct Campus
    {
        Portal portal = Portal::None;
        Session session = Session::Unknown;
        Environment env;
        std::string serverIp;
        std::string evidence;
        bool Confirmed() const { return portal != Portal::None; }
    };
    // A private IP alone is never enough to authorize authentication.
    Campus DetectCampus(const Environment& env, const Operation::Cancelled& cancelled = {});
    bool IsDormitoryResponse(const std::string& body);
    bool IsTeachingResponse(const std::string& body);
    const char* PortalName(Portal portal);

    enum class Internet { Online, Captive, Unknown };
    struct InternetResult
    {
        Internet state = Internet::Unknown;
        int latencyMs = -1;
        int dnsFailures = 0;
        int elapsedMs = 0;
        bool campusIntercept = false;
        bool cancelled = false;
        std::string summary;
    };
    InternetResult CheckInternet(const Environment* direct = nullptr, int timeoutMs = 3000,
        bool collectAll = false, const Campus* campus = nullptr,
        const Operation::Cancelled& cancelled = {});
    const char* InternetName(Internet state);
    std::string Diagnose();
#ifdef AUTOSZUWEB_NETWORK_TESTING
    namespace Testing
    {
        void SetProbeUrls(const std::vector<std::string>& urls);
        void AdvanceProbeClock(int seconds);
        void ResetProbeCache();
    }
#endif
}
