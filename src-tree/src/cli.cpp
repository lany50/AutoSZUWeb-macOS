#include "cli.h"

#include <ostream>
#include <iostream>
#include <algorithm>
#include <array>
#include <cstring>
#ifdef __APPLE__
#include <readpassphrase.h>
#endif

namespace Cli
{
    namespace
    {
        bool IsOption(const std::string& argument)
        {
            return argument.size() >= 2 && argument[0] == '-';
        }

        bool EqualsIgnoreCase(const std::string& text, const char* literal)
        {
            if (text.size() != std::string(literal).size())
                return false;
            for (size_t i = 0; i < text.size(); ++i)
            {
                char c = text[i];
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
                if (c != literal[i])
                    return false;
            }
            return true;
        }

        bool ValidAccount(const std::string& account)
        {
            return !account.empty() && account.size() <= 128 &&
                std::none_of(account.begin(), account.end(), [](unsigned char ch) {
                    return ch <= 0x20 || ch == 0x7f;
                });
        }

        bool ValidPassword(const std::string& password)
        {
            return !password.empty() && password.size() <= 512 &&
                std::none_of(password.begin(), password.end(), [](unsigned char ch) {
                    return ch < 0x20 || ch == 0x7f;
                });
        }

        void ClearBuffer(char* buffer, size_t size)
        {
            volatile char* bytes = buffer;
            while (size--) *bytes++ = 0;
        }
    }

    Parsed Parse(const std::vector<std::string>& args)
    {
        Parsed parsed;
        if (args.empty())
        {
            parsed.action = Action::NormalStartup;
            return parsed;
        }

        if (args.size() == 1)
        {
            if (args[0] == "--uninstall")
                parsed.action = Action::Uninstall;
            else if (args[0] == "--login")
                parsed.action = Action::InteractiveAuth;
            else if (args[0] == "--diagnose")
                parsed.action = Action::Diagnose;
            else if (args[0] == "--diagnose-window")
                parsed.action = Action::DiagnoseWindow;
            else if (args[0] == "--settings")
                parsed.action = Action::SettingsWindow;
            else if (args[0] == "--history")
                parsed.action = Action::HistoryWindow;
            else if (args[0] == "--help" || args[0] == "-h")
                parsed.action = Action::Help;
            else
                parsed.action = Action::UsageError;
            return parsed;
        }

        if (args.size() == 2 && (args[0] == "--login" || args[0] == "--password-stdin"))
        {
            parsed.action = ValidAccount(args[1]) && !IsOption(args[1])
                ? (args[0] == "--login" ? Action::InteractiveAuth : Action::PasswordStdinAuth)
                : Action::UsageError;
            parsed.account = args[1];
            return parsed;
        }

        if (args.size() == 2 && args[0] == "--auto")
        {
            if (EqualsIgnoreCase(args[1], "true"))
            {
                parsed.action = Action::SetAutoAuth;
                parsed.autoAuthEnabled = true;
            }
            else if (EqualsIgnoreCase(args[1], "false"))
            {
                parsed.action = Action::SetAutoAuth;
                parsed.autoAuthEnabled = false;
            }
            else
                parsed.action = Action::UsageError;
            return parsed;
        }

        if (args.size() == 2 && !IsOption(args[0]) && !IsOption(args[1]))
        {
            parsed.action = Action::OneShotAuth;
            parsed.account = args[0];
            parsed.password = args[1];
            return parsed;
        }

        parsed.action = Action::UsageError;
        return parsed;
    }

    bool ReadPasswordLine(std::istream& input, std::string& password, std::string& error)
    {
        error.clear();
        password.clear();
        char ch = 0;
        bool tooLong = false;
        while (input.get(ch) && ch != '\n')
        {
            if (password.size() < 513) password.push_back(ch);
            else tooLong = true;
        }
        if (!password.empty() && password.back() == '\r') password.pop_back();
        if (input.bad() || tooLong || !ValidPassword(password))
        {
            password.clear();
            error = "无法读取有效密码，请提供一行 1～512 字节且不含控制字符的密码";
            return false;
        }
        return true;
    }

    bool ReadLoginCredentials(const Parsed& parsed, std::string& account,
                              std::string& password, std::string& error)
    {
        error.clear();
        account = parsed.account;
        password.clear();
        if (parsed.action == Action::OneShotAuth)
        {
            password = parsed.password;
            return true;
        }
        if (parsed.action == Action::PasswordStdinAuth)
        {
            if (!ValidAccount(account)) { error = "账号格式无效"; return false; }
            return ReadPasswordLine(std::cin, password, error);
        }
        if (parsed.action != Action::InteractiveAuth)
        {
            error = "当前命令无需读取登录凭据";
            return false;
        }
#ifdef __APPLE__
        std::array<char, 4096> buffer{};
        if (account.empty())
        {
            if (!readpassphrase("校园网账号：", buffer.data(), buffer.size(), RPP_ECHO_ON | RPP_REQUIRE_TTY))
            { error = "无法读取终端账号，可使用 --password-stdin 账号"; return false; }
            account = buffer.data();
            ClearBuffer(buffer.data(), buffer.size());
        }
        if (!ValidAccount(account)) { error = "账号格式无效"; return false; }
        const bool read = readpassphrase("校园网密码：", buffer.data(), buffer.size(), RPP_REQUIRE_TTY) != nullptr;
        if (read) password = buffer.data();
        ClearBuffer(buffer.data(), buffer.size());
        if (!read || !ValidPassword(password))
        {
            password.clear();
            error = "无法读取有效终端密码，可使用 --password-stdin 账号";
            return false;
        }
        return true;
#else
        error = "当前平台请使用 --password-stdin 账号读取密码";
        return false;
#endif
    }

    void PrintUsage(std::ostream& output)
    {
        output
            << "用法: AutoSZUWeb [选项]\n"
            << "\n"
            << "  (无参数)        后台常驻模式：首次配置并自动认证、监控网络状态\n"
            << "  --login [卡号]   在终端交互认证一次，密码隐藏输入\n"
            << "  --password-stdin 卡号\n"
            << "                  从标准输入读取一行密码并认证一次\n"
            << "  卡号 密码       兼容旧调用；密码会进入命令参数，建议使用上述输入方式\n"
            << "  --auto true|false\n"
            << "                  开启/关闭后台自动认证（写入 setting.json，\n"
            << "                  通常通过配置通知立即生效；手动认证不受影响）\n"
            << "  --uninstall     从本机完全卸载（自启动、配置、日志、密钥与程序本体）\n"
            << "  --diagnose      只读诊断网络、DNS、校园门户与代理路径，不提交账号密码\n"
            << "  --diagnose-window  在独立窗口显示只读诊断结果\n"
            << "  --settings      打开账号与监测设置，不强制重登现有连接\n"
            << "  --history       打开最近故障与恢复记录\n"
            << "  --help, -h      显示本帮助\n";
    }
}
