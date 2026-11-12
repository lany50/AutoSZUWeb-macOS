#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include "config.h"
#include "file_path.h"
#include "netstats.h"
#include "status.h"
#include "statusbar.h"
#include "network_observation.h"
#include "app_windows.h"

#include <ctime>
#include <algorithm>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>

namespace
{
    NSString* U(const std::string& value)
    {
        NSString* string = [NSString stringWithUTF8String:value.c_str()];
        return string ?: @"";
    }

    // 校徽始终标识应用；SF Symbols 仅用作状态角标或资源缺失时的后备。
    NSString* SymbolImageName(Status::State state)
    {
        switch (state)
        {
            case Status::State::Online:         return @"graduationcap.fill";
            case Status::State::Offline:        return @"exclamationmark.triangle.fill";
            case Status::State::Authenticating: return @"arrow.triangle.2.circlepath";
            case Status::State::Paused:         return @"pause.circle.fill";
            case Status::State::Waiting:        return @"clock";
            case Status::State::Attention:      return @"exclamationmark.circle.fill";
            case Status::State::Unknown:        break;
        }
        return @"ellipsis.circle";
    }

    NSImage* MenuBarImage(Status::State state)
    {
        static NSImage* emblem = nil;
        if (!emblem)
        {
            NSString* path = [[NSBundle mainBundle] pathForResource:@"SZUEmblem" ofType:@"pdf"];
            if (path) emblem = [[NSImage alloc] initWithContentsOfFile:path];
        }
        if (!emblem)
        {
            NSImage* fallback = [NSImage imageWithSystemSymbolName:SymbolImageName(state)
                accessibilityDescription:@"AutoSZUWeb"];
            [fallback setTemplate:YES];
            return fallback;
        }
        const BOOL showState = state != Status::State::Online;
        const NSSize size = NSMakeSize(showState ? 27 : 18, 18);
        NSImage* image = [NSImage imageWithSize:size flipped:NO drawingHandler:^BOOL(NSRect rect) {
            (void)rect;
            [emblem drawInRect:NSMakeRect(0, 0, 18, 18) fromRect:NSZeroRect
                operation:NSCompositingOperationSourceOver fraction:1.0];
            if (showState)
            {
                NSImage* indicator = [NSImage imageWithSystemSymbolName:SymbolImageName(state)
                    accessibilityDescription:nil];
                [indicator drawInRect:NSMakeRect(19, 0, 8, 8) fromRect:NSZeroRect
                    operation:NSCompositingOperationSourceOver fraction:1.0];
            }
            return YES;
        }];
        [image setTemplate:YES];
        image.accessibilityDescription = @"深圳大学校徽，AutoSZUWeb";
        return image;
    }

    NSString* FormatRate(double bytesPerSecond)
    {
        if (bytesPerSecond < 1024.0)
            return [NSString stringWithFormat:@"%.0f B/s", bytesPerSecond];
        if (bytesPerSecond < 1048576.0)
            return [NSString stringWithFormat:@"%.0f KB/s", bytesPerSecond / 1024.0];
        return [NSString stringWithFormat:@"%.1f MB/s",
                bytesPerSecond / 1048576.0];
    }

    // 菜单栏空间有限，只保留数值 + 单位首字母（1.2M / 88K）。
    NSString* CompactRate(double bytesPerSecond)
    {
        if (bytesPerSecond < 1024.0)
            return [NSString stringWithFormat:@"%.0fB", bytesPerSecond];
        if (bytesPerSecond < 1048576.0)
            return [NSString stringWithFormat:@"%.0fK", bytesPerSecond / 1024.0];
        return [NSString stringWithFormat:@"%.1fM",
                bytesPerSecond / 1048576.0];
    }

    NSString* FormatUptime(long long seconds)
    {
        if (seconds <= 0)
            return @"";
        if (seconds < 60)
            return [NSString stringWithFormat:@"%lld 秒", seconds];
        if (seconds < 3600)
            return [NSString stringWithFormat:@"%lld 分钟", seconds / 60];
        return [NSString stringWithFormat:@"%lld 小时 %lld 分",
                seconds / 3600, (seconds % 3600) / 60];
    }

    NSString* const kShowSpeedKey = @"AutoSZUWebShowSpeedInMenuBar";

    NSMenuItem* AddInfoItem(NSMenu* menu, NSString* title)
    {
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                      action:nil
                                               keyEquivalent:@""];
        item.enabled = NO;
        [menu addItem:item];
        [item release];
        return item;
    }

    NSMenuItem* AddActionItem(NSMenu* menu, NSString* title, id target, SEL action)
    {
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                      action:action
                                               keyEquivalent:@""];
        item.target = target;
        [menu addItem:item];
        [item release];
        return item;
    }
}

@interface AutoSZUWebStatusController : NSObject <NSMenuDelegate>
{
    NSStatusItem* _item;
    NSMenu* _menu;
    NSMenuItem* _stateItem;
    NSMenuItem* _networkItem;
    NSMenuItem* _monitorItem;
    NSMenuItem* _retryItem;
    NSMenuItem* _diagnosticItem;
    NSMenuItem* _manualItem;
    NSMenuItem* _speedItem;
    NSMenuItem* _latencyItem;
    NSMenuItem* _authItem;
    NSMenuItem* _lastItem;
    NSMenuItem* _ipItem;
    NSMenuItem* _detailItem;
    NSMenuItem* _autoItem;
    NSMenuItem* _speedToggleItem;
    NSTimer* _timer;
    NSWindow* _diagnosticWindow;
    NSTextView* _diagnosticText;
    unsigned _seenDiagnosticSequence;

    BOOL _menuOpen;
    BOOL _autoSaving;
    BOOL _hasImage;
    Status::State _imageState;
    std::atomic<bool> _refreshScheduled;
    NetStats::Sampler _sampler;
    double _rxRate;
    double _txRate;
}
- (void)install;
- (void)refresh;
- (void)sampleSpeed;
- (void)updateSampling;
- (void)resetSampling:(NSNotification*)notification;
- (void)tick:(NSTimer*)timer;
- (void)manualAuth:(id)sender;
- (void)showSettings:(id)sender;
- (void)showHistory:(id)sender;
- (void)diagnose:(id)sender;
- (void)showDiagnosticText:(NSString*)text;
- (void)updateDiagnosticText:(NSString*)text;
- (void)copyDiagnostic:(id)sender;
- (void)toggleAutoAuth:(id)sender;
- (void)toggleSpeedDisplay:(id)sender;
- (void)openLogFolder:(id)sender;
- (void)openDataFolder:(id)sender;
- (void)quit:(id)sender;
@end

@implementation AutoSZUWebStatusController

- (BOOL)showSpeed
{
    return [[NSUserDefaults standardUserDefaults] boolForKey:kShowSpeedKey];
}

- (void)install
{
    _refreshScheduled.store(false);
    _item = [[[NSStatusBar systemStatusBar]
        statusItemWithLength:NSVariableStatusItemLength] retain];
    _item.button.imagePosition = NSImageOnly;

    _menu = [[NSMenu alloc] initWithTitle:@"AutoSZUWeb"];
    _menu.autoenablesItems = NO;
    _menu.delegate = self;

    _stateItem = [AddInfoItem(_menu, @"状态：启动中…") retain];
    _networkItem = [AddInfoItem(_menu, @"网络：正在识别…") retain];
    _monitorItem = [AddInfoItem(_menu, @"监测：正在准备…") retain];
    _retryItem = [AddInfoItem(_menu, @"认证策略：正在准备…") retain];
    _speedItem = [AddInfoItem(_menu, @"网速：—") retain];
    _latencyItem = [AddInfoItem(_menu, @"延迟：—") retain];
    _authItem = [AddInfoItem(_menu, @"本次运行认证：0 次") retain];
    _lastItem = [AddInfoItem(_menu, @"最近认证：尚无记录") retain];
    _ipItem = [AddInfoItem(_menu, @"") retain];
    _detailItem = [AddInfoItem(_menu, @"") retain];

    [_menu addItem:[NSMenuItem separatorItem]];

    AddActionItem(_menu, @"账号与监测设置…", self, @selector(showSettings:));
    AddActionItem(_menu, @"最近故障与恢复…", self, @selector(showHistory:));
    _diagnosticItem = [AddActionItem(_menu, @"一键诊断（不改网络）", self, @selector(diagnose:)) retain];
    _manualItem = [AddActionItem(_menu, @"立即认证／恢复重试", self, @selector(manualAuth:)) retain];
    _autoItem = [AddActionItem(_menu, @"自动认证", self, @selector(toggleAutoAuth:)) retain];
    _speedToggleItem = [AddActionItem(_menu, @"菜单栏显示网速",
        self, @selector(toggleSpeedDisplay:)) retain];

    [_menu addItem:[NSMenuItem separatorItem]];

    AddActionItem(_menu, @"打开认证日志", self, @selector(openLogFolder:));
    AddActionItem(_menu, @"打开数据目录", self, @selector(openDataFolder:));

    [_menu addItem:[NSMenuItem separatorItem]];

    AddActionItem(_menu, @"退出 AutoSZUWeb", self, @selector(quit:));

    _item.menu = _menu;
    [self refresh];

    Status::SetChangeCallback([self] {
        if (_refreshScheduled.exchange(true)) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            _refreshScheduled.store(false);
            [self refresh];
        });
    });
    [[NSWorkspace sharedWorkspace].notificationCenter addObserver:self selector:@selector(resetSampling:)
        name:NSWorkspaceDidWakeNotification object:nil];
    [self updateSampling];
}

- (void)menuWillOpen:(NSMenu*)menu
{
    (void)menu;
    _menuOpen = YES;
    [self refresh];
}

- (void)menuDidClose:(NSMenu*)menu
{
    (void)menu;
    _menuOpen = NO;
    [self updateSampling];
}

- (void)resetSampling:(NSNotification*)notification
{
    (void)notification;
    _sampler.Reset();
    _rxRate = 0;
    _txRate = 0;
    [self refresh];
}

- (void)updateSampling
{
    const BOOL needed = _menuOpen || ([self showSpeed] && Status::Get().state == Status::State::Online);
    if (needed && !_timer)
    {
        _sampler.Reset();
        [self sampleSpeed];
        _timer = [[NSTimer timerWithTimeInterval:1.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES] retain];
        _timer.tolerance = 0.15;
        [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    else if (!needed && _timer)
    {
        [_timer invalidate];
        [_timer release];
        _timer = nil;
        _sampler.Reset();
        _rxRate = 0;
        _txRate = 0;
    }
}

- (void)tick:(NSTimer*)timer
{
    (void)timer;
    [self sampleSpeed];
    [self refresh];
}

- (void)sampleSpeed
{
    if (Status::Get().state != Status::State::Online)
    {
        _sampler.Reset();
        _rxRate = 0;
        _txRate = 0;
        return;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto rate = _sampler.Sample(NetStats::Read(), std::chrono::duration<double>(now).count());
    _rxRate = rate.rx;
    _txRate = rate.tx;
}

- (void)refresh
{
    [self updateSampling];
    const Status::Snapshot snapshot = Status::Get();
    NSString* stateText = U(Status::StateText(snapshot.state));
    const bool online = snapshot.state == Status::State::Online;
    _networkItem.title = [NSString stringWithFormat:@"网络：%@", U(snapshot.networkLabel)];
    const long long remaining = std::max(0LL, snapshot.nextCheckTime - static_cast<long long>(std::time(nullptr)));
    _monitorItem.title = !snapshot.autoAuth && snapshot.state == Status::State::Paused
        ? @"监测：自动认证已关闭，仍可手动诊断"
        : snapshot.monitorInterval > 0
        ? [NSString stringWithFormat:@"监测：%@ · %lld 秒后检查", snapshot.powerSaving ? @"省电" : @"常规", remaining]
        : @"监测：正在检查…";
    _retryItem.title = [NSString stringWithFormat:@"认证策略：%@", U(snapshot.retryText)];
    _diagnosticItem.title = snapshot.diagnosticRunning
        ? @"正在诊断…（不改网络）" : @"一键诊断（不改网络）";
    _diagnosticItem.enabled = !snapshot.diagnosticRunning;
    _manualItem.enabled = !snapshot.authInProgress && !snapshot.manualAuthPending;
    _manualItem.title = snapshot.authInProgress ? @"认证正在进行…" :
        snapshot.manualAuthPending ? @"认证请求已排队…" : @"立即认证／恢复重试";
    _autoItem.enabled = !_autoSaving;
    if (snapshot.diagnosticSequence > _seenDiagnosticSequence)
    {
        _seenDiagnosticSequence = snapshot.diagnosticSequence;
        [self updateDiagnosticText:U(snapshot.diagnosticReport)];
    }

    // 图标
    if (!_hasImage || _imageState != snapshot.state)
    {
        _item.button.image = MenuBarImage(snapshot.state);
        _imageState = snapshot.state;
        _hasImage = YES;
    }
    NSImage* image = _item.button.image;

    // 菜单栏紧凑模式：在线时在图标右侧显示下行速率
    NSString* compact = @"";
    if ([self showSpeed] && online)
        compact = [NSString stringWithFormat:@" ↓%@", CompactRate(_rxRate)];

    if (!image)
        compact = compact.length > 0 ? compact : @" 校";
    _item.button.imagePosition = compact.length > 0
        ? NSImageLeft : NSImageOnly;
    _item.button.title = compact;

    // 菜单文本
    if (online && snapshot.onlineSince > 0)
    {
        const long long seconds =
            static_cast<long long>(std::time(nullptr)) - snapshot.onlineSince;
        const NSString* uptime = FormatUptime(seconds);
        _stateItem.title = [NSString stringWithFormat:@"状态：%@ · 已在线 %@",
                            stateText, uptime];
    }
    else
    {
        _stateItem.title = [NSString stringWithFormat:@"状态：%@", stateText];
    }

    _speedItem.title = [NSString stringWithFormat:@"网速：↓ %@   ↑ %@",
                        FormatRate(_rxRate), FormatRate(_txRate)];
    _speedItem.hidden = !online;

    if (snapshot.lastCheckTime > 0)
    {
        NSDateFormatter* formatter = [[[NSDateFormatter alloc] init] autorelease];
        formatter.dateFormat = @"HH:mm:ss";
        NSString* checked = [formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:snapshot.lastCheckTime]];
        NSString* result = snapshot.latencyMs >= 0 ? [NSString stringWithFormat:@"%d ms", snapshot.latencyMs] : @"外网未通过";
        _latencyItem.title = [NSString stringWithFormat:@"最近检查：%@ · %@", checked, result];
        _latencyItem.hidden = NO;
    }
    else
    {
        _latencyItem.hidden = YES;
    }

    _authItem.title = [NSString stringWithFormat:@"本次运行认证：%d 次",
                       snapshot.authCount];
    _authItem.hidden = NO;

    if (snapshot.lastAuthTime.empty())
    {
        _lastItem.title = @"最近认证：尚无记录";
    }
    else
    {
        NSString* method = snapshot.lastAuthMethod.empty()
            ? @"" : [NSString stringWithFormat:@" · %@", U(snapshot.lastAuthMethod)];
        _lastItem.title = [NSString stringWithFormat:@"最近尝试：%@%@ · %@",
                           U(snapshot.lastAuthTime), method, snapshot.lastAuthOk ? @"接口已接受" : @"认证未完成"];
    }

    if (snapshot.currentIp.empty())
    {
        _ipItem.title = @"";
        _ipItem.hidden = YES;
    }
    else
    {
        _ipItem.title = [NSString stringWithFormat:@"本机 IP：%@", U(snapshot.currentIp)];
        _ipItem.hidden = NO;
    }

    if (snapshot.detail.empty())
    {
        _detailItem.title = @"";
        _detailItem.hidden = YES;
    }
    else
    {
        NSString* detail = U(snapshot.detail);
        if (detail.length > 72)
            detail = [[detail substringWithRange:
                [detail rangeOfComposedCharacterSequencesForRange:NSMakeRange(0, 72)]]
                    stringByAppendingString:@"…"];
        _detailItem.title = [NSString stringWithFormat:@"说明：%@", detail];
        _detailItem.hidden = NO;
    }

    _autoItem.state = snapshot.autoAuth ? NSControlStateValueOn : NSControlStateValueOff;
    _speedToggleItem.state = [self showSpeed]
        ? NSControlStateValueOn : NSControlStateValueOff;

    // 悬停提示：不用点开也能看到关键信息
    NSMutableString* tip = [NSMutableString stringWithFormat:@"AutoSZUWeb · %@",
                            stateText];
    if (online)
        [tip appendFormat:@" · ↓%@", FormatRate(_rxRate)];
    if (snapshot.latencyMs >= 0)
        [tip appendFormat:@" · %d ms", snapshot.latencyMs];
    _item.button.toolTip = tip;
    _item.button.accessibilityLabel = tip;
}

- (void)manualAuth:(id)sender
{
    (void)sender;
    Status::RequestManualAuth();
    [self refresh];
}

- (void)showSettings:(id)sender { (void)sender; AppWindows::ShowSettings(); }
- (void)showHistory:(id)sender { (void)sender; AppWindows::ShowHistory(); }

- (void)diagnose:(id)sender
{
    (void)sender;
    Status::RequestDiagnostic();
    [self showDiagnosticText:@"正在检查网络、DNS、校园认证门户和当前代理路径…\n\n检查只发送不带账号密码的请求，不会关闭代理、切换网络或重置校园网认证。"];
    [self refresh];
}

- (void)showDiagnosticText:(NSString*)text
{
    if (!_diagnosticWindow)
    {
        _diagnosticWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 740, 560)
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
            backing:NSBackingStoreBuffered defer:NO];
        [_diagnosticWindow setTitle:@"AutoSZUWeb · 网络诊断"];
        [_diagnosticWindow setReleasedWhenClosed:NO];
        [_diagnosticWindow setMinSize:NSMakeSize(540, 400)];
        NSView* content = _diagnosticWindow.contentView;
        NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(16, 60, 708, 484)];
        scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        scroll.hasVerticalScroller = YES;
        scroll.borderType = NSBezelBorder;
        _diagnosticText = [[NSTextView alloc] initWithFrame:scroll.contentView.bounds];
        _diagnosticText.editable = NO;
        _diagnosticText.selectable = YES;
        _diagnosticText.font = [NSFont systemFontOfSize:14];
        _diagnosticText.textColor = [NSColor labelColor];
        _diagnosticText.backgroundColor = [NSColor textBackgroundColor];
        _diagnosticText.textContainerInset = NSMakeSize(14, 14);
        _diagnosticText.autoresizingMask = NSViewWidthSizable;
        _diagnosticText.verticallyResizable = YES;
        _diagnosticText.horizontallyResizable = NO;
        _diagnosticText.textContainer.widthTracksTextView = YES;
        scroll.documentView = _diagnosticText;
        [content addSubview:scroll];
        [scroll release];
        NSButton* copy = [NSButton buttonWithTitle:@"复制诊断结果" target:self action:@selector(copyDiagnostic:)];
        copy.frame = NSMakeRect(16, 16, 140, 32);
        copy.bezelStyle = NSBezelStyleRounded;
        [content addSubview:copy];
        NSTextField* hint = [NSTextField labelWithString:@"仅在本机显示，不会自动上传"];
        hint.frame = NSMakeRect(176, 22, 480, 20);
        hint.textColor = [NSColor secondaryLabelColor];
        [content addSubview:hint];
        [_diagnosticWindow center];
    }
    [self updateDiagnosticText:text];
    [_diagnosticWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)updateDiagnosticText:(NSString*)text
{
    if (!_diagnosticText) return;
    _diagnosticText.string = text;
    [_diagnosticText scrollRangeToVisible:NSMakeRange(0, 0)];
}

- (void)copyDiagnostic:(id)sender
{
    (void)sender;
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    [pasteboard setString:_diagnosticText.string forType:NSPasteboardTypeString];
}

- (void)toggleAutoAuth:(id)sender
{
    (void)sender;
    if (_autoSaving) return;
    const bool next = !Status::Get().autoAuth;
    _autoSaving = YES;
    [self refresh];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool
        {
            std::string error;
            bool saved = false;
            try { saved = Config::WriteAutoAuthEnabled(GetConfigPath(), next, error); }
            catch (...) { error = "保存开关时发生错误，请稍后重试"; }
            dispatch_async(dispatch_get_main_queue(), ^{
                _autoSaving = NO;
                if (saved) Status::SetAutoAuth(next);
                else
                {
                    NSAlert* alert = [[NSAlert alloc] init];
                    alert.messageText = @"无法修改自动认证开关";
                    alert.informativeText = U(error);
                    [alert runModal];
                    [alert release];
                }
                [self refresh];
            });
        }
    });
}

- (void)toggleSpeedDisplay:(id)sender
{
    (void)sender;
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    const BOOL next = ![defaults boolForKey:kShowSpeedKey];
    [defaults setBool:next forKey:kShowSpeedKey];
    [self refresh];
}

- (void)openLogFolder:(id)sender
{
    (void)sender;
    const fs::path folder = GetConfigPath().parent_path() / "logs";
    [[NSFileManager defaultManager] createDirectoryAtPath:U(folder.u8string())
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    [[NSWorkspace sharedWorkspace] openURL:
        [NSURL fileURLWithPath:U(folder.u8string())]];
}

- (void)openDataFolder:(id)sender
{
    (void)sender;
    [[NSWorkspace sharedWorkspace] openURL:
        [NSURL fileURLWithPath:U(GetConfigPath().parent_path().u8string())]];
}

- (void)quit:(id)sender
{
    (void)sender;
    // KeepAlive 用的是 SuccessfulExit=false，正常退出（exit 0）不会被 launchd 拉起。
    [NSApp terminate:nil];
}

@end

namespace
{
    AutoSZUWebStatusController* gController = nil;
}

bool StatusBar::Install()
{
    @autoreleasepool
    {
        NSApplication* application = [NSApplication sharedApplication];
        if (!application)
            return false;

        [application setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [application finishLaunching];

        gController = [[AutoSZUWebStatusController alloc] init];
        [gController install];
    }
    return true;
}

void StatusBar::Run()
{
    @autoreleasepool
    {
        [[NSApplication sharedApplication] run];
    }
}

void StatusBar::ShowDiagnosticWindow()
{
    @autoreleasepool
    {
        NSApplication* application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [application finishLaunching];
        AutoSZUWebStatusController* controller = [[AutoSZUWebStatusController alloc] init];
        [controller showDiagnosticText:@"正在进行只读网络诊断…\n不会提交账号密码、关闭代理或切换网络。"];
        [[NSNotificationCenter defaultCenter] addObserver:controller selector:@selector(quit:)
            name:NSWindowWillCloseNotification object:nil];
        [controller retain];
        std::thread([controller] {
            std::string report;
            try { report = Observation::Diagnose(); }
            catch (...) { report = "诊断未完成，未修改网络或代理设置。请稍后重试。"; }
            dispatch_async(dispatch_get_main_queue(), ^{
                [controller updateDiagnosticText:U(report)];
            });
            [controller release];
        }).detach();
        [application run];
        [[NSNotificationCenter defaultCenter] removeObserver:controller];
        [controller release];
    }
}
