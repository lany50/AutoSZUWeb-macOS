#pragma once
#include <filesystem>
#include <string>
#include <optional>
namespace Settings
{
    struct Preferences
    {
        bool autoAuth = true;
        bool powerSaving = true;
        int stableInterval = 60;
    };
    struct Summary
    {
        Preferences preferences;
        bool hasCredentials = false;
        bool valid = true;
        std::string accountHint = "尚未配置";
    };
    struct PreferencesPatch
    {
        std::optional<bool> autoAuth;
        std::optional<bool> powerSaving;
        std::optional<int> stableInterval;
    };
    Summary ReadSummary(const std::filesystem::path& path);
    Preferences ReadPreferences(const std::filesystem::path& path);
    // Blank fields preserve existing ciphertext; no network requests are made.
    bool Save(const std::filesystem::path& path, const std::string& account,
              const std::string& password, const std::string& confirmation,
              const Preferences& preferences, std::string& error);
    bool SavePatch(const std::filesystem::path& path, const std::string& account,
                   const std::string& password, const std::string& confirmation,
                   const PreferencesPatch& preferences, std::string& error);
}
