#include "sys.h"

#include "app_ui.h"
#include "credential_crypto.h"
#include "file_path.h"
#include "json_store.h"
#include "platform_support.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <Security/Security.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <libproc.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char** environ;

namespace fs = std::filesystem;

namespace
{
    const char* kMacKeychainService = "com.autoszuweb.AutoSZUWeb";
    const char* kMacKeychainAccount = "credential-encryption-key";
    const char* kMacLaunchAgentLabel = "com.autoszuweb.AutoSZUWeb";

    const char* MacKeychainService()
    {
        const char* overrideService = std::getenv("AUTOSZUWEB_KEYCHAIN_SERVICE");
        return (overrideService && *overrideService)
            ? overrideService : kMacKeychainService;
    }

    std::string MacStatusMessage(OSStatus status)
    {
        CFStringRef description = SecCopyErrorMessageString(status, nullptr);
        if (!description)
            return "OSStatus=" + std::to_string(status);

        char buffer[512] = {};
        const bool converted = CFStringGetCString(description, buffer,
            sizeof(buffer), kCFStringEncodingUTF8);
        CFRelease(description);
        return converted
            ? std::string(buffer) + " (OSStatus=" + std::to_string(status) + ")"
            : "OSStatus=" + std::to_string(status);
    }

    std::vector<unsigned char> ReadMacKeychainKey(OSStatus& status)
    {
        CFStringRef service = CFStringCreateWithCString(nullptr,
            MacKeychainService(), kCFStringEncodingUTF8);
        CFStringRef account = CFStringCreateWithCString(nullptr,
            kMacKeychainAccount, kCFStringEncodingUTF8);
        const void* keys[] = {
            kSecClass, kSecAttrService, kSecAttrAccount,
            kSecReturnData, kSecMatchLimit
        };
        const void* values[] = {
            kSecClassGenericPassword, service, account,
            kCFBooleanTrue, kSecMatchLimitOne
        };
        CFDictionaryRef query = CFDictionaryCreate(nullptr, keys, values, 5,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFTypeRef result = nullptr;
        status = SecItemCopyMatching(query, &result);
        CFRelease(query);
        CFRelease(service);
        CFRelease(account);

        if (status != errSecSuccess)
        {
            if (result)
                CFRelease(result);
            return {};
        }
        if (!result || CFGetTypeID(result) != CFDataGetTypeID())
        {
            if (result)
                CFRelease(result);
            status = errSecDecode;
            return {};
        }

        CFDataRef data = static_cast<CFDataRef>(result);
        std::vector<unsigned char> key(CFDataGetBytePtr(data),
            CFDataGetBytePtr(data) + CFDataGetLength(data));
        CFRelease(result);
        if (key.size() != CredentialCrypto::kAesKeySize)
        {
            status = errSecDecode;
            return {};
        }
        return key;
    }

    struct KeychainReadResult
    {
        OSStatus status = errSecItemNotFound;
        std::vector<unsigned char> key;
    };

    // 钥匙串条目的 ACL 与当前签名不匹配时，系统会等待用户授权。
    // 后台进程可能等不到弹窗而永久阻塞（表现为整个认证线程静默卡死），
    // 因此这里给读取加超时；超时按“无法交互”处理，绝不覆盖已有密钥。
    KeychainReadResult ReadMacKeychainKeyTimed(int timeoutMs)
    {
        auto promise = std::make_shared<std::promise<KeychainReadResult>>();
        std::future<KeychainReadResult> future = promise->get_future();
        std::thread([promise] {
            KeychainReadResult result;
            result.key = ReadMacKeychainKey(result.status);
            promise->set_value(std::move(result));
        }).detach();

        if (future.wait_for(std::chrono::milliseconds(timeoutMs)) !=
            std::future_status::ready)
        {
            return KeychainReadResult{errSecInteractionNotAllowed, {}};
        }
        return future.get();
    }

    // 钥匙串 ACL 实际绑定二进制签名，自建工具每次重建都会要求重新授权。
    // 因此首次从钥匙串读到密钥后，迁移到 0600 权限的本地文件，之后不再碰钥匙串。
    fs::path MacKeyFilePath()
    {
        return GetConfigPath().parent_path() / "credential.key";
    }

    std::vector<unsigned char> ReadMacKeyFile(bool& missing)
    {
        missing = false;
        const int fd = open(MacKeyFilePath().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0)
        {
            missing = errno == ENOENT;
            if (!missing) CredentialCrypto::SetError("无法安全读取本机凭据密钥");
            return {};
        }
        struct stat info{};
        std::vector<unsigned char> key(CredentialCrypto::kAesKeySize);
        const bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
            info.st_uid == geteuid() && info.st_nlink == 1 &&
            info.st_size == static_cast<off_t>(key.size()) &&
            read(fd, key.data(), key.size()) == static_cast<ssize_t>(key.size());
        close(fd);
        if (!valid)
        {
            CredentialCrypto::SetError("本机凭据密钥格式或归属异常，已保留原文件");
            return {};
        }
        return key;
    }

    bool WriteMacKeyFile(const std::vector<unsigned char>& key)
    {
        std::string error;
        const bool ok = JsonStore::AtomicReplacePrivateFile(MacKeyFilePath(),
            std::string(reinterpret_cast<const char*>(key.data()), key.size()), error);
        if (!ok) CredentialCrypto::SetError("保存本机凭据密钥失败：" + error);
        return ok;
    }

    std::vector<unsigned char> PlatformEncryptionKey()
    {
        std::string permissionError;
        if (!JsonStore::SecureDataDirectory(GetConfigPath(), permissionError))
        {
            CredentialCrypto::SetError(permissionError);
            return {};
        }
        bool missing = false;
        std::vector<unsigned char> fileKey = ReadMacKeyFile(missing);
        if (!fileKey.empty())
        {
            CredentialCrypto::ClearError();
            return fileKey;
        }
        if (!missing) return {};

        OSStatus status = errSecSuccess;
        KeychainReadResult initialRead = ReadMacKeychainKeyTimed(60000);
        status = initialRead.status;
        std::vector<unsigned char> existingKey = std::move(initialRead.key);
        if (!existingKey.empty())
        {
            if (!WriteMacKeyFile(existingKey)) return {};
            CredentialCrypto::ClearError();
            return existingKey;
        }
        if (status != errSecItemNotFound)
        {
            CredentialCrypto::SetError("读取 macOS 登录钥匙串失败：" + MacStatusMessage(status));
            return {};
        }

        std::vector<unsigned char> key(CredentialCrypto::kAesKeySize);
        status = SecRandomCopyBytes(kSecRandomDefault, key.size(), key.data());
        if (status != errSecSuccess)
        {
            CredentialCrypto::SetError("生成 macOS 凭据密钥失败：" + MacStatusMessage(status));
            return {};
        }

        CFStringRef service = CFStringCreateWithCString(nullptr,
            MacKeychainService(), kCFStringEncodingUTF8);
        CFStringRef account = CFStringCreateWithCString(nullptr,
            kMacKeychainAccount, kCFStringEncodingUTF8);
        CFDataRef keyData = CFDataCreate(nullptr, key.data(), key.size());
        const void* addKeys[] = {
            kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData
        };
        const void* addValues[] = {
            kSecClassGenericPassword, service, account, keyData
        };
        CFDictionaryRef addQuery = CFDictionaryCreate(nullptr,
            addKeys, addValues, 4,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        status = SecItemAdd(addQuery, nullptr);
        CFRelease(addQuery);
        CFRelease(service);
        CFRelease(account);
        CFRelease(keyData);

        if (status == errSecDuplicateItem)
        {
            KeychainReadResult duplicateRead = ReadMacKeychainKeyTimed(5000);
            status = duplicateRead.status;
            existingKey = std::move(duplicateRead.key);
            if (!existingKey.empty())
            {
                if (!WriteMacKeyFile(existingKey)) return {};
                CredentialCrypto::ClearError();
                return existingKey;
            }
        }
        if (status != errSecSuccess)
        {
            CredentialCrypto::SetError("写入 macOS 登录钥匙串失败：" + MacStatusMessage(status));
            return {};
        }

        if (!WriteMacKeyFile(key)) return {};
        CredentialCrypto::ClearError();
        return key;
    }

    fs::path CurrentExecutablePath()
    {
        uint32_t size = PATH_MAX;
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        {
            buffer.assign(static_cast<size_t>(size) + 1, '\0');
            if (_NSGetExecutablePath(buffer.data(), &size) != 0)
                return {};
        }
        std::error_code error;
        fs::path canonical = fs::weakly_canonical(fs::u8path(buffer.data()), error);
        return error ? fs::u8path(buffer.data()) : canonical;
    }

    bool IsIsolatedDataEnvironment()
    {
        const char* data = std::getenv("AUTOSZUWEB_DATA_ROOT");
        const char* agents = std::getenv("AUTOSZUWEB_LAUNCH_AGENT_DIR");
        return (data && *data) || (agents && *agents);
    }

    bool RunLaunchctl(const std::string& action, const std::string& target)
    {
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) return false;
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        char* arguments[] = {const_cast<char*>("/bin/launchctl"), const_cast<char*>(action.c_str()),
            const_cast<char*>(target.c_str()), nullptr};
        pid_t child = -1;
        const int spawned = posix_spawn(&child, "/bin/launchctl", &actions, nullptr, arguments, environ);
        posix_spawn_file_actions_destroy(&actions);
        if (spawned != 0) return false;
        int status = 0;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const auto result = waitpid(child, &status, WNOHANG);
            if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            if (result < 0 && errno != EINTR) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        return false;
    }
}

void SetAutoStart()
{
    fs::path executable = CurrentExecutablePath();
    fs::path home = std::getenv("HOME") ? fs::u8path(std::getenv("HOME")) : fs::path{};
    if (executable.empty() || home.empty())
    {
        AppUI::ShowMessage("无法获取程序路径或用户主目录，未能注册开机自启");
        return;
    }

    fs::path launchAgents = home / "Library" / "LaunchAgents";
    const char* launchAgentOverride = std::getenv("AUTOSZUWEB_LAUNCH_AGENT_DIR");
    if (launchAgentOverride && *launchAgentOverride)
        launchAgents = fs::u8path(launchAgentOverride);
    fs::path logDirectory = GetUsersFolderPath() / "AutoSZUWeb" / "logs";
    std::error_code error;
    fs::create_directories(launchAgents, error);
    fs::create_directories(logDirectory, error);
    fs::path plistPath =
        launchAgents / (std::string(kMacLaunchAgentLabel) + ".plist");
    std::ofstream plist(plistPath, std::ios::trunc);
    if (!plist)
    {
        AppUI::ShowMessage("无法写入 LaunchAgent 配置：" + plistPath.u8string());
        return;
    }
    plist << PlatformSupport::BuildLaunchAgentPlist(
        executable.u8string(),
        (logDirectory / "launchd.out.log").u8string(),
        (logDirectory / "launchd.err.log").u8string());
    plist.close();
    fs::permissions(plistPath,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace, error);
}

std::vector<std::string> GetUtf8Arguments(int argc, char* argv[])
{
    // macOS 的 argv 本身就是 UTF-8 编码。
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i)
        arguments.push_back(argv[i] ? argv[i] : "");
    return arguments;
}

bool PrepareCliConsole()
{
    return true;
}

void RemoveAutoStart()
{
    fs::path home = std::getenv("HOME") ? fs::u8path(std::getenv("HOME")) : fs::path{};
    if (home.empty())
        return;
    fs::path launchAgents = home / "Library" / "LaunchAgents";
    const char* launchAgentOverride = std::getenv("AUTOSZUWEB_LAUNCH_AGENT_DIR");
    if (launchAgentOverride && *launchAgentOverride)
        launchAgents = fs::u8path(launchAgentOverride);
    std::error_code error;
    fs::remove(launchAgents / (std::string(kMacLaunchAgentLabel) + ".plist"), error);
}

bool StopBackgroundForUninstall(std::string& error)
{
    error.clear();
    // Test/development data overrides must never control the real login agent.
    if (!IsIsolatedDataEnvironment())
    {
        const std::string target = "gui/" + std::to_string(getuid()) + "/" + kMacLaunchAgentLabel;
        if (!RunLaunchctl("bootout", target) && RunLaunchctl("print", target))
        {
            error = "无法停止登录后台任务，已保留应用数据";
            return false;
        }
    }

    const fs::path lockPath = GetConfigPath().parent_path() / "AutoSZUWeb.instance.lock";
    const int fd = open(lockPath.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT) return true;
    struct stat info{};
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_nlink != 1)
    {
        if (fd >= 0) close(fd);
        error = "无法确认后台实例锁归属，已保留应用数据";
        return false;
    }
    struct Close { int fd; ~Close() { close(fd); } } cleanup{fd};
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return true;
    if (errno != EWOULDBLOCK && errno != EAGAIN)
    {
        error = "无法检查后台实例锁，已保留应用数据";
        return false;
    }
    char bytes[64] = {};
    const auto count = pread(fd, bytes, sizeof(bytes) - 1, 0);
    char* end = nullptr;
    const long candidate = count > 0 ? std::strtol(bytes, &end, 10) : 0;
    char processPath[PROC_PIDPATHINFO_MAXSIZE] = {};
    struct proc_bsdinfo processInfo{};
    const bool validPid = candidate > 1 && candidate <= INT_MAX && candidate != getpid() &&
        end != bytes && (*end == '\0' || *end == '\n') &&
        proc_pidinfo(static_cast<pid_t>(candidate), PROC_PIDTBSDINFO, 0, &processInfo, sizeof(processInfo)) == sizeof(processInfo) &&
        processInfo.pbi_uid == geteuid() &&
        proc_pidpath(static_cast<pid_t>(candidate), processPath, sizeof(processPath)) > 0;
    const fs::path executable = validPid ? fs::path(processPath) : fs::path{};
    const bool ownedProcess = validPid && (executable == CurrentExecutablePath() ||
        (!IsIsolatedDataEnvironment() && executable == "/Applications/AutoSZUWeb.app/Contents/MacOS/AutoSZUWeb"));
    if (!ownedProcess)
    {
        error = "无法确认持锁进程属于 AutoSZUWeb，已保留应用数据";
        return false;
    }
    if (kill(static_cast<pid_t>(candidate), SIGTERM) != 0 && errno != ESRCH)
    {
        error = "无法停止后台进程，已保留应用数据";
        return false;
    }
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = "后台进程尚未停止，已保留应用数据";
    return false;
}

bool RemoveCredentialKey(std::string& error)
{
    CFStringRef service = CFStringCreateWithCString(nullptr,
        MacKeychainService(), kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(nullptr,
        kMacKeychainAccount, kCFStringEncodingUTF8);
    const void* keys[] = { kSecClass, kSecAttrService, kSecAttrAccount };
    const void* values[] = { kSecClassGenericPassword, service, account };
    CFDictionaryRef query = CFDictionaryCreate(nullptr, keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    CFRelease(service);
    CFRelease(account);
    if (status != errSecSuccess && status != errSecItemNotFound)
    {
        error = "删除 macOS 登录钥匙串密钥失败：" + MacStatusMessage(status);
        return false;
    }
    return true;
}

bool RemovePlatformResidue(std::vector<fs::path>& leftovers, std::string& error)
{
    if (!RemoveCredentialKey(error))
        return false;

    // 程序本体：.app 包（或裸二进制）；POSIX 允许删除正在运行的文件。
    const fs::path executable = CurrentExecutablePath();
    if (!executable.empty())
    {
        fs::path target = executable;
        const std::string native = executable.u8string();
        const size_t marker = native.find(".app/Contents/MacOS/");
        if (marker != std::string::npos)
            target = fs::u8path(native.substr(0, marker + 4)); // 含 ".app"
        std::error_code removeError;
        fs::remove_all(target, removeError);
        if (!removeError && !fs::exists(target, removeError))
            leftovers.erase(std::remove(leftovers.begin(), leftovers.end(), target),
                leftovers.end());
        else
            leftovers.push_back(target);
    }
    return true;
}

std::string EncryptStr(const std::string& plaintext)
{
    std::vector<unsigned char> key = PlatformEncryptionKey();
    if (key.size() != CredentialCrypto::kAesKeySize)
        return {};
    return CredentialCrypto::EncryptAesGcm(key, plaintext);
}

std::string DecryptStr(const std::string& ciphertext)
{
    std::vector<unsigned char> key = PlatformEncryptionKey();
    if (key.size() != CredentialCrypto::kAesKeySize)
        return {};
    return CredentialCrypto::DecryptAesGcm(key, ciphertext);
}
