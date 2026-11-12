#include "cli.h"
#include "file_path.h"
#include "json_store.h"
#include "single_instance.h"
#include "sys.h"
#include "uninstall.h"
#include "user_data.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <poll.h>
#include <termios.h>
#include <util.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace
{
    int failures = 0;

    void Check(bool ok, const std::string& name)
    {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failures;
    }

    void Write(const fs::path& path, const std::string& bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << bytes;
    }

    std::string Bytes(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    mode_t Mode(const fs::path& path)
    {
        struct stat info{};
        return stat(path.c_str(), &info) == 0 ? info.st_mode & 0777 : 0;
    }

    void TestPrivateFiles(const fs::path& root)
    {
        const auto config = GetConfigPath();
        const auto directory = config.parent_path();
        Write(config, "{\"value\":1}\n");
        Write(directory / "credential.key", std::string(32, 'S'));
        Write(directory / "events.json", "{}");
        chmod(directory.c_str(), 0755);
        for (const auto& path : {config, directory / "credential.key", directory / "events.json"})
            chmod(path.c_str(), 0644);
        std::string error;
        Check(JsonStore::SecureDataDirectory(config, error), "old private-file permissions migrate");
        Check(Mode(directory) == 0700 && Mode(config) == 0600 &&
              Mode(directory / "credential.key") == 0600 && Mode(directory / "events.json") == 0600,
              "data directory is 0700 and private files are 0600");
        const auto before = Bytes(config);
        chmod(config.c_str(), 0644);
        Check(JsonStore::Update(config, false, [](auto&, auto&) { return true; }, error) &&
              Bytes(config) == before && Mode(config) == 0600,
              "no-op update repairs permissions without rewriting content");

        const auto ciphertext = EncryptStr("synthetic credential");
        Check(!ciphertext.empty() && DecryptStr(ciphertext) == "synthetic credential",
              "credential round trip uses isolated local key");
        Write(directory / "credential.key", "invalid-short-key");
        Check(EncryptStr("synthetic").empty() && Bytes(directory / "credential.key") == "invalid-short-key",
              "damaged key fails without replacing it or consulting keychain");
        Check(JsonStore::AtomicReplacePrivateFile(directory / "credential.key", std::string(32, 'N'), error) &&
              Bytes(directory / "credential.key") == std::string(32, 'N') && Mode(directory / "credential.key") == 0600,
              "atomic private-file writer persists complete key with owner-only mode");

        const auto outside = root / "unrelated.json";
        Write(outside, "{\"user\":true}");
        chmod(outside.c_str(), 0644);
        fs::remove(config);
        fs::create_symlink(outside, config);
        nlohmann::json data;
        Check(!JsonStore::SecureDataDirectory(config, error) && !JsonStore::Read(config, data) &&
              !JsonStore::AtomicReplacePrivateFile(config, "replacement", error) && Mode(outside) == 0644 &&
              Bytes(outside) == "{\"user\":true}", "symlinked config is rejected and target remains intact");
        fs::remove(config);
        Write(config, "{}");

        const auto directoryLink = root / "directory-link";
        fs::create_directory_symlink(directory, directoryLink);
        Check(!JsonStore::SecureDataDirectory(directoryLink / "setting.json", error),
              "symlinked data directory is rejected");

        const auto lock = directory / "blocked.lock";
        fs::create_symlink(outside, lock);
        Runtime::SingleInstanceGuard rejected(lock);
        Check(!rejected.IsPrimary() && Bytes(outside) == "{\"user\":true}",
              "instance guard rejects symlink without truncating target");

        const auto inheritedLock = directory / "inheritance.lock";
        Runtime::SingleInstanceGuard guard(inheritedLock);
        bool closeOnExec = false;
        struct stat lockInfo{};
        stat(inheritedLock.c_str(), &lockInfo);
        for (int fd = 3; fd < 256; ++fd)
        {
            struct stat descriptorInfo{};
            if (fstat(fd, &descriptorInfo) == 0 && descriptorInfo.st_dev == lockInfo.st_dev &&
                descriptorInfo.st_ino == lockInfo.st_ino)
                closeOnExec = (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
        }
        Check(guard.IsPrimary() && closeOnExec, "instance lock descriptor closes on exec");
    }

    void TestFailedAndInterruptedSaves(const fs::path& root)
    {
        const auto directory = root / "save-failures";
        const auto limitedPath = directory / "limited.json";
        const std::string original = "{\"version\":0,\"payload\":\"original\"}\n";
        std::string error;
        Check(JsonStore::AtomicReplacePrivateFile(limitedPath, original, error),
              "write-failure fixture starts with a complete private file");
        const pid_t limitedChild = fork();
        if (limitedChild == 0)
        {
            signal(SIGXFSZ, SIG_IGN);
            const struct rlimit limit{4096, 4096};
            if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(2);
            std::string issue;
            const bool saved = JsonStore::Update(limitedPath, false, [](auto& data, auto&) {
                data["version"] = 1;
                data["payload"] = std::string(16384, 'F');
                return true;
            }, issue);
            _exit(!saved && !issue.empty() ? 0 : 1);
        }
        int limitedStatus = -1;
        if (limitedChild > 0)
            while (waitpid(limitedChild, &limitedStatus, 0) < 0 && errno == EINTR) {}
        Check(limitedChild > 0 && WIFEXITED(limitedStatus) && WEXITSTATUS(limitedStatus) == 0,
              "isolated file-size limit makes partial temporary-file write fail");
        Check(Bytes(limitedPath) == original && Mode(limitedPath) == 0600,
              "failed partial write preserves exact original bytes and private permissions");
        bool temporaryRemains = false;
        for (const auto& entry : fs::directory_iterator(directory))
            temporaryRemains |= entry.path().filename().string().find("limited.json.tmp.") == 0;
        Check(!temporaryRemains, "failed partial write removes its temporary file");

        const auto interruptedPath = directory / "interrupted.json";
        const nlohmann::json oldVersion{{"version", 0}, {"payload", std::string(256 * 1024, 'A')}};
        const nlohmann::json newVersion{{"version", 1}, {"payload", std::string(256 * 1024, 'B')}};
        Check(JsonStore::AtomicReplacePrivateFile(interruptedPath, oldVersion.dump(), error),
              "interrupted-save fixture starts with a complete large JSON document");
        int committed[2];
        if (pipe(committed) != 0) { Check(false, "create interrupted-save channel"); return; }
        const pid_t writer = fork();
        if (writer == 0)
        {
            close(committed[0]);
            unsigned iteration = 0;
            for (;;)
            {
                const auto& next = (++iteration % 2) ? newVersion : oldVersion;
                std::string issue;
                if (!JsonStore::Update(interruptedPath, false, [&](auto& data, auto&) {
                        data = next;
                        return true;
                    }, issue)) _exit(2);
                if (iteration == 1)
                {
                    const char ready = 1;
                    write(committed[1], &ready, 1);
                    close(committed[1]);
                }
            }
        }
        close(committed[1]);
        struct pollfd ready{committed[0], POLLIN, 0};
        char firstCommit = 0;
        const bool started = writer > 0 && poll(&ready, 1, 3000) > 0 &&
            read(committed[0], &firstCommit, 1) == 1 && firstCommit == 1;
        close(committed[0]);
        bool sawTemporary = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (started && !sawTemporary && std::chrono::steady_clock::now() < deadline)
        {
            for (const auto& entry : fs::directory_iterator(directory))
                sawTemporary |= entry.path().filename().string().find("interrupted.json.tmp.") == 0;
            if (!sawTemporary) usleep(200);
        }
        int writerStatus = -1;
        if (writer > 0)
        {
            kill(writer, SIGKILL);
            while (waitpid(writer, &writerStatus, 0) < 0 && errno == EINTR) {}
        }
        Check(started && sawTemporary && WIFSIGNALED(writerStatus) && WTERMSIG(writerStatus) == SIGKILL,
              "writer is interrupted after observing an in-progress atomic-save temporary file");
        nlohmann::json recovered;
        Check(JsonStore::Read(interruptedPath, recovered) && (recovered == oldVersion || recovered == newVersion),
              "interrupted save leaves a parseable complete old or new JSON version");
        Check(Mode(interruptedPath) == 0600, "interrupted save retains owner-only configuration permissions");
        Check(JsonStore::Update(interruptedPath, false, [&](auto& data, auto&) {
                  data = data == oldVersion ? newVersion : oldVersion;
                  return true;
              }, error) && JsonStore::Read(interruptedPath, recovered) &&
              (recovered == oldVersion || recovered == newVersion),
              "save can be retried after interrupted writer releases its process lock");
    }

    void TestLegacyOwnership(const fs::path& root)
    {
        const auto desktop = root / "desktop";
        fs::create_directories(desktop);
        const auto path = desktop / "userdata.txt";
        std::string error;
        Write(path, "unrelated user document");
        Check(!UserData::CreateTemplate(path, error) && Bytes(path) == "unrelated user document",
              "template creation preserves existing same-name user file");
        Check(UserData::RemoveOwnedFile(path, error) && fs::exists(path),
              "uninstall preserves unrecognized desktop file");
        fs::remove(path);
        Check(UserData::CreateTemplate(path, error), "legacy template records its file identity");
        Check(UserData::RemoveOwnedFile(path, error) && !fs::exists(path),
              "owned legacy template is removable");
        Check(UserData::CreateTemplate(path, error), "replacement test template is created");
        const auto replacement = desktop / "replacement.txt";
        Write(replacement, "replacement user document");
        fs::rename(replacement, path);
        Check(UserData::RemoveOwnedFile(path, error) && Bytes(path) == "replacement user document",
              "replaced desktop file is preserved despite stale origin record");
    }

    void TestStopBeforeRemoval()
    {
        const auto lockPath = GetConfigPath().parent_path() / "AutoSZUWeb.instance.lock";
        int ready[2];
        if (pipe(ready) != 0) { Check(false, "create isolated process channel"); return; }
        const pid_t child = fork();
        if (child == 0)
        {
            close(ready[0]);
            Runtime::SingleInstanceGuard guard(lockPath);
            const char ok = guard.IsPrimary() ? 1 : 0;
            write(ready[1], &ok, 1);
            close(ready[1]);
            for (;;) pause();
        }
        close(ready[1]);
        char readyByte = 0;
        const bool started = child > 0 && read(ready[0], &readyByte, 1) == 1 && readyByte == 1;
        close(ready[0]);
        std::string error;
        const bool stopped = started && StopBackgroundForUninstall(error);
        if (child > 0)
        {
            if (!stopped) kill(child, SIGKILL);
            int status = 0;
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
        Check(stopped && fs::exists(GetConfigPath()), "verified isolated resident stops before shared data is removed");
        Check(StopBackgroundForUninstall(error), "already-stopped isolated resident is harmless");
        std::vector<fs::path> leftovers;
        Uninstall::RemoveSharedData(leftovers);
        Check(leftovers.empty() && !fs::exists(lockPath.parent_path()), "shared data removal completes after resident stops");
        Check(Bytes(GetDesktopPath() / "userdata.txt") == "replacement user document",
              "shared-data cleanup preserves replaced desktop document");
    }

    void TestUnrecognizedLockAndDirectory(const fs::path& root)
    {
        const auto config = GetConfigPath();
        Write(config, "{}");
        const auto lockPath = config.parent_path() / "AutoSZUWeb.instance.lock";
        int ready[2];
        if (pipe(ready) != 0) { Check(false, "create unrecognized-lock channel"); return; }
        const pid_t child = fork();
        if (child == 0)
        {
            close(ready[0]);
            Runtime::SingleInstanceGuard guard(lockPath);
            Write(lockPath, std::to_string(getppid()) + "\n");
            const char ok = guard.IsPrimary() ? 1 : 0;
            write(ready[1], &ok, 1);
            close(ready[1]);
            for (;;) pause();
        }
        close(ready[1]);
        char readyByte = 0;
        const bool started = child > 0 && read(ready[0], &readyByte, 1) == 1 && readyByte == 1;
        close(ready[0]);
        std::string error;
        const bool rejected = started && !StopBackgroundForUninstall(error);
        int status = 0;
        const bool alive = child > 0 && waitpid(child, &status, WNOHANG) == 0;
        if (child > 0 && alive)
        {
            kill(child, SIGTERM);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
        Check(rejected && alive && fs::exists(config),
              "unrecognized held lock preserves process and application data");
        fs::remove_all(config.parent_path());
        const auto outside = root / "uninstall-outside";
        fs::create_directories(outside);
        Write(outside / "document.txt", "user document");
        fs::create_directory_symlink(outside, config.parent_path());
        std::vector<fs::path> leftovers;
        Uninstall::RemoveSharedData(leftovers);
        Check(!leftovers.empty() && Bytes(outside / "document.txt") == "user document",
              "uninstall rejects symlinked data directory and preserves target contents");
    }

    void TestCli()
    {
        Check(Cli::Parse({"--login"}).action == Cli::Action::InteractiveAuth &&
              Cli::Parse({"--login", "1234"}).action == Cli::Action::InteractiveAuth,
              "interactive login accepts optional account");
        Check(Cli::Parse({"--password-stdin", "1234"}).action == Cli::Action::PasswordStdinAuth &&
              Cli::Parse({"--password-stdin"}).action == Cli::Action::UsageError &&
              Cli::Parse({"--login", "bad account"}).action == Cli::Action::UsageError,
              "stdin login requires valid account");
        std::string password, error;
        std::istringstream valid("synthetic pass&$\r\n");
        Check(Cli::ReadPasswordLine(valid, password, error) && password == "synthetic pass&$",
              "stdin password preserves spaces and symbols and normalizes CRLF");
        std::istringstream empty("\n"), oversized(std::string(513, 'x') + "\n"), control("bad\tvalue\n");
        Check(!Cli::ReadPasswordLine(empty, password, error) &&
              !Cli::ReadPasswordLine(oversized, password, error) &&
              !Cli::ReadPasswordLine(control, password, error) && password.empty(),
              "invalid stdin passwords are rejected and cleared");
        const auto legacy = Cli::Parse({"1234", "legacy-secret"});
        std::string account;
        Check(Cli::ReadLoginCredentials(legacy, account, password, error) &&
              account == "1234" && password == "legacy-secret", "legacy one-shot credentials remain compatible");
    }

    void TestHiddenTerminalInput()
    {
        int terminal = -1;
        const pid_t child = forkpty(&terminal, nullptr, nullptr, nullptr);
        if (child == 0)
        {
            std::string account, password, error;
            const bool ok = Cli::ReadLoginCredentials(Cli::Parse({"--login", "1234"}), account, password, error) &&
                account == "1234" && password == "terminal-synthetic-secret";
            const std::string result = ok ? "input-ok\n" : "input-failed\n";
            write(STDOUT_FILENO, result.data(), result.size());
            _exit(ok ? 0 : 1);
        }
        if (child < 0) { Check(false, "create isolated terminal"); return; }
        std::string output;
        bool sent = false;
        for (int attempt = 0; attempt < 60; ++attempt)
        {
            struct pollfd pending{terminal, POLLIN, 0};
            if (poll(&pending, 1, 100) <= 0) continue;
            char buffer[512];
            const auto count = read(terminal, buffer, sizeof(buffer));
            if (count <= 0) break;
            output.append(buffer, static_cast<size_t>(count));
            if (!sent && output.find("校园网密码") != std::string::npos)
            {
                const std::string password = "terminal-synthetic-secret\n";
                write(terminal, password.data(), password.size());
                sent = true;
            }
        }
        int status = 0;
        if (waitpid(child, &status, WNOHANG) == 0)
        {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
        struct termios restored{};
        const bool echoRestored = tcgetattr(terminal, &restored) == 0 && (restored.c_lflag & ECHO);
        close(terminal);
        Check(sent && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
              output.find("input-ok") != std::string::npos && output.find("terminal-synthetic-secret") == std::string::npos,
              "interactive login accepts password without terminal echo");
        Check(echoRestored, "interactive login restores terminal echo");
    }
}

int main()
{
    const auto root = fs::temp_directory_path() / ("AutoSZU-Security-" + std::to_string(getpid()));
    fs::create_directories(root);
    setenv("AUTOSZUWEB_DATA_ROOT", (root / "data").c_str(), 1);
    setenv("AUTOSZUWEB_DESKTOP_ROOT", (root / "desktop").c_str(), 1);
    setenv("AUTOSZUWEB_LAUNCH_AGENT_DIR", (root / "launch-agents").c_str(), 1);
    try
    {
        TestPrivateFiles(root);
        TestFailedAndInterruptedSaves(root);
        TestLegacyOwnership(root);
        TestCli();
        TestHiddenTerminalInput();
        TestStopBeforeRemoval();
        TestUnrecognizedLockAndDirectory(root);
    }
    catch (const std::exception& error)
    {
        std::cerr << "[FAIL] " << error.what() << '\n';
        ++failures;
    }
    std::error_code cleanup;
    fs::remove_all(root, cleanup);
    return failures == 0 ? 0 : 1;
}
