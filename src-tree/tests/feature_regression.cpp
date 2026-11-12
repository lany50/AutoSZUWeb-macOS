#include "auth_policy.h"
#include "monitor_policy.h"
#include "network_observation.h"
#include "status.h"
#include "cli.h"
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace
{
    int failures = 0;
    void Check(bool ok, const char* text)
    { std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; if (!ok) ++failures; }
    class HttpServer
    {
        int fd = -1;
        std::thread worker;
    public:
        std::string url;
        explicit HttpServer(std::string response)
        {
            fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(fd, 1))
                throw std::runtime_error("loopback setup failed");
            socklen_t length = sizeof(address);
            getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
            url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/";
            worker = std::thread([this, response] {
                fd_set read; FD_ZERO(&read); FD_SET(fd, &read); timeval timeout{3, 0};
                if (select(fd + 1, &read, nullptr, nullptr, &timeout) <= 0) return;
                int client = accept(fd, nullptr, nullptr);
                if (client < 0) return;
                int one = 1; setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
                char buffer[2048]; recv(client, buffer, sizeof(buffer), 0);
                size_t written = 0;
                while (written < response.size())
                {
                    auto bytes = send(client, response.data() + written, response.size() - written, 0);
                    if (bytes <= 0) break;
                    written += bytes;
                }
                close(client);
            });
        }
        ~HttpServer() { if (worker.joinable()) worker.join(); if (fd >= 0) close(fd); }
    };
}
int main()
{
    using Auth::Failure;
    using Monitor::Action;
    const auto Decide = [](bool enabled, bool manual, bool confirmed, bool online, bool directOnline,
                           const Auth::RetryPolicy& policy, long long now) {
        Observation::Campus campus;
        if (confirmed) { campus.portal = Observation::Portal::Teaching; campus.session = Observation::Session::Offline; }
        Observation::InternetResult current, direct;
        if (online) current.state = Observation::Internet::Online;
        if (directOnline) direct.state = Observation::Internet::Online;
        return Monitor::Decide(enabled, manual, campus, current, direct, policy, now);
    };
    Auth::RetryPolicy retry;
    Check(Auth::ClassifyRejection("E2901: bind_user2: ldap_bind error") == Failure::Server,
        "observed LDAP backend failure does not accuse user password");
    Check(Auth::ClassifyRejection("用户密码错误") == Failure::Credentials, "wrong password recognized");
    Check(Auth::ClassifyRejection("终端数量已达上限") == Failure::AccountRestricted, "device limit recognized");
    Check(Auth::ClassifyRejection("未知返回码 12345") == Failure::Rejected, "unknown codes remain uncertain");
    retry.Fail(Failure::Credentials, 100);
    Check(retry.blocked && !retry.CanAttempt(100000), "bad credentials never retry on a timer");
    retry.NetworkChanged();
    Check(retry.blocked, "Wi-Fi change does not restart rejected credentials");
    retry.Reset();
    Check(retry.CanAttempt(100), "explicit manual reset permits a retry");
    retry.Fail(Failure::Transport, 100);
    Check(!retry.CanAttempt(104) && retry.CanAttempt(105), "first timeout waits five seconds");
    for (int i = 0; i < 10; ++i) retry.Fail(Failure::Transport, 200);
    Check(retry.nextAttempt == 260, "repeated timeout caps at sixty seconds");
    retry.Reset(); retry.Fail(Failure::Server, 100);
    Check(!retry.blocked && retry.nextAttempt == 220, "backend failure waits two minutes");
    retry.Reset();
    for (int i = 0; i < 3; ++i) retry.Fail(Failure::Rejected, 100);
    Check(retry.blocked, "three unknown rejections stop login spam");
    retry.Reset();
    Check(Decide(true,false,false,false,false,retry,100) == Action::Waiting,
        "unconfirmed home Wi-Fi or hotspot never authenticates");
    Check(Decide(true,true,false,false,false,retry,100) == Action::Waiting,
        "manual click still requires confirmed campus portal");
    Check(Decide(true,false,true,true,false,retry,100) == Action::Healthy,
        "working connection never resets campus session");
    Check(Decide(true,false,true,false,true,retry,100) == Action::PathProblem,
        "healthy direct path with broken proxy never triggers login");
    Check(Decide(false,false,true,false,false,retry,100) == Action::Paused,
        "disabled automatic auth sends no login");
    Check(Decide(false,true,true,false,false,retry,100) == Action::Authenticate,
        "manual authentication works with automatic auth disabled");
    retry.Fail(Failure::Transport, 100);
    Check(Decide(true,false,true,false,false,retry,101) == Action::Cooling,
        "confirmed offline campus respects cooldown");
    retry.Reset(); retry.Fail(Failure::AccountRestricted,100);
    Check(Decide(true,false,true,false,false,retry,10000) == Action::NeedUser,
        "restricted account requires user action");
    Check(!Observation::IsDormitoryResponse("<html>home router</html>"), "home router HTML is not campus proof");
    Check(!Observation::IsDormitoryResponse(R"({"result":0,"ret_code":1,"msg":42})"),
        "malformed portal types do not crash detection");
    Check(Observation::IsDormitoryResponse(R"(jsonpReturn({"result":0,"ret_code":"1","msg":"无法获取用户认证账号！"});)"),
        "captured real dormitory response identifies portal");
    Check(!Observation::IsTeachingResponse(R"({"error":"ok"})"), "generic OK response is not SRun proof");
    Check(Observation::IsTeachingResponse(R"(_({"error":"not_online_error"}))"), "SRun offline state identifies portal");
    Observation::Environment empty;
    Check(!Observation::ResolveDirect(empty,"net.szu.edu.cn",10).Ok(), "no physical interface cannot resolve via proxy fallback");
    Check(!Observation::Get("http://127.0.0.1/",&empty).Ok(), "missing physical route never falls back to proxy");
    setenv("no_proxy","127.0.0.1",1);
    {
        HttpServer server("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
        const auto http = Observation::Get(server.url, nullptr, {}, {}, 1000);
        Check(http.Ok() && http.status == 204, "HTTP probe accepts real 204 response");
    }
    {
        HttpServer server("HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/should-not-follow\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        const auto http = Observation::Get(server.url, nullptr, {}, {}, 1000);
        Check(http.Ok() && http.status == 302 && !http.location.empty(), "captive redirect is observed without following it");
    }
    {
        HttpServer server("HTTP/1.1 200 OK\r\nContent-Length: 70000\r\nConnection: close\r\n\r\n" + std::string(70000,'x'));
        const auto http = Observation::Get(server.url, nullptr, {}, {}, 1000);
        Check(!http.Ok() && http.body.size() <= 65536, "untrusted portal response is bounded");
    }
    Status::RequestDiagnostic();
    Check(Status::Get().diagnosticRunning && Status::ConsumeDiagnosticRequest(), "diagnosis request wakes background worker");
    const auto count = Status::Get().authCount;
    Status::CompleteDiagnostic("test result");
    Check(!Status::Get().diagnosticRunning && Status::Get().diagnosticSequence == 1 &&
        Status::Get().authCount == count, "diagnosis completion never records a login");
    Check(Cli::Parse({"--diagnose"}).action == Cli::Action::Diagnose, "read-only CLI diagnostic supported");
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
