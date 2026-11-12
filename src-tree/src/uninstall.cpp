#include "uninstall.h"

#include "file_path.h"
#include "sys.h"
#include "user_data.h"
#include "single_instance.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
    void RemoveEntryBestEffort(const fs::path& path,
                               std::vector<fs::path>& leftovers)
    {
        std::error_code error;
        fs::remove_all(path, error);
        if (error || fs::exists(path, error))
            leftovers.push_back(path);
    }
}

namespace Uninstall
{
    void RemoveSharedData(std::vector<fs::path>& leftovers)
    {
        // 数据目录：setting.json、logs/、实例锁文件以及可能残留的自启动副本。
        // 逐项删除以便把被占用的文件（如正在运行的 exe 副本）单独记入
        // leftovers，交由平台善后。
        const fs::path dataRoot = GetUsersFolderPath();
        if (!dataRoot.empty())
        {
            const fs::path dataDirectory = dataRoot / "AutoSZUWeb";
            std::error_code error;
            const auto type = fs::symlink_status(dataDirectory, error).type();
            if (type == fs::file_type::symlink ||
                (type != fs::file_type::not_found && type != fs::file_type::directory))
            {
                leftovers.push_back(dataDirectory);
                return;
            }
#ifndef _WIN32
            struct stat info{};
            if (type == fs::file_type::directory &&
                (lstat(dataDirectory.c_str(), &info) != 0 || info.st_uid != geteuid()))
            {
                leftovers.push_back(dataDirectory);
                return;
            }
#endif
            if (fs::exists(dataDirectory, error))
            {
                std::error_code iterationError;
                for (const auto& entry :
                     fs::directory_iterator(dataDirectory, iterationError))
                    RemoveEntryBestEffort(entry.path(), leftovers);
                if (iterationError)
                    leftovers.push_back(dataDirectory);
                else
                    RemoveEntryBestEffort(dataDirectory, leftovers);
            }
        }

        // A common desktop filename alone does not establish application ownership.
        const fs::path desktop = GetDesktopPath();
        if (!desktop.empty())
        {
            std::string error;
            if (!UserData::RemoveOwnedFile(desktop / "userdata.txt", error))
                leftovers.push_back(desktop / "userdata.txt");
        }
    }

    bool RemoveAll(std::string& error)
    {
#ifdef __APPLE__
        if (!StopBackgroundForUninstall(error)) return false;
        Runtime::LifecycleGuard lifecycle(GetUsersFolderPath(), Runtime::LifecycleAccess::Exclusive);
        if (!lifecycle.IsLocked())
        {
            error = "常驻后台已停止。" + lifecycle.Error() + "。登录启动配置、应用数据和程序已保留";
            return false;
        }
#endif
        RemoveAutoStart();

        std::vector<fs::path> leftovers;
        RemoveSharedData(leftovers);

#ifdef __APPLE__
        if (!leftovers.empty())
        {
            error = "共享数据清理未完成，程序和钥匙串密钥已保留";
            for (const auto& path : leftovers) error += "\n" + path.u8string();
            return false;
        }
#endif

        if (!RemovePlatformResidue(leftovers, error))
            return false;

        if (!leftovers.empty())
        {
            std::string remaining;
            for (const fs::path& path : leftovers)
                remaining += (remaining.empty() ? "" : "\n") + path.u8string();
            error = "以下内容未能删除（可能正被占用），"
                "请结束 AutoSZUWeb 进程或重启后手动删除:\n" + remaining;
            return false;
        }
        return true;
    }
}
