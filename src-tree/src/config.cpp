#include "config.h"
#include "json_store.h"

namespace Config
{
    bool ReadAutoAuthEnabled(const std::filesystem::path& path)
    {
        nlohmann::json data;
        if (!JsonStore::Read(path, data) || !data.is_object() ||
            !data.contains("AutoAuth") || !data["AutoAuth"].is_boolean()) return true;
        return data["AutoAuth"].get<bool>();
    }
    bool WriteAutoAuthEnabled(const std::filesystem::path& path, bool enabled, std::string& error)
    {
        return JsonStore::Update(path, false, [&](nlohmann::json& data, std::string& issue) {
            if (!data.is_object()) { issue = "配置格式错误，无法修改"; return false; }
            data["AutoAuth"] = enabled;
            return true;
        }, error);
    }
}
