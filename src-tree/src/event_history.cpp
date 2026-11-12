#include "event_history.h"
#include "json_store.h"
#include "file_path.h"
#include <algorithm>
#include <ctime>
#include <sstream>
namespace History
{
    namespace
    {
        std::string Bounded(std::string text)
        {
            for (auto& ch : text) if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
            if (text.size() > 600)
            {
                size_t end = 600;
                while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
                text.resize(end);
            }
            return text;
        }
    }
    std::filesystem::path DefaultPath() { return GetConfigPath().parent_path() / "events.json"; }
    bool Add(const std::filesystem::path& path, const std::string& kind,
             const std::string& title, const std::string& detail)
    {
        std::string error;
        return JsonStore::Update(path, true, [&](nlohmann::json& data, std::string& issue) {
            if (!data.is_object()) { issue = "记录格式异常"; return false; }
            auto& events = data["events"];
            if (events.is_null()) events = nlohmann::json::array();
            if (!events.is_array()) { issue = "记录格式异常"; return false; }
            const auto now = static_cast<long long>(std::time(nullptr));
            if (!events.empty())
            {
                const auto& last = events.back();
                try
                {
                    if (last.is_object() && last.value("kind", "") == kind && last.value("title", "") == title &&
                        last.value("detail", "") == detail && now - last.value("time", 0LL) < 60) return true;
                }
                catch (...) { /* Skip malformed historical rows; retain the original data. */ }
            }
            events.push_back({{"time", now}, {"kind", Bounded(kind)}, {"title", Bounded(title)}, {"detail", Bounded(detail)}});
            while (events.size() > 200) events.erase(events.begin());
            data["version"] = 1;
            return true;
        }, error);
    }
    std::vector<Event> Read(const std::filesystem::path& path)
    {
        nlohmann::json data;
        std::vector<Event> result;
        if (!JsonStore::Read(path, data) || !data.is_object() || !data.contains("events") || !data["events"].is_array()) return result;
        for (const auto& row : data["events"])
            try
            {
                if (!row.is_object() || !row.at("time").is_number_integer() || !row.at("kind").is_string() ||
                    !row.at("title").is_string() || !row.at("detail").is_string()) continue;
                result.push_back({row["time"].get<long long>(), row["kind"].get<std::string>(),
                    Bounded(row["title"].get<std::string>()), Bounded(row["detail"].get<std::string>())});
            }
            catch (...) {}
        if (result.size() > 200) result.erase(result.begin(), result.end() - 200);
        std::reverse(result.begin(), result.end());
        return result;
    }
    std::string Text(const std::filesystem::path& path)
    {
        const auto events = Read(path);
        if (events.empty()) return "最近故障与恢复记录\n\n尚无可显示的记录。这里只记录新版本实际观察到的事件，不会将旧日志中的“成功”误报导入为已恢复。";
        std::ostringstream text;
        text << "最近故障与恢复记录\n最多保留 200 条，最新在前；不会记录账号密码或原始认证响应。\n\n";
        for (const auto& event : events)
        {
            const std::time_t time = static_cast<std::time_t>(event.timestamp);
            std::tm local{}; localtime_r(&time, &local);
            char stamp[40]; std::strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S", &local);
            text << stamp << "  " << event.title << '\n';
            if (!event.detail.empty()) text << "  " << event.detail << '\n';
            text << '\n';
        }
        return text.str();
    }
    void Record(const std::string& kind, const std::string& title, const std::string& detail)
    { Add(DefaultPath(), kind, title, detail); }
}
