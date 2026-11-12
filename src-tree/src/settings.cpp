#include "settings.h"
#include "json_store.h"
#include "sys.h"
#include <algorithm>

namespace Settings
{
    namespace
    {
        Preferences PreferencesFrom(const nlohmann::json& data)
        {
            Preferences p;
            if (!data.is_object()) return p;
            if (data.contains("AutoAuth") && data["AutoAuth"].is_boolean()) p.autoAuth = data["AutoAuth"];
            if (data.contains("PowerSaving") && data["PowerSaving"].is_boolean()) p.powerSaving = data["PowerSaving"];
            if (data.contains("StableInterval") && data["StableInterval"].is_number_integer())
            {
                const auto& value = data["StableInterval"];
                if (value == 30 || value == 60 || value == 120) p.stableInterval = value.get<int>();
            }
            return p;
        }
        bool HasCredentials(const nlohmann::json& data)
        {
            return data.is_object() && data.contains("Account") && data["Account"].is_string() &&
                !data["Account"].get_ref<const std::string&>().empty() &&
                data.contains("Password") && data["Password"].is_string() &&
                !data["Password"].get_ref<const std::string&>().empty();
        }
    }
    Preferences ReadPreferences(const std::filesystem::path& path)
    {
        nlohmann::json data;
        return JsonStore::Read(path, data) ? PreferencesFrom(data) : Preferences{};
    }
    Summary ReadSummary(const std::filesystem::path& path)
    {
        Summary result;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) && !ec) return result;
        nlohmann::json data;
        if (!JsonStore::Read(path, data) || !data.is_object())
        { result.valid = false; result.accountHint = "配置不可读，原文件不会被覆盖"; return result; }
        result.preferences = PreferencesFrom(data);
        result.hasCredentials = HasCredentials(data);
        if (result.hasCredentials)
        {
            const auto account = DecryptStr(data["Account"].get<std::string>());
            result.accountHint = account.empty() ? "已配置（当前无法解密）"
                : "已配置 · 尾号 " + account.substr(account.size() > 4 ? account.size() - 4 : 0);
        }
        return result;
    }
    bool Save(const std::filesystem::path& path, const std::string& rawAccount,
              const std::string& password, const std::string& confirmation,
              const Preferences& preferences, std::string& error)
    {
        PreferencesPatch patch;
        patch.autoAuth = preferences.autoAuth;
        patch.powerSaving = preferences.powerSaving;
        patch.stableInterval = preferences.stableInterval;
        return SavePatch(path, rawAccount, password, confirmation, patch, error);
    }
    bool SavePatch(const std::filesystem::path& path, const std::string& rawAccount,
                   const std::string& password, const std::string& confirmation,
                   const PreferencesPatch& preferences, std::string& error)
    {
        auto account = rawAccount;
        const auto begin = account.find_first_not_of(" \t\r\n");
        account = begin == std::string::npos ? "" : account.substr(begin, account.find_last_not_of(" \t\r\n") - begin + 1);
        if (account.size() > 128 || password.size() > 512 ||
            std::any_of(account.begin(), account.end(), [](unsigned char ch) { return ch <= 0x20 || ch == 0x7f; }) ||
            std::any_of(password.begin(), password.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }))
        { error = "账号或密码包含不支持的字符，或长度过长"; return false; }
        if (password != confirmation) { error = "两次密码输入不一致"; return false; }
        if (preferences.stableInterval && *preferences.stableInterval != 30 &&
            *preferences.stableInterval != 60 && *preferences.stableInterval != 120)
        { error = "请选择有效的稳定检查间隔"; return false; }
        std::string encryptedAccount, encryptedPassword;
        if (!account.empty()) encryptedAccount = EncryptStr(account);
        if (!password.empty()) encryptedPassword = EncryptStr(password);
        if ((!account.empty() && encryptedAccount.empty()) || (!password.empty() && encryptedPassword.empty()))
        { error = "本机凭据加密失败，未保存更改"; return false; }
        return JsonStore::Update(path, true, [&](nlohmann::json& data, std::string& issue) {
            if (!data.is_object()) { issue = "配置格式异常，已保留原文件"; return false; }
            if (!encryptedAccount.empty()) data["Account"] = encryptedAccount;
            if (!encryptedPassword.empty()) data["Password"] = encryptedPassword;
            if (!HasCredentials(data)) { issue = "首次配置需要同时填写账号和密码"; return false; }
            if (preferences.autoAuth) data["AutoAuth"] = *preferences.autoAuth;
            if (preferences.powerSaving) data["PowerSaving"] = *preferences.powerSaving;
            if (preferences.stableInterval) data["StableInterval"] = *preferences.stableInterval;
            return true;
        }, error);
    }
}
