#include "app_ui.h"
#include "eportal_response.h"
#include "network.h"
#include "status.h"
#include "web.h"
#include "internal/curl_cancel.h"
#include "srun.h"
#include "sys.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <string>

bool IsFirstBoot();

namespace
{
    size_t WriteCallback(void* contents, size_t size, size_t count, void* data)
    {
        auto& body = *static_cast<std::string*>(data);
        if (size && count > 65536 / size) return 0;
        if (body.size() + size * count > 65536) return 0;
        body.append(static_cast<char*>(contents), size * count);
        return size * count;
    }

    void ReplaceAll(std::string& text, const std::string& secret)
    {
        if (secret.empty()) return;
        size_t pos = 0;
        while ((pos = text.find(secret, pos)) != std::string::npos)
        {
            text.replace(pos, secret.size(), "[已隐藏]");
            pos += std::string("[已隐藏]").size();
        }
    }

    std::string Sanitize(std::string text, const std::string& account,
                          const std::string& password)
    {
        ReplaceAll(text, password);
        ReplaceAll(text, account);
        auto* curl = curl_easy_init();
        if (curl)
        {
            for (const auto* value : {&password, &account})
            {
                auto* encoded = curl_easy_escape(curl, value->c_str(), static_cast<int>(value->size()));
                if (encoded) { ReplaceAll(text, encoded); curl_free(encoded); }
            }
            curl_easy_cleanup(curl);
        }
        for (auto& ch : text)
            if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
        if (text.size() > 480)
        {
            size_t end = 480;
            while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
            text = text.substr(0, end) + "…";
        }
        return text;
    }

    Auth::Result LoginDormitory(const std::string& account, const std::string& password,
                                const Observation::Campus& context,
                                const Operation::Cancelled& cancelled)
    {
        Auth::Result outcome;
        outcome.method = "ePortal(宿舍区)";
        outcome.failure = Auth::Failure::Protocol;
        CURL* curl = curl_easy_init();
        if (!curl) { outcome.message = "curl 初始化失败"; return outcome; }
        char* user = curl_easy_escape(curl, account.c_str(), static_cast<int>(account.size()));
        char* pass = curl_easy_escape(curl, password.c_str(), static_cast<int>(password.size()));
        if (!user || !pass)
        {
            curl_free(user); curl_free(pass); curl_easy_cleanup(curl);
            outcome.message = "宿舍区请求参数编码失败";
            return outcome;
        }
        const std::string url = "http://172.30.255.42:801/eportal/portal/login?user_account=%2C0%2C"
            + std::string(user) + "&user_password=" + pass;
        curl_free(user); curl_free(pass);
        auto* resolves = Observation::ConfigureDirect(curl, context.env);
        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        Operation::ConfigureCancellation(curl, cancelled);
        const auto code = Operation::Perform(curl, cancelled);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (code == CURLE_ABORTED_BY_CALLBACK || Operation::IsCancelled(cancelled))
        {
            outcome.failure = Auth::Failure::Cancelled;
            outcome.message = Auth::FailureName(outcome.failure);
        }
        else if (code != CURLE_OK)
        {
            outcome.failure = Auth::Failure::Transport;
            outcome.message = "宿舍区请求失败：" + std::string(curl_easy_strerror(code));
        }
        else if (status >= 500)
        {
            outcome.failure = Auth::Failure::Server;
            outcome.message = "认证服务 HTTP " + std::to_string(status);
        }
        else if (status != 200)
            outcome.message = "认证接口返回 HTTP " + std::to_string(status);
        else
        {
            try
            {
                const auto start = body.find('('), end = body.rfind(')');
                const auto response = nlohmann::json::parse(
                    start != std::string::npos && end != std::string::npos && end > start
                        ? body.substr(start + 1, end - start - 1) : body);
                const auto msg = response.find("msg");
                outcome.message = msg != response.end() && msg->is_string()
                    ? msg->get<std::string>() : "认证接口未提供结果说明";
                outcome.ok = EPortal::LoginSucceeded(response);
                outcome.failure = outcome.ok ? Auth::Failure::None
                    : Auth::ClassifyRejection(outcome.message);
                const auto result = response.find("result");
                if (result != response.end() && (result->is_number() || result->is_string()))
                    outcome.message += " [result=" + result->dump() + "]";
                const auto ret = response.find("ret_code");
                if (ret != response.end() && (ret->is_number() || ret->is_string()))
                    outcome.message += " [ret_code=" + ret->dump() + "]";
            }
            catch (...)
            {
                outcome.message = "宿舍区响应格式异常，未将响应正文写入日志";
                outcome.failure = Auth::Failure::Protocol;
            }
        }
        curl_easy_cleanup(curl);
        curl_slist_free_all(resolves);
        return outcome;
    }
}

Auth::Result Authenticate(const std::string& account, const std::string& password,
                          const Observation::Campus& context,
                          const Operation::Cancelled& cancelled, bool manual)
{
    Auth::Result result;
    if (Operation::IsCancelled(cancelled))
    {
        result.failure = Auth::Failure::Cancelled;
        result.message = Auth::FailureName(result.failure);
        return result;
    }
    if (!Observation::EnsureCurl())
    {
        result.failure = Auth::Failure::Protocol;
        result.message = "网络库初始化失败";
        return result;
    }
    const auto current = Observation::ReadEnvironment();
    if (!context.Confirmed() || current.Key() != context.env.Key() || current.localIp.empty())
    {
        result.failure = Auth::Failure::NotCampus;
        result.message = "未确认当前校园网环境，未提交账号密码";
        return result;
    }
    // Refresh evidence immediately before sending credentials, even after a cached observation.
    const Operation::Cancelled stale = [&] {
        return Operation::IsCancelled(cancelled) ||
            Observation::ReadEnvironment().Key() != current.Key();
    };
    const auto checked = Observation::DetectCampus(current, stale);
    if (stale())
    {
        result.failure = Auth::Failure::Cancelled;
        result.message = Auth::FailureName(result.failure);
        return result;
    }
    if (!checked.Confirmed())
    {
        result.failure = Auth::Failure::NotCampus;
        result.message = checked.evidence;
        return result;
    }
    if (!manual && checked.session == Observation::Session::Online)
    {
        result.alreadyOnline = true;
        result.message = "门户会话已经在线，正在复查外网";
        return result;
    }
    if (checked.portal == Observation::Portal::Dormitory)
        result = LoginDormitory(account, password, checked, stale);
    else
    {
        result.method = "SRun(教学区)";
        result.ok = SrunLogin(account, password, result.message, result.failure, checked, stale);
    }
    if (stale() || result.failure == Auth::Failure::Cancelled)
    {
        result.ok = false;
        result.failure = Auth::Failure::Cancelled;
        result.message = Auth::FailureName(result.failure);
        return result;
    }
    result.message = Sanitize(result.message, account, password);
    const auto detail = result.ok ? result.message
        : std::string(Auth::FailureName(result.failure)) + "；" + result.message;
    WriteAuthLog(result.method, result.ok, current.localIp, detail);
    Status::RecordAuth(result.ok, result.method, current.localIp, result.ok ? "" : detail);
    if (IsFirstBoot() && AppUI::ShouldShowAuthenticationResult())
        AppUI::ShowMessage(detail, "认证结果");
    return result;
}

bool Login(const std::string& account, const std::string& password, std::string& message)
{
    const auto result = Authenticate(account, password,
        Observation::DetectCampus(Observation::ReadEnvironment()));
    message = result.message;
    return result.ok;
}

bool Login(const std::string& account, const std::string& password)
{
    std::string message;
    return Login(account, password, message);
}
