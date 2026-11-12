#include "file_path.h"
#include "single_instance.h"
#include "sys.h"
#include "uninstall.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <cerrno>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace
{
    fs::path root;
    int failures = 0;
    bool launchRemoved = false;
    bool platformRemoved = false;
    bool stopCalled = false;
    bool exclusiveCoveredPlatformRemoval = false;
    std::unique_ptr<Runtime::LifecycleGuard> resident;

    void Check(bool ok, const std::string& description)
    {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << description << '\n';
        if (!ok) ++failures;
    }

    int Reap(pid_t child)
    {
        int status = -1;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        return status;
    }
}

// Platform stubs keep the uninstall flow entirely inside this fixture. The real
// macOS implementation, keychain, launchctl, and application deletion are absent.
fs::path GetUsersFolderPath() { return root; }
fs::path GetDesktopPath() { return root / "desktop"; }
fs::path GetConfigPath() { return root / "AutoSZUWeb" / "setting.json"; }
void RemoveAutoStart() { launchRemoved = true; }
bool StopBackgroundForUninstall(std::string&)
{
    stopCalled = true;
    resident.reset();
    return true;
}
bool RemovePlatformResidue(std::vector<fs::path>&, std::string&)
{
    platformRemoved = true;
    const pid_t child = fork();
    if (child == 0)
    {
        Runtime::LifecycleGuard newWindow(root, Runtime::LifecycleAccess::Shared);
        if (newWindow.IsLocked())
        {
            fs::create_directories(root / "AutoSZUWeb");
            std::ofstream(root / "AutoSZUWeb" / "unexpected.json") << "{}";
        }
        _exit(newWindow.IsLocked() ? 1 : 0);
    }
    const int status = child > 0 ? Reap(child) : -1;
    exclusiveCoveredPlatformRemoval = child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return true;
}

int main()
{
    root = fs::temp_directory_path() / ("AutoSZU-Uninstall-Lifecycle-" + std::to_string(getpid()));
    fs::create_directories(root / "AutoSZUWeb");
    fs::create_directories(GetDesktopPath());
    std::ofstream(GetConfigPath()) << "{\"synthetic\":true}";
    std::ofstream(GetDesktopPath() / "userdata.txt") << "user-owned document";
    resident = std::make_unique<Runtime::LifecycleGuard>(root, Runtime::LifecycleAccess::Shared);
    Check(resident->IsLocked(), "resident acquires shared lifecycle lock");
    int ready[2], finish[2];
    if (pipe(ready) != 0 || pipe(finish) != 0) return 1;
    const pid_t window = fork();
    if (window == 0)
    {
        // The independent window has its own shared descriptor.
        resident.reset();
        close(ready[0]); close(finish[1]);
        Runtime::LifecycleGuard settings(root, Runtime::LifecycleAccess::Shared);
        const char value = settings.IsLocked() ? 1 : 0;
        write(ready[1], &value, 1);
        close(ready[1]);
        char done;
        read(finish[0], &done, 1);
        close(finish[0]);
        _exit(settings.IsLocked() ? 0 : 1);
    }
    close(ready[1]); close(finish[0]);
    char started = 0;
    const bool windowReady = window > 0 && read(ready[0], &started, 1) == 1 && started == 1;
    close(ready[0]);
    Check(windowReady, "independent settings process acquires its shared lock");

    Runtime::LifecycleGuard unrelated(root / "other-instance", Runtime::LifecycleAccess::Exclusive);
    Check(unrelated.IsLocked(), "different data root has independent lifecycle coordination");
    std::string error;
    const bool blocked = !Uninstall::RemoveAll(error);
    Check(blocked && stopCalled && !launchRemoved && !platformRemoved && fs::exists(GetConfigPath()),
          "open settings window blocks uninstall after resident stops and preserves launch configuration and data");
    Check(error.find("账号设置窗口") != std::string::npos && error.find("已停止") != std::string::npos,
          "blocked uninstall reports remaining window and stopped resident accurately");
    const char done = 1;
    write(finish[1], &done, 1);
    close(finish[1]);
    const int windowStatus = window > 0 ? Reap(window) : -1;
    Check(window > 0 && WIFEXITED(windowStatus) && WEXITSTATUS(windowStatus) == 0,
          "settings process exits and releases its lifecycle lock");

    Check(Uninstall::RemoveAll(error) && launchRemoved && platformRemoved && !fs::exists(GetConfigPath()),
          "uninstall succeeds after independent settings process exits");
    Check(exclusiveCoveredPlatformRemoval && !fs::exists(root / "AutoSZUWeb"),
          "exclusive lock covers final platform cleanup and prevents data recreation by a new window");
    struct stat lockInfo{};
    const auto coordination = root / ".AutoSZUWeb.lifecycle.lock";
    Check(stat(coordination.c_str(), &lockInfo) == 0 && lockInfo.st_size == 0 && (lockInfo.st_mode & 0777) == 0600,
          "zero-byte owner-only coordination lock survives data deletion");
    Check(fs::exists(GetDesktopPath() / "userdata.txt"), "uninstall preserves unrelated desktop file");
    std::error_code cleanup;
    fs::remove_all(root, cleanup);
    return failures == 0 ? 0 : 1;
}
