#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace History
{
    struct Event { long long timestamp = 0; std::string kind, title, detail; };
    bool Add(const std::filesystem::path& path, const std::string& kind,
             const std::string& title, const std::string& detail = {});
    std::vector<Event> Read(const std::filesystem::path& path);
    std::string Text(const std::filesystem::path& path);
    std::filesystem::path DefaultPath();
    // Producers pass controlled summaries, never raw responses or credential values.
    void Record(const std::string& kind, const std::string& title, const std::string& detail = {});
}
