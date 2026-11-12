// macOS 与 Linux 共用的 POSIX flock 实现。
#include "single_instance.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

Runtime::LifecycleGuard::LifecycleGuard(const fs::path& dataRoot, LifecycleAccess access)
{
    if (dataRoot.empty()) { error_ = "无法确定应用数据根目录"; return; }
    std::error_code error;
    fs::create_directories(dataRoot, error);
    if (error) { error_ = "无法创建应用数据根目录"; return; }
    const auto path = dataRoot / ".AutoSZUWeb.lifecycle.lock";
    descriptor_ = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    struct stat info{};
    if (descriptor_ < 0 || fstat(descriptor_, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_nlink != 1 || fchmod(descriptor_, 0600) != 0)
    {
        error_ = "应用生命周期锁归属、类型或权限异常";
        return;
    }
    const int operation = access == LifecycleAccess::Exclusive ? LOCK_EX : LOCK_SH;
    locked_ = flock(descriptor_, operation | LOCK_NB) == 0;
    if (!locked_)
    {
        if (errno != EWOULDBLOCK && errno != EAGAIN)
            error_ = "无法获取应用生命周期锁";
        else
            error_ = access == LifecycleAccess::Exclusive
                ? "账号设置窗口或其他命令仍在使用应用数据，请关闭后重试卸载"
                : "应用正在卸载，请等待卸载完成后重新打开";
    }
}

Runtime::LifecycleGuard::~LifecycleGuard()
{
    if (descriptor_ >= 0)
    {
        if (locked_) flock(descriptor_, LOCK_UN);
        close(descriptor_);
    }
}

bool Runtime::LifecycleGuard::IsLocked() const { return locked_; }
const std::string& Runtime::LifecycleGuard::Error() const { return error_; }

struct Runtime::SingleInstanceGuard::Impl
{
    bool primary = false;
    int descriptor = -1;
};

Runtime::SingleInstanceGuard::SingleInstanceGuard(const fs::path& lockPath)
    : impl_(std::make_unique<Impl>())
{
    std::error_code error;
    if (!lockPath.parent_path().empty())
        fs::create_directories(lockPath.parent_path(), error);
    if (error)
        return;

    impl_->descriptor = open(lockPath.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (impl_->descriptor < 0)
        return;

    struct stat info{};
    if (fstat(impl_->descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_nlink != 1 || fchmod(impl_->descriptor, 0600) != 0)
    {
        close(impl_->descriptor);
        impl_->descriptor = -1;
        return;
    }

    if (flock(impl_->descriptor, LOCK_EX | LOCK_NB) != 0)
    {
        close(impl_->descriptor);
        impl_->descriptor = -1;
        return;
    }

    // The file is diagnostic only; flock, not file existence, owns the lock.
    if (ftruncate(impl_->descriptor, 0) == 0)
    {
        const std::string pid = std::to_string(static_cast<long long>(getpid())) + "\n";
        (void)write(impl_->descriptor, pid.data(), pid.size());
    }
    impl_->primary = true;
}

Runtime::SingleInstanceGuard::~SingleInstanceGuard()
{
    if (!impl_)
        return;
    if (impl_->descriptor >= 0)
    {
        if (impl_->primary)
            flock(impl_->descriptor, LOCK_UN);
        close(impl_->descriptor);
    }
}

bool Runtime::SingleInstanceGuard::IsPrimary() const
{
    return impl_ && impl_->primary;
}
