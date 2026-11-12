#include "network_observation.h"
#include "status.h"
#include "auth_policy.h"
#include "app_version.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <future>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

namespace Observation
{
    namespace
    {
        constexpr size_t kMaxBody = 65536;
        std::mutex probeMutex;
        std::vector<std::string> probeUrls = {
            "http://connect.rom.miui.com/generate_204",
            "http://wifi.vivo.com.cn/generate_204",
            "http://connectivitycheck.platform.hicloud.com/generate_204"};
        struct ProbeCache
        {
            std::string network;
            int preferred = -1;
            long long refreshAfter = 0;
        };
        std::array<ProbeCache, 2> probeCaches;
#ifdef AUTOSZUWEB_NETWORK_TESTING
        long long probeClockOffset = 0;
#endif
        long long Milliseconds()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        bool IsCancelled(const Operation::Cancelled& cancelled)
        { return cancelled && cancelled(); }
        int Progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
        { return IsCancelled(*static_cast<const Operation::Cancelled*>(context)) ? 1 : 0; }
        long long ProbeClock()
        {
#ifdef AUTOSZUWEB_NETWORK_TESTING
            return Milliseconds() + probeClockOffset;
#else
            return Milliseconds();
#endif
        }
        size_t WriteBody(char* ptr, size_t size, size_t count, void* data)
        {
            auto& body = *static_cast<std::string*>(data);
            if (size && count > kMaxBody / size) return 0;
            const size_t bytes = size * count;
            if (body.size() + bytes > kMaxBody) return 0;
            body.append(ptr, bytes);
            return bytes;
        }
        size_t WriteHeader(char* ptr, size_t size, size_t count, void* data)
        {
            const size_t bytes = size * count;
            std::string line(ptr, bytes);
            if (line.size() >= 9)
            {
                std::string prefix = line.substr(0, 9);
                std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (prefix == "location:" && bytes < 4096)
                    *static_cast<std::string*>(data) = line.substr(9);
            }
            return bytes;
        }
        void ConfigureRequest(CURL* curl, const std::string& url, HttpResult& result,
                              int timeoutMs, const Operation::Cancelled& cancelled)
        {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeoutMs));
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeader);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result.location);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, (std::string("AutoSZUWeb/") + AppVersion::String).c_str());
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled);
        }
        CURLcode Perform(CURL* curl, int timeoutMs, const Operation::Cancelled& cancelled)
        {
            CURLM* multi = curl_multi_init();
            if (!multi) return CURLE_FAILED_INIT;
            CURLcode result = CURLE_OPERATION_TIMEDOUT;
            if (curl_multi_add_handle(multi, curl) != CURLM_OK)
            { curl_multi_cleanup(multi); return CURLE_FAILED_INIT; }
            const auto deadline = Milliseconds() + timeoutMs;
            int running = 0;
            while (!IsCancelled(cancelled) && Milliseconds() < deadline)
            {
                if (curl_multi_perform(multi, &running) != CURLM_OK) break;
                int remaining = 0;
                if (auto* message = curl_multi_info_read(multi, &remaining))
                { result = message->data.result; break; }
                if (!running) break;
                int ready = 0;
                curl_multi_poll(multi, nullptr, 0,
                    static_cast<int>(std::max(1LL, std::min(50LL, deadline - Milliseconds()))), &ready);
            }
            if (IsCancelled(cancelled)) result = CURLE_ABORTED_BY_CALLBACK;
            curl_multi_remove_handle(multi, curl);
            curl_multi_cleanup(multi);
            return result;
        }
        nlohmann::json Jsonp(const std::string& body)
        {
            try
            {
                const auto start = body.find('('), end = body.rfind(')');
                const auto text = start != std::string::npos && end > start
                    ? body.substr(start + 1, end - start - 1) : body;
                return nlohmann::json::parse(text);
            }
            catch (...) { return {}; }
        }
        uint16_t Read16(const std::vector<unsigned char>& bytes, size_t pos)
        { return static_cast<uint16_t>((bytes[pos] << 8) | bytes[pos + 1]); }
        bool SkipName(const std::vector<unsigned char>& bytes, size_t& pos)
        {
            size_t labels = 0;
            while (pos < bytes.size() && ++labels < 128)
            {
                unsigned char length = bytes[pos++];
                if (length == 0) return true;
                if ((length & 0xc0) == 0xc0)
                {
                    if (pos >= bytes.size()) return false;
                    const size_t offset = ((length & 0x3f) << 8) | bytes[pos++];
                    return offset < bytes.size();
                }
                if (length > 63 || pos + length > bytes.size()) return false;
                pos += length;
            }
            return false;
        }
        std::string Answer(const std::vector<unsigned char>& bytes, uint16_t id)
        {
            if (bytes.size() < 12 || Read16(bytes, 0) != id ||
                !(bytes[2] & 0x80) || (bytes[2] & 2) || (bytes[3] & 0xf)) return {};
            size_t pos = 12;
            for (unsigned i = 0; i < Read16(bytes, 4); ++i)
            {
                if (!SkipName(bytes, pos) || pos + 4 > bytes.size()) return {};
                pos += 4;
            }
            for (unsigned i = 0; i < Read16(bytes, 6); ++i)
            {
                if (!SkipName(bytes, pos) || pos + 10 > bytes.size()) return {};
                const auto type = Read16(bytes, pos), cls = Read16(bytes, pos + 2);
                const auto length = Read16(bytes, pos + 8);
                pos += 10;
                if (pos + length > bytes.size()) return {};
                if (type == 1 && cls == 1 && length == 4)
                {
                    // Proxy fake-IP ranges are not a usable physical-network answer.
                    if (bytes[pos] == 198 && (bytes[pos + 1] == 18 || bytes[pos + 1] == 19))
                        return {};
                    char result[INET_ADDRSTRLEN] = {};
                    if (inet_ntop(AF_INET, bytes.data() + pos, result, sizeof(result)))
                        return result;
                }
                pos += length;
            }
            return {};
        }
        std::string MaskIp(std::string ip)
        {
            const auto dot = ip.rfind('.');
            return dot == std::string::npos ? "未取得" : ip.substr(0, dot + 1) + "*";
        }
    }

    bool EnsureCurl()
    {
        static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
        return ready;
    }

    DnsResult ResolveDirect(const Environment& env, const std::string& host, int timeoutMs,
                            const Operation::Cancelled& cancelled)
    {
        DnsResult result;
        if (timeoutMs <= 0 || IsCancelled(cancelled))
        { result.error = "解析已取消或已到截止时间"; return result; }
        if (env.localIp.empty() || env.interfaceName.empty())
        { result.error = "没有可用的物理 IPv4 网络"; return result; }
        if (host.empty() || host.size() > 253)
        { result.error = "域名格式异常"; return result; }
        in_addr literal{};
        if (inet_pton(AF_INET, host.c_str(), &literal) == 1)
        { result.ip = host; return result; }
        const uint16_t id = static_cast<uint16_t>(arc4random_uniform(65536));
        std::vector<unsigned char> query = {static_cast<unsigned char>(id >> 8),
            static_cast<unsigned char>(id), 1, 0, 0, 1, 0, 0, 0, 0, 0, 0};
        std::istringstream labels(host);
        std::string label;
        while (std::getline(labels, label, '.'))
        {
            if (label.empty() || label.size() > 63)
            { result.error = "域名格式异常"; return result; }
            query.push_back(static_cast<unsigned char>(label.size()));
            query.insert(query.end(), label.begin(), label.end());
        }
        query.insert(query.end(), {0, 0, 1, 0, 1});
        auto servers = env.dnsServers;
        if (std::find(servers.begin(), servers.end(), "223.5.5.5") == servers.end())
            servers.push_back("223.5.5.5");
        if (servers.size() > 3) servers.resize(3);
        const auto deadline = Milliseconds() + std::max(1, timeoutMs);
        for (size_t i = 0; i < servers.size(); ++i)
        {
            if (IsCancelled(cancelled)) break;
            sockaddr_in server{};
            server.sin_family = AF_INET; server.sin_port = htons(53);
            if (inet_pton(AF_INET, servers[i].c_str(), &server.sin_addr) != 1) continue;
            const auto serverIp = ntohl(server.sin_addr.s_addr);
            if ((serverIp >> 24) == 127 || (serverIp >> 17) == (0xc6120000u >> 17)) continue;
            const int remaining = static_cast<int>(deadline - Milliseconds());
            if (remaining <= 0) break;
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) continue;
#ifdef __APPLE__
            const unsigned index = if_nametoindex(env.interfaceName.c_str());
            if (!index || setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index)) != 0)
            { close(fd); continue; }
#endif
            sockaddr_in local{}; local.sin_family = AF_INET;
            inet_pton(AF_INET, env.localIp.c_str(), &local.sin_addr);
            const int waitMs = std::max(1, remaining / static_cast<int>(servers.size() - i));
            if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
                connect(fd, reinterpret_cast<sockaddr*>(&server), sizeof(server)) != 0 ||
                send(fd, query.data(), query.size(), 0) != static_cast<ssize_t>(query.size()))
            { close(fd); continue; }
            std::vector<unsigned char> response(4096);
            const auto serverDeadline = Milliseconds() + waitMs;
            ssize_t count = -1;
            while (!IsCancelled(cancelled))
            {
                const int left = static_cast<int>(serverDeadline - Milliseconds());
                if (left <= 0) break;
                pollfd descriptor{fd, POLLIN, 0};
                if (poll(&descriptor, 1, std::min(left, 50)) > 0)
                {
                    count = recv(fd, response.data(), response.size(), 0);
                    break;
                }
            }
            close(fd);
            if (count <= 0) continue;
            response.resize(static_cast<size_t>(count));
            result.ip = Answer(response, id);
            if (result.Ok()) return result;
        }
        result.error = IsCancelled(cancelled) ? "解析已取消" : "物理网络 DNS 未取得有效 IPv4 响应";
        return result;
    }

    curl_slist* ConfigureDirect(CURL* curl, const Environment& env,
                               const std::string& host, const std::string& ip)
    {
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(curl, CURLOPT_INTERFACE, ("if!" + env.interfaceName).c_str());
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_slist* resolves = nullptr;
        if (!host.empty() && !ip.empty())
        {
            resolves = curl_slist_append(resolves, (host + ":443:" + ip).c_str());
            resolves = curl_slist_append(resolves, (host + ":80:" + ip).c_str());
            curl_easy_setopt(curl, CURLOPT_RESOLVE, resolves);
        }
        return resolves;
    }

    HttpResult Get(const std::string& url, const Environment* direct,
                   const std::string& host, const std::string& ip, int timeoutMs,
                   const Operation::Cancelled& cancelled)
    {
        HttpResult result;
        if (IsCancelled(cancelled)) { result.error = CURLE_ABORTED_BY_CALLBACK; return result; }
        if (timeoutMs <= 0) { result.error = CURLE_OPERATION_TIMEDOUT; return result; }
        if (!EnsureCurl()) return result;
        if (direct && (direct->interfaceName.empty() || direct->localIp.empty()))
        { result.error = CURLE_INTERFACE_FAILED; return result; }
        auto* curl = curl_easy_init();
        if (!curl) return result;
        auto* resolves = direct ? ConfigureDirect(curl, *direct, host, ip) : nullptr;
        ConfigureRequest(curl, url, result, timeoutMs, cancelled);
        const auto start = Milliseconds();
        result.error = Perform(curl, timeoutMs, cancelled);
        result.elapsedMs = static_cast<int>(Milliseconds() - start);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
        curl_easy_cleanup(curl);
        curl_slist_free_all(resolves);
        return result;
    }

    bool IsDormitoryResponse(const std::string& body)
    {
        const auto data = Jsonp(body);
        if (!data.is_object()) return false;
        const auto message = data.find("msg");
        // The empty-account response is specific to this read-only portal probe.
        return data.contains("result") && data.contains("ret_code") &&
            message != data.end() && message->is_string() &&
            message->get<std::string>().find("无法获取用户认证账号") != std::string::npos;
    }
    bool IsTeachingResponse(const std::string& body)
    {
        const auto data = Jsonp(body);
        if (!data.is_object()) return false;
        const auto error = data.find("error");
        const auto ip = data.find("online_ip"), user = data.find("user_name");
        return error != data.end() && error->is_string() &&
            (error->get<std::string>() == "not_online_error" ||
             (error->get<std::string>() == "ok" && ip != data.end() && ip->is_string() &&
              !ip->get_ref<const std::string&>().empty() && user != data.end() && user->is_string() &&
              !user->get_ref<const std::string&>().empty()));
    }
    const char* PortalName(Portal portal)
    {
        switch (portal)
        {
            case Portal::Dormitory: return "深大宿舍区 ePortal";
            case Portal::Teaching: return "深大教学区 SRun";
            case Portal::None: return "其他或未确认网络";
        }
        return "其他或未确认网络";
    }
    Campus DetectCampus(const Environment& env, const Operation::Cancelled& cancelled)
    {
        Campus context; context.env = env;
        if (IsCancelled(cancelled)) { context.evidence = "网络识别已取消"; return context; }
        if (env.localIp.empty()) { context.evidence = "物理网卡没有 IPv4 地址"; return context; }
        auto dorm = std::async(std::launch::async, [&] {
            return Get("http://172.30.255.42:801/eportal/portal/login", &env, {}, {}, 1500, cancelled);
        });
        auto teaching = std::async(std::launch::async, [&] {
            const auto dns = ResolveDirect(env, "net.szu.edu.cn", 900, cancelled);
            in_addr address{};
            const auto value = inet_pton(AF_INET, dns.ip.c_str(), &address) == 1
                ? ntohl(address.s_addr) : 0;
            // A public portal reachable from home is not proof of being on campus.
            const bool campusLocal = (value >> 24) == 10 || (value >> 20) == 0xac1;
            return std::make_pair(dns, dns.Ok() && campusLocal ? Get(
                "https://net.szu.edu.cn/cgi-bin/rad_user_info?callback=_", &env,
                "net.szu.edu.cn", dns.ip, 1800, cancelled) : HttpResult{});
        });
        const auto dormResult = dorm.get();
        const auto teachingResult = teaching.get();
        if (IsCancelled(cancelled)) { context.evidence = "网络识别已取消"; return context; }
        if (dormResult.Ok() && dormResult.status == 200 && IsDormitoryResponse(dormResult.body))
        {
            context.portal = Portal::Dormitory;
            context.serverIp = "172.30.255.42";
            context.evidence = "物理网络上的宿舍门户返回了可识别的认证响应（未提交账号）";
        }
        else if (teachingResult.second.Ok() && teachingResult.second.status == 200 &&
                 IsTeachingResponse(teachingResult.second.body))
        {
            context.portal = Portal::Teaching;
            const auto status = Jsonp(teachingResult.second.body);
            context.session = status.value("error", "") == "ok" ? Session::Online : Session::Offline;
            context.serverIp = teachingResult.first.ip;
            context.evidence = "物理网络上的深大教学门户返回了可识别的在线状态响应";
        }
        else
            context.evidence = "未通过物理网络确认深大认证门户；不会提交账号密码";
        return context;
    }

    namespace
    {
        std::string UrlHost(std::string url)
        {
            const auto first = url.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return {};
            url = url.substr(first, url.find_last_not_of(" \t\r\n") - first + 1);
            CURLU* parsed = curl_url();
            if (!parsed) return {};
            char* host = nullptr;
            char* scheme = nullptr;
            if (curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK)
            {
                curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0);
                if (scheme && (std::strcmp(scheme, "http") == 0 || std::strcmp(scheme, "https") == 0))
                    curl_url_get(parsed, CURLUPART_HOST, &host, 0);
            }
            std::string value = host ? host : "";
            curl_free(host); curl_free(scheme); curl_url_cleanup(parsed);
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }
        bool CampusRedirect(const HttpResult& http, const Environment* direct, const Campus* campus)
        {
            if (!direct || !campus || !campus->Confirmed() || direct->Key() != campus->env.Key())
                return false;
            if (http.status != 511 && (http.status < 300 || http.status >= 400)) return false;
            const auto host = UrlHost(http.location);
            return campus->portal == Portal::Dormitory ? host == "172.30.255.42"
                : host == "net.szu.edu.cn";
        }
        struct ProbeRequest
        {
            CURL* curl = nullptr;
            curl_slist* resolves = nullptr;
            HttpResult http;
            int index = -1;
            long long started = 0;
            ~ProbeRequest()
            {
                if (curl) curl_easy_cleanup(curl);
                curl_slist_free_all(resolves);
            }
        };
        struct BatchResult
        {
            InternetResult value;
            int winner = -1;
        };
        BatchResult ProbeBatch(const std::vector<std::string>& urls, const std::vector<int>& indices,
                               const Environment* direct, long long deadline, bool collectAll,
                               const Campus* campus, const Operation::Cancelled& cancelled)
        {
            BatchResult result;
            if (!EnsureCurl() || indices.empty() || IsCancelled(cancelled)) return result;
            std::vector<std::future<DnsResult>> resolutions;
            if (direct)
                for (int index : indices)
                    resolutions.push_back(std::async(std::launch::async, [&, index] {
                        return ResolveDirect(*direct, UrlHost(urls[index]),
                            static_cast<int>(std::min(900LL, deadline - Milliseconds())), cancelled);
                    }));
            CURLM* multi = curl_multi_init();
            if (!multi) return result;
            std::vector<std::unique_ptr<ProbeRequest>> requests;
            for (size_t i = 0; i < indices.size(); ++i)
            {
                DnsResult dns;
                if (direct)
                {
                    dns = resolutions[i].get();
                    if (!dns.Ok()) { ++result.value.dnsFailures; continue; }
                }
                const int remaining = static_cast<int>(deadline - Milliseconds());
                if (remaining <= 0 || IsCancelled(cancelled)) continue;
                auto request = std::make_unique<ProbeRequest>();
                request->curl = curl_easy_init();
                if (!request->curl) continue;
                request->index = indices[i];
                if (direct)
                    request->resolves = ConfigureDirect(request->curl, *direct,
                        UrlHost(urls[indices[i]]), dns.ip);
                ConfigureRequest(request->curl, urls[indices[i]], request->http, remaining, cancelled);
                curl_easy_setopt(request->curl, CURLOPT_PRIVATE, request.get());
                request->started = Milliseconds();
                if (curl_multi_add_handle(multi, request->curl) == CURLM_OK)
                    requests.push_back(std::move(request));
            }
            bool done = requests.empty();
            while (!done && !IsCancelled(cancelled) && Milliseconds() < deadline)
            {
                int running = 0;
                if (curl_multi_perform(multi, &running) != CURLM_OK) break;
                int pending = 0;
                while (auto* message = curl_multi_info_read(multi, &pending))
                {
                    if (message->msg != CURLMSG_DONE) continue;
                    ProbeRequest* request = nullptr;
                    curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &request);
                    auto& http = request->http;
                    http.error = message->data.result;
                    http.elapsedMs = static_cast<int>(Milliseconds() - request->started);
                    curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &http.status);
                    if (!http.Ok()) continue;
                    if (http.status == 204)
                    {
                        if (result.winner < 0) result.winner = request->index;
                        result.value.state = Internet::Online;
                        result.value.campusIntercept = false;
                        if (result.value.latencyMs < 0 || http.elapsedMs < result.value.latencyMs)
                            result.value.latencyMs = http.elapsedMs;
                        if (!collectAll) { done = true; break; }
                    }
                    else if (result.value.state != Internet::Online &&
                        (http.status == 511 || (http.status >= 300 && http.status < 400) ||
                         (http.status == 200 && (http.body.find("eportal") != std::string::npos ||
                                                http.body.find("srun_portal") != std::string::npos))))
                    {
                        result.value.state = Internet::Captive;
                        result.value.campusIntercept |= CampusRedirect(http, direct, campus);
                    }
                }
                if (!running || done) break;
                int ready = 0;
                curl_multi_poll(multi, nullptr, 0,
                    static_cast<int>(std::max(1LL, std::min(50LL, deadline - Milliseconds()))), &ready);
            }
            for (const auto& request : requests) curl_multi_remove_handle(multi, request->curl);
            requests.clear();
            curl_multi_cleanup(multi);
            return result;
        }
    }

    InternetResult CheckInternet(const Environment* direct, int timeoutMs, bool collectAll,
                                 const Campus* campus, const Operation::Cancelled& cancelled)
    {
        const auto started = Milliseconds();
        const auto deadline = started + std::max(0, timeoutMs);
        const size_t cacheIndex = direct ? 1 : 0;
        const std::string network = direct ? direct->Key() : campus ? campus->env.Key() : "default";
        std::vector<std::string> urls;
        int preferred = -1;
        bool rotate = false;
        {
            std::lock_guard<std::mutex> lock(probeMutex);
            urls = probeUrls;
            auto& cache = probeCaches[cacheIndex];
            if (cache.network != network) { cache = {}; cache.network = network; }
            preferred = cache.preferred;
            rotate = preferred >= 0 && ProbeClock() >= cache.refreshAfter;
            if (rotate && !urls.empty()) preferred = (preferred + 1) % urls.size();
        }
        std::vector<int> indices(urls.size());
        std::iota(indices.begin(), indices.end(), 0);
        BatchResult batch;
        if (!collectAll && preferred >= 0 && preferred < static_cast<int>(urls.size()))
        {
            const auto firstDeadline = std::min(deadline, started + std::max(1, std::min(1000, timeoutMs / 2)));
            batch = ProbeBatch(urls, {preferred}, direct, firstDeadline, false, campus, cancelled);
            indices.erase(std::remove(indices.begin(), indices.end(), preferred), indices.end());
        }
        if (batch.value.state != Internet::Online && !IsCancelled(cancelled) && Milliseconds() < deadline)
        {
            auto expanded = ProbeBatch(urls, indices, direct, deadline, collectAll, campus, cancelled);
            expanded.value.dnsFailures += batch.value.dnsFailures;
            if (expanded.value.state == Internet::Unknown && batch.value.state == Internet::Captive)
            { expanded.value.state = Internet::Captive; expanded.value.campusIntercept = batch.value.campusIntercept; }
            else if (expanded.value.state == Internet::Captive)
                expanded.value.campusIntercept |= batch.value.campusIntercept;
            batch = std::move(expanded);
        }
        auto& result = batch.value;
        result.cancelled = IsCancelled(cancelled);
        if (result.cancelled)
        { result.state = Internet::Unknown; result.campusIntercept = false; result.latencyMs = -1; }
        if (!collectAll && !result.cancelled && batch.winner >= 0)
        {
            std::lock_guard<std::mutex> lock(probeMutex);
            auto& cache = probeCaches[cacheIndex];
            if (cache.network == network)
            {
                if (cache.preferred != batch.winner || rotate || cache.refreshAfter == 0)
                    cache.refreshAfter = ProbeClock() + 600000;
                cache.preferred = batch.winner;
            }
        }
        result.elapsedMs = static_cast<int>(Milliseconds() - started);
        result.summary = result.cancelled ? "本次检查已取消" : InternetName(result.state);
        return result;
    }
#ifdef AUTOSZUWEB_NETWORK_TESTING
    namespace Testing
    {
        void SetProbeUrls(const std::vector<std::string>& urls)
        { std::lock_guard<std::mutex> lock(probeMutex); probeUrls = urls; probeCaches = {}; }
        void AdvanceProbeClock(int seconds)
        { std::lock_guard<std::mutex> lock(probeMutex); probeClockOffset += static_cast<long long>(seconds) * 1000; }
        void ResetProbeCache()
        { std::lock_guard<std::mutex> lock(probeMutex); probeCaches = {}; probeClockOffset = 0; }
    }
#endif
    const char* InternetName(Internet state)
    {
        switch (state)
        {
            case Internet::Online: return "可用（HTTP 204 验证通过）";
            case Internet::Captive: return "收到重定向或认证页，可能需要认证";
            case Internet::Unknown: return "未通过探测，原因尚不确定";
        }
        return "未知";
    }

    std::string Diagnose()
    {
        const auto env = ReadEnvironment();
        auto currentJob = std::async(std::launch::async, [] { return CheckInternet(nullptr, 3000, true); });
        auto directJob = std::async(std::launch::async, [&] { return CheckInternet(&env, 3000, true); });
        auto campusJob = std::async(std::launch::async, [&] { return DetectCampus(env); });
        auto dnsJob = std::async(std::launch::async, [&] { return ResolveDirect(env, "net.szu.edu.cn"); });
        const auto current = currentJob.get(), direct = directJob.get();
        const auto campus = campusJob.get();
        const auto dns = dnsJob.get();
        const auto snapshot = Status::Get();
        const auto now = std::time(nullptr);
        std::tm local{}; localtime_r(&now, &local);
        char stamp[40] = {}; std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        std::ostringstream report;
        report << "AutoSZUWeb 网络诊断 · " << AppVersion::String << "\n" << stamp << "\n\n"
            << "这是只读检查：未提交账号密码，未重置认证，未关闭代理或切换网络。\n\n"
            << "物理网络：" << (env.interfaceName.empty() ? "未连接或没有 IPv4" : env.interfaceName)
            << " / " << MaskIp(env.localIp) << "\n"
            << "默认出口：" << (env.tunnelDefault ? "代理／VPN 隧道（保持原设置）" : "物理网络或系统出口") << "\n"
            << "系统 HTTP／SOCKS／PAC 代理：" << (env.systemProxyKnown
                ? (env.systemProxyConfigured ? "已配置" : "当前关闭") : "配置状态未知") << "\n"
            << "校园网识别：" << PortalName(campus.portal) << "\n"
            << "识别依据：" << campus.evidence << "\n"
            << "校园域名 DNS（物理网络）：" << (dns.Ok() ? "解析成功" : dns.error) << "\n"
            << "默认路由路径外网：" << current.summary << "\n"
            << "物理直连外网：" << direct.summary << "\n\n";
        if (current.state == Internet::Online && direct.state == Internet::Online)
            report << "判断：默认路由与物理直连均可用，无需重新认证。\n";
        else if (direct.state == Internet::Online)
            report << "判断：物理直连可用，默认路由路径异常。优先检查代理节点或分流。\n";
        else if (current.state == Internet::Online)
            report << "判断：默认路由路径可用，物理直连未通过。需要结合代理、DNS 和网络访问策略进一步判断。\n";
        else if (env.localIp.empty())
            report << "判断：先检查 Wi-Fi／网线和 DHCP 地址。\n";
        else if (direct.dnsFailures == 3)
            report << "判断：物理网络 DNS 探测失败；先检查 DNS 或路由器，暂不能确定账号认证状态。\n";
        else if (campus.Confirmed())
            report << "判断：已识别深大门户，但外网未通过。可查看最近认证结果；本次诊断没有尝试登录。\n";
        else
            report << "判断：未确认深大门户，自动认证应待命。可能在热点、其他 Wi-Fi，或门户／路由不可达。\n";
        report << "\n最近认证：" << (snapshot.lastAuthTime.empty() ? "本次运行尚未提交认证" : snapshot.lastAuthTime)
            << "\n自动重试：" << snapshot.retryText << "\n"
            << "\n覆盖范围：默认路由检查使用本进程的路由和环境代理设置。浏览器的系统 HTTP／SOCKS／PAC 路径需单独检查。\n"
            << "\n注意：外网可用不等于已验证校园账号认证；门户可达也不等于账号认证成功。\n"
            << "报告不含账号、密码、SSID、MAC、节点地址或完整本机 IP；不会自动上传。\n";
        return report.str();
    }
}
