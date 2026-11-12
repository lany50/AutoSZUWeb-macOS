#include "network_observation.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <string>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    int failures = 0;
    void Check(bool passed, const char* description)
    {
        std::cout << (passed ? "PASS " : "FAIL ") << description << '\n';
        if (!passed) ++failures;
    }
    long long Elapsed(Clock::time_point start)
    { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count(); }

    class Server
    {
        int listener = -1;
        std::atomic<bool> stopping{false};
        std::thread acceptThread;
        std::vector<std::thread> workers;
        std::mutex mutex;
        std::condition_variable cv;
        std::string location;

        void Handle(int client)
        {
            int enabled = 1;
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
            timeval timeout{0, 500000};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            char request[4096];
            if (recv(client, request, sizeof(request), 0) <= 0) { close(client); return; }
            ++requests;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(delayMs.load()), [&] { return stopping.load(); });
            }
            if (!stopping)
            {
                const int code = status.load();
                std::string response = "HTTP/1.1 " + std::to_string(code) + " Test\r\nContent-Length: 0\r\n";
                if (!location.empty()) response += "Location: " + location + "\r\n";
                response += "Connection: close\r\n\r\n";
                send(client, response.data(), response.size(), 0);
            }
            close(client);
        }
    public:
        std::atomic<int> requests{0};
        std::atomic<int> delayMs{0};
        std::atomic<int> status{204};
        std::string url;

        Server(int delay, int responseStatus = 204, std::string redirect = {})
            : location(std::move(redirect)), delayMs(delay), status(responseStatus)
        {
            listener = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
                listen(listener, 16)) throw std::runtime_error("loopback fixture setup failed");
            socklen_t length = sizeof(address);
            getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
            url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/generate_204";
            acceptThread = std::thread([this] {
                while (!stopping)
                {
                    pollfd descriptor{listener, POLLIN, 0};
                    if (poll(&descriptor, 1, 25) <= 0) continue;
                    int client = accept(listener, nullptr, nullptr);
                    if (client >= 0) workers.emplace_back([this, client] { Handle(client); });
                }
            });
        }
        ~Server()
        {
            stopping = true;
            cv.notify_all();
            if (acceptThread.joinable()) acceptThread.join();
            for (auto& worker : workers) worker.join();
            close(listener);
        }
    };
}

int main()
{
    using Observation::Internet;
    setenv("no_proxy", "127.0.0.1", 1);
    {
        Server slowA(3000), fast(50), slowB(3000);
        Observation::Testing::SetProbeUrls({slowA.url, fast.url, slowB.url});
        auto start = Clock::now();
        auto result = Observation::CheckInternet(nullptr, 3000);
        Check(result.state == Internet::Online && Elapsed(start) < 500,
            "50ms success returns within 500ms while other probes take three seconds");
        const int slowBefore = slowA.requests + slowB.requests;
        result = Observation::CheckInternet(nullptr, 3000);
        Check(result.state == Internet::Online && slowA.requests + slowB.requests == slowBefore,
            "stable monitoring reuses only the last successful endpoint");
        start = Clock::now();
        result = Observation::CheckInternet(nullptr, 3000, true, nullptr,
            [&] { return Elapsed(start) >= 100; });
        Check(result.cancelled && result.state == Internet::Unknown && result.latencyMs < 0,
            "cancellation discards an earlier success from an obsolete complete diagnosis");
    }
    {
        Server first(10), second(70), third(120);
        Observation::Testing::SetProbeUrls({first.url, second.url, third.url});
        Observation::CheckInternet();
        const int secondBefore = second.requests;
        Observation::Testing::AdvanceProbeClock(601);
        auto result = Observation::CheckInternet();
        Check(result.state == Internet::Online && second.requests == secondBefore + 1,
            "preferred endpoint rotates after ten minutes");
        first.status = 503;
        second.status = 503;
        const int thirdBefore = third.requests;
        result = Observation::CheckInternet(nullptr, 1000);
        Check(result.state == Internet::Online && third.requests > thirdBefore,
            "preferred endpoint failure expands to the other endpoints");
    }
    {
        Server first(10), second(160), third(250);
        Observation::Testing::SetProbeUrls({first.url, second.url, third.url});
        auto start = Clock::now();
        const auto result = Observation::CheckInternet(nullptr, 1000, true);
        Check(result.state == Internet::Online && Elapsed(start) >= 200 &&
              first.requests == 1 && second.requests == 1 && third.requests == 1,
            "complete diagnosis waits for and collects every configured endpoint");
    }
    {
        Server slow(3000);
        Observation::Testing::SetProbeUrls({slow.url});
        auto start = Clock::now();
        auto result = Observation::CheckInternet(nullptr, 180);
        Check(result.state == Internet::Unknown && Elapsed(start) < 450,
            "overall probe budget is honored without a hidden three-second minimum");
        start = Clock::now();
        auto cancelled = [&] { return Elapsed(start) >= 70; };
        result = Observation::CheckInternet(nullptr, 3000, false, nullptr, cancelled);
        Check(result.cancelled && result.state == Internet::Unknown && Elapsed(start) < 400,
            "network generation cancellation interrupts a pending probe promptly");
        start = Clock::now();
        const auto request = Observation::Get(slow.url, nullptr, {}, {}, 3000, cancelled);
        Check(request.error == CURLE_ABORTED_BY_CALLBACK && Elapsed(start) < 400,
            "individual portal requests support the same prompt cancellation");
    }
    {
        Server captive(0, 302, "http://172.30.255.42:801/eportal/");
        Observation::Testing::SetProbeUrls({captive.url});
        Observation::Environment physical;
        physical.interfaceName = "lo0";
        physical.localIp = "127.0.0.1";
        Observation::Campus campus;
        campus.portal = Observation::Portal::Dormitory;
        campus.env = physical;
        auto result = Observation::CheckInternet(nullptr, 500, false, &campus);
        Check(result.state == Internet::Captive && !result.campusIntercept,
            "default-route redirects do not authorize campus authentication");
        result = Observation::CheckInternet(&physical, 500, false, &campus);
        Check(result.state == Internet::Captive && result.campusIntercept,
            "confirmed physical campus redirect is recognized as an authentication intercept");
        campus.env.localIp = "127.0.0.2";
        result = Observation::CheckInternet(&physical, 500, false, &campus);
        Check(!result.campusIntercept, "stale campus context cannot authorize a redirect");
    }
    {
        Server unrelated(0, 302, "http://net.szu.edu.cn.example.invalid/");
        Observation::Testing::SetProbeUrls({unrelated.url});
        Observation::Environment physical;
        physical.interfaceName = "lo0";
        physical.localIp = "127.0.0.1";
        Observation::Campus campus;
        campus.portal = Observation::Portal::Teaching;
        campus.env = physical;
        const auto result = Observation::CheckInternet(&physical, 500, false, &campus);
        Check(result.state == Internet::Captive && !result.campusIntercept,
            "a domain containing the campus name does not count as a trusted campus redirect");
    }
    Check(!Observation::IsTeachingResponse(R"({"error":"ok","online_ip":null,"user_name":42})"),
        "malformed teaching session fields cannot claim an online campus session");
    Check(Observation::IsTeachingResponse(R"({"error":"ok","online_ip":"192.0.2.1","user_name":"synthetic"})"),
        "typed teaching session fields identify an online campus session");
    {
        Observation::Environment before;
        before.interfaceName = "en0";
        before.localIp = "192.0.2.2";
        auto after = before;
        after.defaultInterface = "utun-test";
        Check(before.Key() != after.Key(), "default route changes invalidate network evidence");
        after = before;
        after.dnsServers = {"192.0.2.53"};
        Check(before.Key() != after.Key(), "DNS changes invalidate network evidence");
        Server fast(50), other(150);
        Observation::Testing::SetProbeUrls({fast.url, other.url});
        Observation::Campus context;
        context.env = before;
        Observation::CheckInternet(nullptr, 1000, false, &context);
        const int earlier = other.requests;
        context.env = after;
        Observation::CheckInternet(nullptr, 1000, false, &context);
        Check(other.requests > earlier, "network changes invalidate default-route endpoint preference");
    }
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
