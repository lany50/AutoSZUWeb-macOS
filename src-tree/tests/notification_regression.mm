// Includes the implementation to exercise its process launcher with a harmless substitute.
#include "../src/macos/notify.mm"
#include <vector>
#include <chrono>
#include <csignal>

int main()
{
    @autoreleasepool
    {
        std::vector<pid_t> children;
        for (int i = 0; i < 100; ++i)
        {
            char* arguments[] = {const_cast<char*>("true"), nullptr};
            const pid_t child = SpawnAndReap("/usr/bin/true", arguments);
            if (child <= 0) return 1;
            children.push_back(child);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (const auto child : children)
        {
            while (kill(child, 0) == 0 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (kill(child, 0) == 0) return 2;
        }
        int status = 0;
        if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD) return 3;
        gMode.store(-1);
        Notify::Post("fixture", "denied notification", true);
        gMode.store(0);
        Notify::Post("fixture", "pending permission", false);
        if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD) return 4;
        std::puts("PASS 100 notification substitute processes reaped");
        std::puts("PASS denied and pending notification modes launch no process");
    }
    return 0;
}
