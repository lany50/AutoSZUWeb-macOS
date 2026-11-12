#include "json_store.h"
#include <cerrno>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace JsonStore
{
    namespace
    {
        std::mutex mutex;

        bool SecureDirectory(const std::filesystem::path& path, std::string& error)
        {
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            if (ec) { error = "无法创建数据目录"; return false; }
            const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat info{};
            const bool ok = fd >= 0 && fstat(fd, &info) == 0 &&
                S_ISDIR(info.st_mode) && info.st_uid == geteuid() && fchmod(fd, 0700) == 0;
            if (fd >= 0) close(fd);
            if (!ok) error = "数据目录归属或权限异常";
            return ok;
        }

        bool SecureExistingFile(const std::filesystem::path& path, std::string& error)
        {
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0 && errno == ENOENT) return true;
            struct stat info{};
            const bool ok = fd >= 0 && fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                info.st_uid == geteuid() && info.st_nlink == 1 && fchmod(fd, 0600) == 0;
            if (fd >= 0) close(fd);
            if (!ok) error = "数据文件归属、类型或权限异常：" + path.filename().string();
            return ok;
        }
    }

    bool SecureDataDirectory(const std::filesystem::path& configPath, std::string& error)
    {
        error.clear();
        const auto directory = configPath.parent_path();
        if (directory.empty() || !SecureDirectory(directory, error)) return false;
        for (const auto& name : {configPath.filename().string(), configPath.filename().string() + ".lock",
                std::string("credential.key"), std::string("events.json"), std::string("events.json.lock"),
                std::string("AutoSZUWeb.instance.lock"), std::string("authentication.lock")})
            if (!SecureExistingFile(directory / name, error)) return false;
        return true;
    }

    bool Read(const std::filesystem::path& path, nlohmann::json& data)
    {
        try
        {
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) return false;
            struct Close { int fd; ~Close() { close(fd); } } cleanup{fd};
            struct stat info{};
            if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
                info.st_size < 0 || info.st_size > 1024 * 1024) return false;
            std::string bytes;
            char buffer[4096];
            while (true)
            {
                const auto count = read(fd, buffer, sizeof(buffer));
                if (count < 0 && errno == EINTR) continue;
                if (count < 0) return false;
                if (count == 0) break;
                bytes.append(buffer, static_cast<size_t>(count));
                if (bytes.size() > 1024 * 1024) return false;
            }
            data = nlohmann::json::parse(bytes);
            return true;
        }
        catch (...) { return false; }
    }

    bool AtomicReplacePrivateFile(const std::filesystem::path& path,
                                  const std::string& bytes, std::string& error)
    {
        error.clear();
        if (!SecureDirectory(path.parent_path(), error) || !SecureExistingFile(path, error)) return false;
        std::string pattern = path.string() + ".tmp.XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back(0);
        const int fd = mkstemp(name.data());
        if (fd < 0) { error = "无法创建临时文件，已保留原文件"; return false; }
        bool ok = fchmod(fd, 0600) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
        size_t offset = 0;
        while (ok && offset < bytes.size())
        {
            const auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { ok = false; break; }
            offset += static_cast<size_t>(count);
        }
        if (ok) ok = fsync(fd) == 0;
        if (close(fd) != 0) ok = false;
        if (ok) ok = rename(name.data(), path.c_str()) == 0;
        if (!ok) { unlink(name.data()); error = "无法保存更新，已保留原文件"; return false; }
        const int directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        const bool synced = directory >= 0 && fsync(directory) == 0;
        if (directory >= 0) close(directory);
        if (!synced) error = "文件已保存，但数据目录同步失败";
        return synced;
    }

    bool Update(const std::filesystem::path& path, bool create,
                const std::function<bool(nlohmann::json&, std::string&)>& edit, std::string& error)
    {
        std::lock_guard<std::mutex> guard(mutex);
        error.clear();
        std::error_code ec;
        if (!SecureDirectory(path.parent_path(), error) || !SecureExistingFile(path, error)) return false;
        const int lockFd = open((path.string() + ".lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (lockFd < 0) { error = "无法锁定配置文件"; return false; }
        struct stat lockInfo{};
        if (fstat(lockFd, &lockInfo) != 0 || !S_ISREG(lockInfo.st_mode) ||
            lockInfo.st_uid != geteuid() || lockInfo.st_nlink != 1 || fchmod(lockFd, 0600) != 0)
        { close(lockFd); error = "配置锁文件归属或类型异常"; return false; }
        bool locked = false;
        for (int i = 0; i < 20; ++i)
        {
            if (flock(lockFd, LOCK_EX | LOCK_NB) == 0) { locked = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!locked) { close(lockFd); error = "文件正在使用，请稍后重试"; return false; }
        struct Unlock { int fd; ~Unlock() { flock(fd, LOCK_UN); close(fd); } } unlock{lockFd};
        nlohmann::json data;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec || (exists && !Read(path, data)) || (!exists && !create))
        { error = "配置或记录文件不可读，已保留原文件"; return false; }
        if (!exists) data = nlohmann::json::object();
        const auto original = data;
        try { if (!edit(data, error)) return false; }
        catch (...) { error = "无法更新配置，已保留原文件"; return false; }
        if (exists && data == original) return true;
        std::string serialized;
        try { serialized = data.dump(2); }
        catch (...) { error = "无法编码配置，已保留原文件"; return false; }
        return AtomicReplacePrivateFile(path, serialized, error);
    }
}
