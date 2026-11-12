#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace Runtime
{
    enum class LifecycleAccess { Shared, Exclusive };

    // The zero-byte coordination file lives beside the application data directory
    // and survives uninstall so concurrent processes always lock the same inode.
    class LifecycleGuard
    {
    public:
        LifecycleGuard(const std::filesystem::path& dataRoot, LifecycleAccess access);
        ~LifecycleGuard();
        LifecycleGuard(const LifecycleGuard&) = delete;
        LifecycleGuard& operator=(const LifecycleGuard&) = delete;
        bool IsLocked() const;
        const std::string& Error() const;

    private:
        int descriptor_ = -1;
        bool locked_ = false;
        std::string error_;
    };

    class SingleInstanceGuard
    {
    public:
        explicit SingleInstanceGuard(const std::filesystem::path& lockPath);
        ~SingleInstanceGuard();

        SingleInstanceGuard(const SingleInstanceGuard&) = delete;
        SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;
        SingleInstanceGuard(SingleInstanceGuard&&) = delete;
        SingleInstanceGuard& operator=(SingleInstanceGuard&&) = delete;

        bool IsPrimary() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
