#include "eportal_response.h"
#include "network.h"
#include "status.h"

#include <iostream>

int main()
{
    int failures = 0;
    const auto check = [&](bool condition, const char* name) {
        std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
        if (!condition) ++failures;
    };
    using nlohmann::json;
    check(!EPortal::LoginSucceeded(json::parse(
        R"({"result":0,"msg":"无法获取用户认证账号！","ret_code":"1"})")),
        "live portal rejection is never success");
    check(!EPortal::LoginSucceeded({{"result", 0}, {"msg", "认证失败"}}),
        "rejected password remains rejected");
    check(EPortal::LoginSucceeded({{"result", 1}, {"msg", "认证成功"}}),
        "numeric success");
    check(EPortal::LoginSucceeded({{"result", "1"}, {"msg", "认证成功"}}),
        "string success");
    check(!EPortal::LoginSucceeded({{"msg", "未认证成功"}}),
        "negated success text is not success");
    check(!EPortal::LoginSucceeded({}), "empty response is not success");
    Status::RecordAuth(true, "test", "127.0.0.1", "");
    check(Status::Get().state == Status::State::Authenticating &&
        Status::Get().onlineSince == 0,
        "portal success waits for external connectivity verification");
    Status::SetState(Status::State::Online);
    check(Status::Get().onlineSince > 0, "verified connectivity starts uptime");
    const auto ip = GetLocalIp();
    std::cout << "Physical IP: " << ip << '\n';
    check(!ip.empty() && ip != "198.19.0.1", "VPN address does not hide Wi-Fi changes");
    return failures ? 1 : 0;
}
