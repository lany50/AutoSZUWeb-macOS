#include "user_data.h"

#include <fstream>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{
    std::filesystem::path OriginPath(const std::filesystem::path& path)
    {
        return path.string() + ".autoszuweb-origin";
    }

#ifndef _WIN32
    bool WriteAll(int fd, const std::string& text)
    {
        size_t offset = 0;
        while (offset < text.size())
        {
            const auto count = write(fd, text.data() + offset, text.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            offset += static_cast<size_t>(count);
        }
        return fsync(fd) == 0;
    }
#endif

    void RemoveTrailingCarriageReturn(std::string& value)
    {
        if (!value.empty() && value.back() == '\r')
            value.pop_back();
    }
}

namespace UserData
{
    bool CreateTemplate(const std::filesystem::path& path, std::string& error)
    {
        error.clear();
#ifndef _WIN32
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) { error = "无法创建 userdata.txt，已有同名文件将被保留"; return false; }
        struct stat info{};
        bool ok = fstat(fd, &info) == 0 && WriteAll(fd,
            "(请将此行整体替换为校园卡号)\n"
            "(请将此行整体替换为统一身份认证平台密码,替换完记得Ctrl+s保存ᖰ˃̵ ֊ ˂̵ᖳ)\n");
        if (close(fd) != 0) ok = false;
        const auto marker = OriginPath(path);
        int origin = -1;
        if (ok) origin = open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (origin < 0) ok = false;
        if (ok)
            ok = WriteAll(origin, "AutoSZUWeb userdata origin v1\n" +
                std::to_string(static_cast<unsigned long long>(info.st_dev)) + " " +
                std::to_string(static_cast<unsigned long long>(info.st_ino)) + "\n");
        if (origin >= 0 && close(origin) != 0) ok = false;
        if (!ok)
        {
            unlink(path.c_str());
            if (origin >= 0) unlink(marker.c_str());
            error = "无法完整保存首次配置模板及来源记录";
        }
        return ok;
#else
        std::ofstream output(path, std::ios::trunc);
        if (!output)
        {
            error = "无法创建 userdata.txt";
            return false;
        }

        output << "(请将此行整体替换为校园卡号)\n"
               << "(请将此行整体替换为统一身份认证平台密码,替换完记得Ctrl+s保存ᖰ˃̵ ֊ ˂̵ᖳ)\n";
        output.flush();
        if (!output)
        {
            error = "写入 userdata.txt 失败";
            output.close();
            return false;
        }

        output.close();
        if (output.fail())
        {
            error = "关闭 userdata.txt 时写入失败";
            return false;
        }
        return true;
#endif
    }

    bool ReadCredentials(const std::filesystem::path& path,
                         std::string& account,
                         std::string& password,
                         std::string& error)
    {
        account.clear();
        password.clear();
        error.clear();

        {
            std::ifstream input(path);
            if (!input)
            {
                error = "无法读取 userdata.txt";
                return false;
            }

            const bool hasAccount = static_cast<bool>(std::getline(input, account));
            const bool hasPassword = static_cast<bool>(std::getline(input, password));
            if (!hasAccount || !hasPassword)
            {
                error = "userdata.txt 的前两行必须分别填写校园卡号和密码";
                return false;
            }
        }

        RemoveTrailingCarriageReturn(account);
        RemoveTrailingCarriageReturn(password);
        if (account.empty() || password.empty() ||
            account.front() == '(' || password.front() == '(')
        {
            error = "userdata.txt 的前两行必须分别填写校园卡号和密码，不能保留提示文本";
            return false;
        }
        return true;
    }

    bool RemoveFile(const std::filesystem::path& path, std::string& error)
    {
        error.clear();
        std::error_code filesystemError;
        const bool removed = std::filesystem::remove(path, filesystemError);
        if (filesystemError)
        {
            error = "删除 userdata.txt 失败：" + filesystemError.message();
            return false;
        }
        if (!removed && std::filesystem::exists(path, filesystemError))
        {
            error = "删除 userdata.txt 失败，文件仍然存在";
            return false;
        }
        std::filesystem::remove(OriginPath(path), filesystemError);
        return true;
    }

    bool RemoveOwnedFile(const std::filesystem::path& path, std::string& error)
    {
        error.clear();
#ifndef _WIN32
        const auto marker = OriginPath(path);
        const int fd = open(marker.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) return true;
        struct stat origin{}, target{};
        char bytes[160] = {};
        const bool valid = fstat(fd, &origin) == 0 && S_ISREG(origin.st_mode) &&
            origin.st_uid == geteuid() && origin.st_nlink == 1 && origin.st_size >= 0 &&
            origin.st_size < static_cast<off_t>(sizeof(bytes)) &&
            read(fd, bytes, sizeof(bytes) - 1) == origin.st_size &&
            lstat(path.c_str(), &target) == 0 && S_ISREG(target.st_mode) && target.st_uid == geteuid();
        close(fd);
        if (!valid) return true;
        std::istringstream input(bytes);
        std::string signature;
        std::getline(input, signature);
        unsigned long long device = 0, inode = 0;
        if (signature != "AutoSZUWeb userdata origin v1" || !(input >> device >> inode) ||
            device != static_cast<unsigned long long>(target.st_dev) ||
            inode != static_cast<unsigned long long>(target.st_ino)) return true;
        return RemoveFile(path, error);
#else
        (void)path;
        return true;
#endif
    }
}
