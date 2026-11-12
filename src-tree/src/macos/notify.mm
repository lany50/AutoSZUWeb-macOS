#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include "notify.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>
#include <thread>

extern char** environ;

namespace
{
    NSString* U(const std::string& value)
    {
        NSString* string = [NSString stringWithUTF8String:value.c_str()];
        return string ?: @"";
    }

    std::atomic<int> gNotificationSerial{0};
}

// 前台也展示横幅：LSUIElement 应用没有窗口，若不在 delegate 里放行，
// 通知会被静默吞掉（用户什么都看不到）。
@interface AutoSZUWebNotifier : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation AutoSZUWebNotifier

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    (void)center;
    (void)notification;
    completionHandler(UNNotificationPresentationOptionBanner |
                      UNNotificationPresentationOptionList |
                      UNNotificationPresentationOptionSound);
}

@end

namespace
{
    AutoSZUWebNotifier* gNotifier = nil;
    std::atomic<bool> gSetupDone{false};
    // -1=用户关闭，0=授权中，1=系统通知，2=系统错误后备
    std::atomic<int> gMode{0};

    std::string Escape(const std::string& value)
    {
        std::string out;
        out.reserve(value.size() + 8);
        for (char c : value)
        {
            if (c == '\\' || c == '"')
                out.push_back('\\');
            out.push_back(c);
        }
        return out;
    }

    pid_t SpawnAndReap(const char* executable, char* const arguments[])
    {
        pid_t pid = -1;
        if (posix_spawn(&pid, executable, nullptr, nullptr, arguments, environ) != 0) return -1;
        std::thread([pid] {
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }).detach();
        return pid;
    }

    // 自签名 App 拿不到通知授权（实测 didGrant=0/error），
    // 这时退回到 osascript，至少让用户能看到提示。
    void PostViaScript(const std::string& title, const std::string& body, bool sound)
    {
        std::string script = "display notification \"" + Escape(body) +
            "\" with title \"" + Escape(title) + "\"";
        if (sound)
            script += " sound name \"Ping\"";

        char* arguments[] = {
            const_cast<char*>("osascript"), const_cast<char*>("-e"),
            const_cast<char*>(script.c_str()), nullptr
        };
        SpawnAndReap("/usr/bin/osascript", arguments);
    }
}

namespace Notify
{
    void Setup()
    {
        @autoreleasepool
        {
            if (gSetupDone.exchange(true))
                return;

            UNUserNotificationCenter* center =
                [UNUserNotificationCenter currentNotificationCenter];
            if (!center)
                return;

            gNotifier = [[AutoSZUWebNotifier alloc] init];
            center.delegate = gNotifier;

            [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
                if (settings.authorizationStatus == UNAuthorizationStatusDenied)
                {
                    gMode.store(-1);
                    return;
                }
                [center requestAuthorizationWithOptions:
                    (UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                    completionHandler:^(BOOL granted, NSError* error) {
                        if (granted && !error)
                        {
                            gMode.store(1);
                            return;
                        }
                        if (!error)
                        {
                            gMode.store(-1);
                            return;
                        }
                        // Re-check denial so an explicit user choice never uses another app's notifications.
                        [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* current) {
                            gMode.store(current.authorizationStatus == UNAuthorizationStatusDenied ? -1 : 2);
                            if (gMode.load() == 2)
                                std::fprintf(stderr, "[notify] 系统通知不可用，启用本机通知后备\n");
                        }];
                    }];
            }];
        }
    }

    void Post(const std::string& title, const std::string& body, bool sound)
    {
        const int mode = gMode.load();
        if (mode <= 0) return;
        if (mode == 2)
        {
            PostViaScript(title, body, sound);
            return;
        }

        @autoreleasepool
        {
            UNUserNotificationCenter* center =
                [UNUserNotificationCenter currentNotificationCenter];
            if (!center)
                return;

            UNMutableNotificationContent* content =
                [[UNMutableNotificationContent alloc] init];
            content.title = U(title);
            content.body = U(body);
            if (sound)
                content.sound = [UNNotificationSound defaultSound];

            const int serial = ++gNotificationSerial;
            UNNotificationRequest* request = [UNNotificationRequest
                requestWithIdentifier:[NSString stringWithFormat:
                    @"autoszuweb-%d-%d", (int)getpid(), serial]
                              content:content
                              trigger:nil];

            [center addNotificationRequest:request
                     withCompletionHandler:^(NSError* error) {
                         if (error) std::fprintf(stderr, "[notify] 通知发送未完成\n");
                     }];
            [content release];
        }
    }
}
