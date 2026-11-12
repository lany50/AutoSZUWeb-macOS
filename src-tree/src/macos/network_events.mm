#import <AppKit/AppKit.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include "wake_signal.h"
#include "file_path.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace NativeEvents
{
    namespace
    {
        SCDynamicStoreRef store = nullptr;
        CFRunLoopSourceRef source = nullptr;
        dispatch_source_t configSource = nullptr;
        id wakeObserver = nil;
        std::string configPath;
        struct Fingerprint
        {
            ino_t inode = 0; off_t size = 0; time_t seconds = 0; long nanos = 0;
            bool operator!=(const Fingerprint& b) const
            { return inode != b.inode || size != b.size || seconds != b.seconds || nanos != b.nanos; }
        } fingerprint;
        Fingerprint ReadFingerprint()
        {
            struct stat value{};
            if (stat(configPath.c_str(), &value) != 0) return {};
            return {value.st_ino, value.st_size, value.st_mtimespec.tv_sec, value.st_mtimespec.tv_nsec};
        }
        void Changed(SCDynamicStoreRef, CFArrayRef, void*)
        { WakeSignal::Notify(WakeSignal::Network); }
    }
    bool Start()
    {
        static bool started = false;
        if (started) return store && source && configSource;
        started = true;
        store = SCDynamicStoreCreate(nullptr, CFSTR("AutoSZUWeb Events"), Changed, nullptr);
        if (store)
        {
            const void* keys[] = {CFSTR("State:/Network/Global/IPv4"), CFSTR("State:/Network/Global/DNS")};
            const void* patterns[] = {CFSTR("State:/Network/Service/.*/IPv4"), CFSTR("State:/Network/Service/.*/DNS")};
            auto keyArray = CFArrayCreate(nullptr, keys, 2, &kCFTypeArrayCallBacks);
            auto patternArray = CFArrayCreate(nullptr, patterns, 2, &kCFTypeArrayCallBacks);
            if (SCDynamicStoreSetNotificationKeys(store, keyArray, patternArray))
                source = SCDynamicStoreCreateRunLoopSource(nullptr, store, 0);
            CFRelease(keyArray); CFRelease(patternArray);
            if (source) CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
        }
        wakeObserver = [[[NSWorkspace sharedWorkspace] notificationCenter]
            addObserverForName:NSWorkspaceDidWakeNotification object:nil queue:[NSOperationQueue mainQueue]
            usingBlock:^(NSNotification* notification) {
                (void)notification; WakeSignal::Notify(WakeSignal::Wake);
            }];
        const auto path = GetConfigPath();
        configPath = path.string(); fingerprint = ReadFingerprint();
        const int fd = open(path.parent_path().c_str(), O_EVTONLY | O_CLOEXEC);
        if (fd >= 0)
        {
            configSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, fd,
                DISPATCH_VNODE_WRITE | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_DELETE, dispatch_get_main_queue());
            if (configSource)
            {
                dispatch_source_set_event_handler(configSource, ^{
                    const auto next = ReadFingerprint();
                    if (next != fingerprint)
                    { fingerprint = next; WakeSignal::Notify(WakeSignal::Settings); }
                });
                dispatch_source_set_cancel_handler(configSource, ^{ close(fd); });
                dispatch_resume(configSource);
            }
            else close(fd);
        }
        return source && configSource && wakeObserver;
    }
}
