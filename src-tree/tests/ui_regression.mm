#import <AppKit/AppKit.h>
#include "app_windows.h"
#include "config.h"
#include "file_path.h"
#include "netstats.h"
#include "network_observation.h"
#include "settings.h"
#include "status.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <stdexcept>

namespace
{
    fs::path testDirectory;
    std::atomic<int> cryptoDelayMs{0};
    std::atomic<bool> decryptedOnMainThread{false};
    std::atomic<bool> throwEncryption{false};
    int failures = 0;
    int samples = 0;
    bool showSpeed = false;
    void Check(bool condition, const char* message)
    {
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
        if (!condition) ++failures;
    }
    bool Until(const std::function<bool()>& condition, double timeout = 3)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
        while (!condition() && std::chrono::steady_clock::now() < deadline)
            [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        return condition();
    }
}
fs::path GetConfigPath() { return testDirectory / "setting.json"; }
std::string EncryptStr(const std::string& value)
{
    if (throwEncryption.load()) throw std::runtime_error("fixture encryption failure");
    std::this_thread::sleep_for(std::chrono::milliseconds(cryptoDelayMs.load()));
    return "fixture:" + value;
}
std::string DecryptStr(const std::string& value)
{
    if ([NSThread isMainThread]) decryptedOnMainThread = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(cryptoDelayMs.load()));
    return value.rfind("fixture:", 0) == 0 ? value.substr(8) : "";
}
namespace Observation { std::string Diagnose() { return "isolated diagnostic fixture"; } }
namespace NetStats
{
    Counters Read(const std::string&)
    {
        ++samples;
        Counters counters;
        counters.valid = true;
        counters.interfaceName = "fixture0";
        counters.interfaceIndex = 1;
        counters.rxBytes = static_cast<unsigned long long>(samples) * 1024;
        counters.txBytes = static_cast<unsigned long long>(samples) * 512;
        return counters;
    }
}

@interface AutoSZUWebStatusController : NSObject <NSMenuDelegate>
- (void)install;
- (void)refresh;
- (void)showDiagnosticText:(NSString*)text;
@end
@interface FixtureStatusController : AutoSZUWebStatusController
@end
@implementation FixtureStatusController
- (BOOL)showSpeed { return showSpeed; }
@end

int main()
{
    @autoreleasepool
    {
        char path[] = "/tmp/autoszu-ui-regression-XXXXXX";
        if (!mkdtemp(path)) return 2;
        testDirectory = path;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];

        std::string error;
        Check(Settings::Save(GetConfigPath(), "fixture-account", "fixture-password", "fixture-password", {}, error), "prepare isolated settings");
        cryptoDelayMs = 150;
        AppWindows::ShowSettings();
        NSWindow* preferencesWindow = nil;
        for (NSWindow* window in NSApp.windows)
            if ([window.title containsString:@"账号与监测设置"]) preferencesWindow = window;
        Check(preferencesWindow != nil, "settings window exists");
        id preferences = preferencesWindow.delegate;
        Check(Until([&] { return ![[preferences valueForKey:@"busy"] boolValue]; }), "settings read finishes");
        Check(!decryptedOnMainThread.load(), "settings decryption stays off main thread");
        NSTextField* account = [preferences valueForKey:@"account"];
        NSSecureTextField* password = [preferences valueForKey:@"password"];
        NSSecureTextField* confirmation = [preferences valueForKey:@"confirmation"];
        account.stringValue = @"draft-account";
        AppWindows::ShowSettings();
        Check([account.stringValue isEqualToString:@"draft-account"], "reopening visible settings preserves draft");
        Check([account.accessibilityLabel isEqualToString:@"校园网账号"] && account.nextKeyView == password, "settings has field labels and keyboard order");
        account.stringValue = @"";
        Check(Config::WriteAutoAuthEnabled(GetConfigPath(), false, error), "concurrent auto-auth update succeeds");
        password.stringValue = @"new-fixture-password";
        confirmation.stringValue = password.stringValue;
        NSButton* save = [preferences valueForKey:@"save"];
        [save performClick:nil];
        Check([[preferences valueForKey:@"busy"] boolValue] && !save.enabled, "save is asynchronous and prevents duplicate submission");
        std::atomic<bool> mainQueueResponded{false};
        auto* responded = &mainQueueResponded;
        dispatch_async(dispatch_get_main_queue(), ^{ responded->store(true); });
        Check(Until([&] { return mainQueueResponded.load(); }, 0.1), "main queue remains responsive during credential encryption");
        Check(Until([&] { return ![[preferences valueForKey:@"busy"] boolValue]; }), "async save finishes");
        Check(!Config::ReadAutoAuthEnabled(GetConfigPath()), "password-only save preserves concurrent auto-auth change");
        password.stringValue = @"retry-fixture-password";
        confirmation.stringValue = password.stringValue;
        throwEncryption = true;
        [save performClick:nil];
        Check(Until([&] { return ![[preferences valueForKey:@"busy"] boolValue]; }), "save exception returns to editable state");
        Check([password.stringValue isEqualToString:@"retry-fixture-password"] && save.enabled, "failed save preserves the entered password for correction");
        throwEncryption = false;
        [save performClick:nil];
        [preferencesWindow close];
        Check(Until([&] { return ![[preferences valueForKey:@"busy"] boolValue]; }), "save completes after closing its window");
        Check(!preferencesWindow.visible, "save completion keeps a closed settings window closed");

        int statusEvents = 0;
        Status::SetChangeCallback([&] { (void)Status::Get(); ++statusEvents; });
        Status::SetAutoAuth(true);
        Check(statusEvents == 1, "status callback may read the snapshot without deadlocking");
        Status::SetChangeCallback({});
        Status::SetState(Status::State::Online);
        FixtureStatusController* controller = [[FixtureStatusController alloc] init];
        [controller install];
        Check([controller valueForKey:@"timer"] == nil, "hidden menu without displayed speed has no sampling timer");
        const int hiddenSamples = samples;
        Until([] { return false; }, 1.1);
        Check(samples == hiddenSamples, "hidden menu performs no periodic interface reads");
        [controller menuWillOpen:[controller valueForKey:@"menu"]];
        Check([controller valueForKey:@"timer"] != nil, "opening menu starts sampling");
        [controller menuDidClose:[controller valueForKey:@"menu"]];
        Check([controller valueForKey:@"timer"] == nil, "closing menu cancels sampling");
        showSpeed = true;
        [controller refresh];
        Check([controller valueForKey:@"timer"] != nil, "visible speed starts sampling");
        Status::SetState(Status::State::Paused);
        Check(Until([&] { return [controller valueForKey:@"timer"] == nil; }), "state callback updates hidden menu without polling");
        showSpeed = false;
        NSStatusItem* statusItem = [controller valueForKey:@"item"];
        NSImage* unchangedImage = statusItem.button.image;
        [controller refresh];
        Check(statusItem.button.image == unchangedImage, "unchanged status reuses its icon object");

        [controller showDiagnosticText:@"diagnostic running"];
        NSWindow* diagnostic = [controller valueForKey:@"diagnosticWindow"];
        [diagnostic close];
        Status::CompleteDiagnostic("diagnostic complete");
        Check(Until([&] { return [[[controller valueForKey:@"diagnosticText"] string] isEqualToString:@"diagnostic complete"]; }), "diagnostic result updates through status callback");
        Check(!diagnostic.visible, "completed diagnosis keeps a closed window closed");

        Status::RequestManualAuth();
        Status::RequestManualAuth();
        Check(Status::ConsumeManualAuthRequest() && !Status::ConsumeManualAuthRequest(), "queued manual clicks coalesce");
        Check(Status::TryBeginAuth() && !Status::TryBeginAuth(), "authentication is single flight");
        Status::RequestManualAuth();
        Check(!Status::ManualAuthPending(), "click during authentication does not queue another login");
        [controller refresh];
        Check(![[controller valueForKey:@"manualItem"] isEnabled], "manual authentication button disables during work");
        Status::EndAuth();

        NetStats::Sampler sampler;
        NetStats::Counters counter;
        counter.valid = true;
        counter.interfaceName = "en0";
        counter.interfaceIndex = 1;
        counter.rxBytes = 0xfffffff0;
        counter.rxPackets = 10;
        Check(sampler.Sample(counter, 1).rx == 0, "first speed sample establishes baseline");
        counter.rxBytes = 16;
        counter.rxPackets = 12;
        Check(sampler.Sample(counter, 2).rx == 32, "32-bit byte wrap retains correct speed");
        counter.interfaceName = "en1";
        counter.interfaceIndex = 2;
        counter.rxBytes = 5;
        Check(sampler.Sample(counter, 3).rx == 0, "interface switch resets speed baseline");
        counter.rxPackets = 0;
        counter.rxBytes = 0;
        Check(sampler.Sample(counter, 4).rx == 0, "counter reset avoids false speed spike");
        counter.rxBytes = 100;
        Check(sampler.Sample(counter, 20).rx == 0, "long sleep gap resets speed baseline");
        counter.linkChangedAt = 123;
        Check(sampler.Sample(counter, 21).rx == 0, "administrative link change resets baseline");

        Status::SetChangeCallback({});
        [[NSWorkspace sharedWorkspace].notificationCenter removeObserver:controller];
        [[NSStatusBar systemStatusBar] removeStatusItem:[controller valueForKey:@"item"]];
        fs::remove_all(testDirectory);
        std::printf("UI regression failures: %d\n", failures);
        return failures ? 1 : 0;
    }
}
