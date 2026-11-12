// macOS 与 Linux 共用的 POSIX socket 实现。
#include "network.h"

#include "status.h"
#include "network_observation.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <thread>

using SockT = int;
static constexpr SockT kInvalidSocket = -1;

namespace
{
    bool SocketStartup()
    {
        return true;
    }

    void SocketCleanup()
    {
    }

    void CloseSock(SockT socketHandle)
    {
        ::close(socketHandle);
    }

    bool SetNonBlocking(SockT socketHandle)
    {
        const int flags = fcntl(socketHandle, F_GETFL, 0);
        return flags >= 0 && fcntl(socketHandle, F_SETFL, flags | O_NONBLOCK) == 0;
    }
}

std::string GetLocalIp()
{
    return Observation::ReadEnvironment().localIp;
}

bool ProbeTcpEndpoint(const std::string& ip, int port, int timeoutMs)
{
    if (ip.empty() || port <= 0 || port > 65535 || timeoutMs < 0)
        return false;
    if (!SocketStartup())
        return false;

    SockT socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == kInvalidSocket)
    {
        SocketCleanup();
        return false;
    }

    bool ok = false;
    if (SetNonBlocking(socketHandle))
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(port));
        if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) == 1)
        {
            int connectResult = connect(socketHandle,
                reinterpret_cast<sockaddr*>(&address), sizeof(address));
            if (connectResult == 0)
            {
                ok = true;
            }
            else
            {
                fd_set writable;
                FD_ZERO(&writable);
                FD_SET(socketHandle, &writable);
                timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
                const int selectResult = select(socketHandle + 1, nullptr, &writable, nullptr, &timeout);
                if (selectResult > 0)
                {
                    int error = 0;
                    socklen_t length = sizeof(error);
                    if (getsockopt(socketHandle, SOL_SOCKET, SO_ERROR,
                            reinterpret_cast<char*>(&error), &length) == 0)
                        ok = (error == 0);
                }
            }
        }
    }

    CloseSock(socketHandle);
    SocketCleanup();
    return ok;
}

bool HostReachable(const std::string& host, int port, int timeoutMs)
{
    if (host.empty() || port <= 0 || port > 65535 || !Observation::EnsureCurl())
        return false;

    // 用 curl 的 CONNECT_ONLY 代替 getaddrinfo + connect：
    // 系统解析器在冷缓存时可能阻塞十几秒，而 curl 的 DNS 走独立线程，受 timeout 约束。
    const std::string url =
        (port == 443 ? "https://" : "http://") + host + ":" + std::to_string(port) + "/";

    CURL* curl = curl_easy_init();
    if (!curl)
        return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AutoSZUWeb/1.4.1");

    const CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
}

bool NetworkCheck(int timeoutMs)
{
    const auto result = Observation::CheckInternet(nullptr, timeoutMs);
    Status::SetLatency(result.state == Observation::Internet::Online ? result.latencyMs : -1);
    return result.state == Observation::Internet::Online;
}
