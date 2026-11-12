#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace Cli
{
    enum class Action
    {
        NormalStartup, // 无参数：后台常驻模式
        OneShotAuth,   // 卡号 密码：手动认证一次后退出
        InteractiveAuth, // --login [卡号]：终端隐藏输入密码
        PasswordStdinAuth, // --password-stdin 卡号：从标准输入读取密码
        SetAutoAuth,   // --auto true|false：修改后台自动认证开关
        Diagnose,      // --diagnose：只读诊断，不读取凭据
        DiagnoseWindow,// --diagnose-window：只读诊断窗口
        SettingsWindow,// --settings：账号与监测设置
        HistoryWindow, // --history：最近故障与恢复
        Uninstall,     // --uninstall：从本机完全卸载
        Help,          // --help / -h
        UsageError     // 未知或多余参数
    };

    struct Parsed
    {
        Action action = Action::NormalStartup;
        std::string account;
        std::string password;
        bool autoAuthEnabled = false;
    };

    // 解析不含程序名的命令行参数（UTF-8）。纯函数，无副作用。
    Parsed Parse(const std::vector<std::string>& args);

    // Read one bounded password line. Exposed separately for stream-based tests.
    bool ReadPasswordLine(std::istream& input, std::string& password, std::string& error);

    // Only the explicit login commands read credentials. Interactive mode requires
    // a controlling terminal and restores terminal echo through readpassphrase.
    bool ReadLoginCredentials(const Parsed& parsed, std::string& account,
                              std::string& password, std::string& error);

    void PrintUsage(std::ostream& output);
}
