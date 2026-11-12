#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace Auth
{
    enum class Failure { None, Transport, Server, Credentials, AccountRestricted,
                         Rejected, Protocol, NotCampus, Cancelled, Busy };

    inline const char* FailureName(Failure kind)
    {
        switch (kind)
        {
            case Failure::None: return "无";
            case Failure::Transport: return "网络超时或连接失败";
            case Failure::Server: return "认证服务异常";
            case Failure::Credentials: return "账号或密码被拒绝";
            case Failure::AccountRestricted: return "账号状态或设备数量受限";
            case Failure::Rejected: return "认证被拒绝";
            case Failure::Protocol: return "认证响应异常";
            case Failure::NotCampus: return "未确认校园网环境";
            case Failure::Cancelled: return "网络或配置已变化，本次操作已结束";
            case Failure::Busy: return "已有认证正在进行";
        }
        return "未知";
    }

    inline Failure ClassifyRejection(std::string message)
    {
        std::transform(message.begin(), message.end(), message.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // LDAP backend failures are not proof of a wrong password.
        if (message.find("ldap_bind") != std::string::npos ||
            message.find("e2901") != std::string::npos ||
            message.find("系统繁忙") != std::string::npos ||
            message.find("服务异常") != std::string::npos ||
            message.find("服务器内部") != std::string::npos)
            return Failure::Server;
        for (const auto* word : {"密码错误", "密码不正确", "密码有误", "账号不存在",
                "用户名不存在", "账号或密码", "用户名或密码", "password error",
                "invalid password", "invalid credentials"})
            if (message.find(word) != std::string::npos)
                return Failure::Credentials;
        for (const auto* word : {"欠费", "停用", "禁用", "冻结", "锁定", "设备数",
                "终端数", "连接数", "连接数量", "在线数", "同时在线", "已达上限",
                "被禁止", "mac绑定", "绑定错误"})
            if (message.find(word) != std::string::npos)
                return Failure::AccountRestricted;
        return Failure::Rejected;
    }

    struct Result
    {
        bool ok = false;
        bool alreadyOnline = false;
        Failure failure = Failure::None;
        std::string message;
        std::string method;
    };

    // Pure retry policy: callers supply a monotonic timestamp in seconds.
    class RetryPolicy
    {
    public:
        Failure failure = Failure::None;
        int attempts = 0;
        bool blocked = false;
        long long nextAttempt = 0;

        void Reset() { *this = RetryPolicy{}; }
        void NetworkChanged()
        {
            if (failure != Failure::Credentials && failure != Failure::AccountRestricted)
                Reset();
        }
        void Fail(Failure kind, long long now)
        {
            attempts = failure == kind ? attempts + 1 : 1;
            failure = kind;
            blocked = kind == Failure::Credentials || kind == Failure::AccountRestricted ||
                ((kind == Failure::Rejected || kind == Failure::Protocol) && attempts >= 3);
            if (blocked) { nextAttempt = 0; return; }
            int base = kind == Failure::Server ? 120 :
                (kind == Failure::Transport ? 5 : 30);
            int maximum = kind == Failure::Server ? 600 :
                (kind == Failure::Transport ? 60 : 300);
            int delay = base;
            for (int i = 1; i < attempts && delay < maximum; ++i)
                delay = std::min(delay * 2, maximum);
            nextAttempt = now + delay;
        }
        bool CanAttempt(long long now) const { return !blocked && now >= nextAttempt; }
        long long Remaining(long long now) const
        { return blocked ? 0 : std::max(0LL, nextAttempt - now); }
    };
}
