#include "app_ui.h"
#include "cli.h"
#include "config.h"
#include "file_path.h"
#include "network.h"
#include "network_observation.h"
#include "monitor_policy.h"
#include "monitor_cycle.h"
#include "monitor_schedule.h"
#include "wake_signal.h"
#include "settings.h"
#include "json_store.h"
#include "event_history.h"
#include "app_windows.h"
#include "app_version.h"
#include <regex>
#include "runtime.h"
#include "single_instance.h"
#include "status.h"
#include "auth_attempt.h"
#include <sys/stat.h>

#include <cstdio>

#ifdef __APPLE__
#include "notify.h"
#endif
#include "sys.h"
#include "uninstall.h"
#include "web.h"

#ifdef __APPLE__
#include "statusbar.h"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace
{
    bool FirstBoot = true;

    struct FileRevision
    {
        ino_t inode = 0;
        off_t size = 0;
        timespec modified{};
        explicit FileRevision(const fs::path& path)
        {
            struct stat info{};
            if (lstat(path.c_str(), &info) == 0)
            {
                inode = info.st_ino;
                size = info.st_size;
                modified = info.st_mtimespec;
            }
        }
        bool Matches(const fs::path& path) const
        {
            const FileRevision current(path);
            return inode == current.inode && size == current.size &&
                modified.tv_sec == current.modified.tv_sec && modified.tv_nsec == current.modified.tv_nsec;
        }
    };

    Observation::InternetResult Probe(const Operation::Cancelled& cancelled, const Observation::Environment& env)
    {
        Status::SetMonitoring(0, Status::Get().powerSaving, true);
        Observation::Campus network;
        network.env = env;
        const auto result = Observation::CheckInternet(nullptr, 3000, false, &network, cancelled);
        if (!result.cancelled && !(cancelled && cancelled())) Status::SetLatency(result.latencyMs);
        return result;
    }

    struct VerifiedAuthentication
    {
        Auth::Result auth;
        Observation::InternetResult current;
        Observation::InternetResult direct;
    };

    VerifiedAuthentication RunVerifiedAuth(const std::string& account, const std::string& password,
        const Observation::Campus& context, bool manual, const Operation::Cancelled& cancelled)
    {
        VerifiedAuthentication result;
        Auth::Attempt attempt(GetConfigPath().parent_path());
        if (!attempt.Acquired())
        {
            result.auth.failure = Auth::Failure::Busy;
            result.auth.message = Auth::FailureName(result.auth.failure);
            return result;
        }
        if (cancelled && cancelled())
        {
            result.auth.failure = Auth::Failure::Cancelled;
            return result;
        }
        Status::SetState(Status::State::Authenticating, "正在通过已识别的校园门户认证…");
        Status::SetRetry(manual ? "手动认证中" : "自动认证中");
        History::Record("auth-start", manual ? "手动认证已开始" : "自动认证已开始", "正在重新确认当前深大门户");
        result.auth = Authenticate(account, password, context, cancelled, manual);
        if ((result.auth.ok || result.auth.alreadyOnline) && !(cancelled && cancelled()))
        {
            result.current = Probe(cancelled, context.env);
            if (result.current.state != Observation::Internet::Online && !(cancelled && cancelled()))
                result.direct = Observation::CheckInternet(&context.env, 3000, false, &context, cancelled);
        }
        return result;
    }

    [[noreturn]] void ExitWithMessage(const std::string& message)
    {
        AppUI::ShowMessage(message, "提示");
        std::exit(1);
    }

    // 手动认证模式：认证一次后退出，不写配置、不注册自启动、不进入监控循环。
    int RunOneShotAuth(const std::string& account, const std::string& password)
    {
        FirstBoot = false; // 抑制首启认证弹窗，改用控制台输出。
        const auto generation = WakeSignal::Generation();
        const auto env = Observation::ReadEnvironment();
        const Operation::Cancelled cancelled = [&] {
            return generation != WakeSignal::Generation() || env.Key() != Observation::ReadEnvironment().Key();
        };
        const auto context = Observation::DetectCampus(env, cancelled);
        const auto result = RunVerifiedAuth(account, password, context, true, cancelled);
        const bool accepted = result.auth.ok && !cancelled();
        std::string message = std::string(accepted ? "认证接口已接受请求" : "认证未完成") + "；" + result.auth.message;
        if (accepted) message += "\n默认路由外网复查：" + result.current.summary +
            (result.current.state == Observation::Internet::Online ? "" : "\n物理直连外网复查：" + result.direct.summary);
        if (PrepareCliConsole())
            std::cout << message << '\n';
        else
            AppUI::ShowMessage(message, "认证结果");
        if (result.auth.failure == Auth::Failure::Busy) return 75;
        return accepted ? 0 : 1;
    }

    // 修改 setting.json 的后台自动认证开关（跳过单实例守卫，可与常驻实例并存）。
    // 运行中的常驻实例在每个监控周期重新读取该字段，约 10 秒内应用。
    int RunSetAutoAuth(bool enabled)
    {
        const fs::path configPath = GetConfigPath();
        if (configPath.empty())
        {
            AppUI::ShowMessage("无法确定配置文件路径。");
            return 1;
        }
        if (!fs::exists(configPath) || !fs::is_regular_file(configPath))
        {
            const std::string message = "尚未完成首次配置，请先启动程序完成配置。";
            if (PrepareCliConsole())
                std::cerr << message << '\n';
            else
                AppUI::ShowMessage(message);
            return 1;
        }

        std::string error;
        if (!Config::WriteAutoAuthEnabled(configPath, enabled, error))
        {
            const std::string message = "修改配置失败：" + error;
            if (PrepareCliConsole())
                std::cerr << message << '\n';
            else
                AppUI::ShowMessage(message);
            return 1;
        }

        // 探测常驻实例是否在运行，用于提示生效时机。
        bool residentRunning = false;
        {
            Runtime::SingleInstanceGuard probe(
                configPath.parent_path() / "AutoSZUWeb.instance.lock");
            residentRunning = !probe.IsPrimary();
        }

        std::string report =
            enabled ? "已开启后台自动认证。" : "已关闭后台自动认证。";
        if (residentRunning)
            report += "\n运行中的实例会通过配置变更通知应用设置；监听不可用时在下一监测周期应用。";
        if (PrepareCliConsole())
            std::cout << report << '\n';
        else
            AppUI::ShowMessage(report);
        return 0;
    }

    // 卸载模式：确认后移除自启动、共享数据与平台痕迹（含程序本体）。
    int RunUninstall()
    {
        if (AppUI::ShowMessage(
                "确定要从本机完全卸载 AutoSZUWeb 吗？\n\n"
                "将删除：开机自启动设置、配置文件与日志、凭据密钥以及程序本体。",
                "卸载确认", true) == AppUI::Button::Cancel)
            return 0;

        std::string error;
        const bool ok = Uninstall::RemoveAll(error);
        const std::string report =
            ok ? std::string("卸载完成。") : "卸载完成，但" + error;
        if (PrepareCliConsole())
            std::cout << report << '\n';
        else
            AppUI::ShowMessage(report, "卸载");
        return ok ? 0 : 1;
    }

    void RunMainLoop()
    {
        const fs::path loginPath = GetConfigPath();
        if (loginPath.empty()) ExitWithMessage("无法确定配置文件路径。");
        SetAutoStart();
        FirstBoot = false;
        History::Record("startup", "程序已启动", std::string("版本 ") + AppVersion::String);
        Auth::RetryPolicy retry;
        Monitor::AdaptiveSchedule schedule;
        Monitor::UncertainSchedule uncertainSchedule;
        Settings::Preferences preferences;
        Observation::Campus context;
        std::string knownNetwork, storedAccount, storedPassword, account, password, lastFailure;
        bool refreshCampus = true;
        std::time_t lastFailNotify = 0;
        bool startup = true, configBad = false, sawOnline = false, observedOffline = false, pathProblem = false;
        uint32_t signals = WakeSignal::Take();
        int lastInterval = -1;
        const auto clock = []() -> long long {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        const auto wait = [&](int seconds) {
            Status::SetMonitoring(seconds, preferences.powerSaving);
            if (lastInterval != seconds)
            {
                std::fprintf(stdout, "[monitor] next=%ds powerSaving=%d probes=%u\n",
                    seconds, preferences.powerSaving ? 1 : 0, Status::Get().probeCount);
                std::fflush(stdout); lastInterval = seconds;
            }
            signals = WakeSignal::Wait(std::chrono::seconds(seconds));
        };
        const auto retryText = [&] {
            return retry.blocked ? std::string("已暂停自动尝试；处理账号后点“立即认证”重试")
                : std::to_string(retry.Remaining(clock())) + " 秒后重试（" + Auth::FailureName(retry.failure) + "）";
        };
        while (true)
        {
            signals |= WakeSignal::Take();
            const auto generation = WakeSignal::Generation();
            const FileRevision configRevision(loginPath);
            const Operation::Cancelled cancelled = [&] {
                return generation != WakeSignal::Generation() || !configRevision.Matches(loginPath);
            };
            if (signals & (WakeSignal::Network | WakeSignal::Wake))
            {
                schedule.Reset(); uncertainSchedule.Reset(); refreshCampus = true;
                if (signals & WakeSignal::Wake) History::Record("wake", "电脑已唤醒", "立即检查连接，未修改代理或网络设置");
                else History::Record("network", "网络环境发生变化", "立即检查连接，未修改代理或网络设置");
            }
            if (Status::ConsumeDiagnosticRequest())
            {
                try { Status::CompleteDiagnostic(Observation::Diagnose()); }
                catch (...) { Status::CompleteDiagnostic("诊断未完成，未修改网络或代理设置。请稍后重试。"); }
            }
            const bool manual = Status::ConsumeManualAuthRequest();
            nlohmann::json data;
            if (!JsonStore::Read(loginPath, data) || !data.is_object() ||
                !data.contains("Account") || !data["Account"].is_string() ||
                !data.contains("Password") || !data["Password"].is_string())
            {
                const bool exists = fs::exists(loginPath);
                Status::SetState(exists ? Status::State::Attention : Status::State::Paused,
                    exists ? "配置不可读，已停止使用旧凭据；请打开账号设置" : "请从菜单栏打开账号与监测设置，完成首次配置");
                Status::SetRetry("未提交账号密码"); schedule.Reset();
                if (!configBad) History::Record("config", "等待有效账号配置", "未向网络提交凭据");
                configBad = true; wait(60); continue;
            }
            const auto nextAccount = data["Account"].get<std::string>();
            const auto nextPassword = data["Password"].get<std::string>();
            if (manual || nextAccount != storedAccount || nextPassword != storedPassword || configBad)
            {
                auto user = DecryptStr(nextAccount), pass = DecryptStr(nextPassword);
                if (user.empty() || pass.empty())
                {
                    Status::SetState(Status::State::Attention, "无法解密当前配置，已停止使用旧凭据");
                    Status::SetRetry("请检查账号设置；未提交账号密码");
                    if (!configBad) History::Record("config", "账号配置无法解密", "未使用旧凭据重试");
                    configBad = true; wait(60); continue;
                }
                const bool changed = !storedAccount.empty() && (nextAccount != storedAccount || nextPassword != storedPassword);
                account = std::move(user); password = std::move(pass);
                storedAccount = nextAccount; storedPassword = nextPassword; configBad = false;
                if (changed || manual) { retry.Reset(); uncertainSchedule.Reset(); }
                if (changed) History::Record("settings-applied", "新账号配置已应用", "当前正常连接不会被强制重登");
            }
            const auto nextPreferences = Settings::ReadPreferences(loginPath);
            if (preferences.powerSaving != nextPreferences.powerSaving || preferences.stableInterval != nextPreferences.stableInterval)
                schedule.Reset();
            preferences = nextPreferences;
            const bool enabled = preferences.autoAuth;
            Status::SetAutoAuth(enabled);
            if (signals & WakeSignal::Settings)
                History::Record("settings-applied", "监测设置已应用", preferences.powerSaving ? "稳定时逐步降低频率，换网与唤醒立即检查" : "每 10 秒检查，换网与唤醒立即检查");
            const auto env = Observation::ReadEnvironment();
            if (knownNetwork != env.Key())
            {
                if (!knownNetwork.empty()) History::Record("network", "网络出口或配置已变化", "旧门户识别结果已作废");
                knownNetwork = env.Key(); retry.NetworkChanged(); schedule.Reset(); uncertainSchedule.Reset(); refreshCampus = true;
            }
            if (!enabled && !manual)
            {
                Status::SetNetwork(context.env.Key() == env.Key() ? Observation::PortalName(context.portal) : "自动认证已关闭", env.localIp);
                Status::SetRetry("自动认证已关闭；仍可诊断或手动认证");
                Status::SetState(Status::State::Paused, "自动认证已关闭"); wait(60); continue;
            }
            const auto cycle = Monitor::Observe(enabled, manual, refreshCampus, context, retry, {
                [&] { return Probe(cancelled, env); },
                [&] { return Observation::DetectCampus(env, cancelled); },
                [&](const Observation::Campus& campus) {
                    return Observation::CheckInternet(&env, 3000, false, &campus, cancelled);
                }, clock, cancelled
            });
            if (cycle.cancelled || env.Key() != Observation::ReadEnvironment().Key())
            {
                refreshCampus = true;
                continue;
            }
            context = cycle.campus;
            refreshCampus = false;
            const auto& current = cycle.current;
            const auto& direct = cycle.direct;
            const bool currentOnline = current.state == Observation::Internet::Online;
            Status::SetNetwork(Observation::PortalName(context.portal), env.localIp);
            if (startup)
            {
                std::fprintf(stdout, "[diag] v=%s external_online=%d campus=%s latency=%dms\n",
                    AppVersion::String, currentOnline ? 1 : 0, Observation::PortalName(context.portal), Status::Get().latencyMs);
                std::fflush(stdout); startup = false;
                History::Record("connectivity", currentOnline ? "启动检查：外网可用" : "启动检查：外网未通过",
                    "探测结果不等同于账号认证结果");
            }
            if (!currentOnline)
            {
                schedule.Reset();
                if (sawOnline && !observedOffline) History::Record("offline", "检测到外网连接中断", "开始检查门户和当前网络路径");
                observedOffline = true;
            }
            else
            {
                if (observedOffline) History::Record("recovered", "外网检查已恢复", "默认路径 HTTP 204 验证通过");
                sawOnline = true; observedOffline = false; pathProblem = false;
            }
            const auto action = cycle.action;
            if (action == Monitor::Action::Healthy)
            {
                uncertainSchedule.Reset();
                if (!retry.blocked) retry.Reset();
                Status::SetRetry(retry.blocked ? retryText() : "外网已可用，无需重复认证");
                Status::SetState(Status::State::Online, retry.blocked ? lastFailure :
                    (context.Confirmed() ? "" : "当前网络可用；校园网自动认证待命"));
                wait(schedule.Healthy(preferences.powerSaving, preferences.stableInterval)); continue;
            }
            if (action == Monitor::Action::Waiting)
            {
                Status::SetRetry("待命：未提交账号密码");
                Status::SetState(Status::State::Waiting, context.evidence); wait(30); continue;
            }
            if (action == Monitor::Action::PathProblem)
            {
                uncertainSchedule.Reset();
                Status::SetRetry("物理直连可用，已避免重复认证");
                Status::SetState(Status::State::Attention, "物理直连可用，默认路由探测异常；可运行诊断检查出口");
                if (!pathProblem) History::Record("path", "默认路由异常，物理直连可用", "可运行诊断检查出口路径");
                pathProblem = true; wait(30); continue;
            }
            if (action == Monitor::Action::NeedUser || action == Monitor::Action::Cooling)
            {
                Status::SetRetry(retryText());
                Status::SetState(retry.blocked ? Status::State::Attention : Status::State::Offline, lastFailure);
                wait(static_cast<int>(retry.blocked ? 30 : std::max(1LL, std::min(30LL, retry.Remaining(clock()))))); continue;
            }
            if (action == Monitor::Action::Uncertain)
            {
                const int delay = uncertainSchedule.Next();
                const auto reason = direct.dnsFailures > 0 ? "物理网络 DNS 或外网探测未通过" :
                    (context.session == Observation::Session::Online ? "门户显示在线，外网探测未通过" : "外网检测失败，认证状态仍不明确");
                Status::SetState(Status::State::Attention, reason);
                Status::SetRetry(std::to_string(delay) + " 秒后复查；可运行诊断或手动恢复");
                if (delay == 2) History::Record("uncertain", "等待确认故障原因", reason);
                wait(delay); continue;
            }
            const auto verified = RunVerifiedAuth(account, password, context, manual, cancelled);
            const auto& result = verified.auth;
            if (cancelled() || result.failure == Auth::Failure::Cancelled ||
                context.env.Key() != Observation::ReadEnvironment().Key())
            {
                refreshCampus = true;
                Status::SetState(Status::State::Waiting, "网络或配置已变化，正在重新检查");
                Status::SetRetry("上一操作已结束");
                continue;
            }
            if (result.failure == Auth::Failure::Busy)
            {
                Status::SetState(Status::State::Waiting, result.message);
                Status::SetRetry("等待正在进行的认证完成");
                wait(2); continue;
            }
            if (result.alreadyOnline)
            {
                if (verified.current.state == Observation::Internet::Online)
                {
                    uncertainSchedule.Reset(); retry.Reset(); sawOnline = true; observedOffline = false; pathProblem = false;
                    Status::SetState(Status::State::Online, "已有校园会话，外网复查通过");
                    Status::SetRetry("连接已恢复");
                    History::Record("recovered", "已有会话的外网检查通过", "默认路由 HTTP 204 验证通过");
                    wait(schedule.Healthy(preferences.powerSaving, preferences.stableInterval));
                }
                else
                {
                    Status::SetState(Status::State::Attention, "门户显示会话在线，外网仍需检查");
                    Status::SetRetry("可运行诊断检查 DNS 和出口路径");
                    wait(uncertainSchedule.Next());
                }
                continue;
            }
            if (result.failure == Auth::Failure::NotCampus)
            {
                refreshCampus = true; Status::SetRetry("待命：未提交账号密码");
                Status::SetState(Status::State::Waiting, result.message);
                History::Record("auth-blocked", "校园门户未能再次确认", "已阻止提交账号密码"); wait(30); continue;
            }
            if (result.ok)
            {
                History::Record("auth-accepted", "认证接口已接受请求", "仍需验证外网，暂不记为已恢复");
                if (verified.current.state == Observation::Internet::Online)
                {
                    uncertainSchedule.Reset();
                    retry.Reset(); sawOnline = true; observedOffline = false; pathProblem = false;
                    Status::SetRetry("认证接口已接受；外网复查通过"); Status::SetState(Status::State::Online);
                    History::Record("recovered", "认证后外网检查通过", "连接已恢复");
#ifdef __APPLE__
                    Notify::Post("校园网连接已恢复", "认证接口已接受，外网检查通过", false);
#endif
                    wait(schedule.Healthy(preferences.powerSaving, preferences.stableInterval)); continue;
                }
                if (verified.direct.state == Observation::Internet::Online)
                {
                    retry.Reset(); Status::SetRetry("已停止重复认证，请检查代理路径");
                    Status::SetState(Status::State::Attention, "认证后直连可用，当前路径异常");
                    History::Record("path", "认证后直连可用，当前路径异常", "已停止重复认证"); wait(30); continue;
                }
                retry.Fail(Auth::Failure::Transport, clock()); retry.nextAttempt = std::max(retry.nextAttempt, clock() + 60);
                lastFailure = "认证接口已接受，但外网尚未通过；至少等待 60 秒，避免反复重登";
                WriteAuthLog("复查", false, "", lastFailure);
                History::Record("verify-failed", "认证后外网复查未通过", "至少等待 60 秒，不将接口接受记为恢复");
            }
            else
            {
                retry.Fail(result.failure, clock());
                lastFailure = std::string(Auth::FailureName(result.failure)) + "；" + result.message;
                std::smatch match;
                const std::regex pattern("[Ee][0-9]{4}");
                const std::string code = std::regex_search(result.message, match, pattern) ? "；错误码 " + match.str() : "";
                History::Record("auth-failed", "认证未完成", std::string(Auth::FailureName(result.failure)) + code + "；" + retryText());
            }
            Status::SetRetry(retryText());
            Status::SetState(retry.blocked ? Status::State::Attention : Status::State::Offline, lastFailure);
            WriteAuthLog("重试策略", false, "", std::string(Auth::FailureName(retry.failure)) + "；" + retryText());
#ifdef __APPLE__
            const auto now = std::time(nullptr);
            if (now - lastFailNotify >= 300)
            {
                lastFailNotify = now;
                Notify::Post(retry.blocked ? "校园网认证需要处理" : "校园网认证暂未完成",
                    retry.blocked ? "已暂停自动尝试，可打开账号设置或查看故障记录" : "正在等待重试，可运行一键诊断", retry.blocked);
            }
#endif
            wait(1);
        }
    }

    void StartResident()
    {
#ifdef __APPLE__
        // 主线程跑菜单栏事件循环，认证循环放后台线程。
        Notify::Setup();
        if (StatusBar::Install())
        {
            const bool watching = NativeEvents::Start();
            std::fprintf(stdout, "[events] native=%d\n", watching ? 1 : 0); std::fflush(stdout);
            std::thread([] { RunMainLoop(); }).detach();
            if (!fs::is_regular_file(GetConfigPath())) AppWindows::ShowSettings();
            StatusBar::Run();
            return;
        }
#endif
        RunMainLoop();
    }


}

// 供 web.cpp 控制首次认证弹窗。
bool IsFirstBoot()
{
    return FirstBoot;
}

int main(int argc, char* argv[])
{
    const Cli::Parsed parsed = Cli::Parse(GetUtf8Arguments(argc, argv));

    if (parsed.action == Cli::Action::Help)
    {
        if (PrepareCliConsole())
            Cli::PrintUsage(std::cout);
        return 0;
    }
    if (parsed.action == Cli::Action::UsageError)
    {
        if (PrepareCliConsole())
        {
            std::cerr << "参数无效。\n\n";
            Cli::PrintUsage(std::cerr);
        }
        return 2;
    }
    if (parsed.action == Cli::Action::Uninstall)
        return RunUninstall();
    if (parsed.action == Cli::Action::Diagnose)
    {
        std::cout << Observation::Diagnose();
        return 0;
    }
    if (parsed.action == Cli::Action::DiagnoseWindow)
    {
#ifdef __APPLE__
        StatusBar::ShowDiagnosticWindow();
#else
        std::cout << Observation::Diagnose();
#endif
        return 0;
    }
    Runtime::LifecycleGuard lifecycle(GetUsersFolderPath(), Runtime::LifecycleAccess::Shared);
    if (!lifecycle.IsLocked())
    {
        if (PrepareCliConsole()) std::cerr << lifecycle.Error() << '\n';
        else AppUI::ShowMessage(lifecycle.Error());
        return 3;
    }
    if (parsed.action == Cli::Action::SettingsWindow || parsed.action == Cli::Action::HistoryWindow)
    {
#ifdef __APPLE__
        if (parsed.action == Cli::Action::SettingsWindow) AppWindows::ShowSettings(true);
        else AppWindows::ShowHistory(true);
#endif
        return 0;
    }
    if (parsed.action == Cli::Action::OneShotAuth || parsed.action == Cli::Action::InteractiveAuth ||
        parsed.action == Cli::Action::PasswordStdinAuth)
    {
        std::string account, password, error;
        if (!Cli::ReadLoginCredentials(parsed, account, password, error))
        {
            std::cerr << error << '\n';
            return 2;
        }
        if (!JsonStore::SecureDataDirectory(GetConfigPath(), error))
        {
            std::cerr << error << '\n';
            return 1;
        }
        return RunOneShotAuth(account, password);
    }
    if (parsed.action == Cli::Action::SetAutoAuth)
        return RunSetAutoAuth(parsed.autoAuthEnabled);

    const fs::path configPath = GetConfigPath();
    if (configPath.empty())
        ExitWithMessage("无法确定配置文件路径。");
    std::string permissionError;
    if (!JsonStore::SecureDataDirectory(configPath, permissionError))
        ExitWithMessage(permissionError);
    Runtime::SingleInstanceGuard instanceGuard(
        configPath.parent_path() / "AutoSZUWeb.instance.lock");
    if (!instanceGuard.IsPrimary())
        return 0;

    StartResident();
    return 0;
}
