#pragma once
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace JsonStore
{
    // Repair owner-only permissions without reading credential contents. Reject
    // symlinks, non-regular files, and entries owned by another user.
    bool SecureDataDirectory(const std::filesystem::path& configPath, std::string& error);

    // Replace a private file atomically. The caller owns any higher-level locking.
    bool AtomicReplacePrivateFile(const std::filesystem::path& path,
                                  const std::string& bytes, std::string& error);

    bool Read(const std::filesystem::path& path, nlohmann::json& data);
    // Lock across processes, merge current data, and replace atomically with mode 0600.
    bool Update(const std::filesystem::path& path, bool create,
                const std::function<bool(nlohmann::json&, std::string&)>& edit,
                std::string& error);
}
