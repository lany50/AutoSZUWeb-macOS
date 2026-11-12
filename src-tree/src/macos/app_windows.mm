#import <AppKit/AppKit.h>
#include "app_windows.h"
#include "settings.h"
#include "event_history.h"
#include "file_path.h"
#include "wake_signal.h"

namespace
{
    NSString* U(const std::string& value) { return [NSString stringWithUTF8String:value.c_str()] ?: @""; }
    NSTextField* Label(NSView* view, NSString* text, NSRect frame, CGFloat size = 13)
    {
        NSTextField* label = [NSTextField labelWithString:text]; label.frame = frame;
        label.font = [NSFont systemFontOfSize:size]; [view addSubview:label]; return label;
    }
    NSButton* Button(NSView* view, NSString* title, NSRect frame, id target, SEL action)
    {
        NSButton* button = [NSButton buttonWithTitle:title target:target action:action];
        button.frame = frame; button.bezelStyle = NSBezelStyleRounded; [view addSubview:button]; return button;
    }
    void Application(bool standalone)
    {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        if (standalone) [app finishLaunching];
    }
}

@interface SZUPreferencesController : NSObject <NSWindowDelegate>
{
    NSWindow* _window;
    NSTextField* _account;
    NSSecureTextField* _password;
    NSSecureTextField* _confirmation;
    NSButton* _autoAuth;
    NSButton* _power;
    NSPopUpButton* _interval;
    NSTextField* _hint;
    NSTextField* _feedback;
    NSButton* _save;
    BOOL _standalone;
    BOOL _busy;
    BOOL _saving;
    unsigned _loadGeneration;
    Settings::Preferences _loadedPreferences;
}
- (void)setBusy:(BOOL)busy;
- (void)applySummary:(const Settings::Summary&)summary;
- (void)show:(BOOL)standalone;
- (void)save:(id)sender;
- (void)cancel:(id)sender;
@end

@implementation SZUPreferencesController
- (void)setBusy:(BOOL)busy
{
    _busy = busy;
    for (NSControl* control in @[_account, _password, _confirmation, _autoAuth, _power, _interval, _save])
        control.enabled = !busy;
    _save.title = _saving ? @"正在保存…" : @"保存设置";
}
- (void)applySummary:(const Settings::Summary&)summary
{
    _loadedPreferences = summary.preferences;
    _hint.stringValue = U(summary.accountHint);
    _autoAuth.state = summary.preferences.autoAuth ? NSControlStateValueOn : NSControlStateValueOff;
    _power.state = summary.preferences.powerSaving ? NSControlStateValueOn : NSControlStateValueOff;
    [_interval selectItemAtIndex:summary.preferences.stableInterval == 30 ? 0 : summary.preferences.stableInterval == 120 ? 2 : 1];
    _save.enabled = summary.valid;
    _feedback.stringValue = summary.valid ? @"" : @"配置不可读，请检查数据目录中的原文件。";
    _feedback.textColor = NSColor.systemRedColor;
}
- (void)show:(BOOL)standalone
{
    _standalone = standalone;
    if (_window.visible || _saving)
    {
        [_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        return;
    }
    if (!_window)
    {
        _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,660,520)
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable)
            backing:NSBackingStoreBuffered defer:NO];
        _window.title = @"AutoSZUWeb · 账号与监测设置"; _window.releasedWhenClosed = NO; _window.delegate = self;
        NSView* view = _window.contentView;
        Label(view,@"账号与监测",NSMakeRect(28,466,600,32),22);
        _hint = Label(view,@"",NSMakeRect(28,432,600,24)); _hint.textColor = NSColor.secondaryLabelColor;
        Label(view,@"校园网账号",NSMakeRect(28,386,130,20));
        _account = [[NSTextField alloc] initWithFrame:NSMakeRect(168,380,460,28)];
        _account.accessibilityLabel = @"校园网账号";
        _account.placeholderString = @"留空保留当前账号"; [view addSubview:_account];
        Label(view,@"新密码",NSMakeRect(28,338,130,20));
        _password = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(168,332,460,28)];
        _password.accessibilityLabel = @"新密码";
        _password.placeholderString = @"留空保留当前密码"; [view addSubview:_password];
        Label(view,@"确认新密码",NSMakeRect(28,290,130,20));
        _confirmation = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(168,284,460,28)];
        _confirmation.accessibilityLabel = @"确认新密码";
        _confirmation.placeholderString = @"再次输入新密码"; [view addSubview:_confirmation];
        _autoAuth = [[NSButton checkboxWithTitle:@"自动认证（仅在确认校园网后）" target:nil action:nil] retain];
        _autoAuth.frame = NSMakeRect(168,234,460,24); [view addSubview:_autoAuth];
        _power = [[NSButton checkboxWithTitle:@"省电监测：稳定时逐步降低探测频率" target:nil action:nil] retain];
        _power.frame = NSMakeRect(168,202,460,24); [view addSubview:_power];
        Label(view,@"稳定检查间隔",NSMakeRect(28,162,130,20));
        _interval = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(168,154,220,30) pullsDown:NO];
        _interval.accessibilityLabel = @"稳定检查间隔";
        [_interval addItemsWithTitles:@[@"30 秒", @"1 分钟（推荐）", @"2 分钟"]]; [view addSubview:_interval];
        NSTextField* note = Label(view,@"空白账号和密码不会覆盖已保存值。保存不会强制重登正常连接；换网、唤醒或手动认证时会立即检查。",NSMakeRect(28,98,600,44));
        note.lineBreakMode = NSLineBreakByWordWrapping; note.textColor = NSColor.secondaryLabelColor;
        _feedback = Label(view,@"",NSMakeRect(28,57,600,34)); _feedback.lineBreakMode = NSLineBreakByWordWrapping;
        NSButton* cancel = Button(view,@"关闭",NSMakeRect(388,16,104,32),self,@selector(cancel:));
        cancel.keyEquivalent = @"\e";
        _save = Button(view,@"保存设置",NSMakeRect(504,16,124,32),self,@selector(save:)); _save.keyEquivalent = @"\r";
        _account.nextKeyView = _password;
        _password.nextKeyView = _confirmation;
        _confirmation.nextKeyView = _autoAuth;
        _autoAuth.nextKeyView = _power;
        _power.nextKeyView = _interval;
        _interval.nextKeyView = _save;
        _window.initialFirstResponder = _account;
        [_window center];
    }
    _account.stringValue = @"";
    _password.stringValue = @"";
    _confirmation.stringValue = @"";
    _hint.stringValue = @"正在读取本机设置…";
    _feedback.stringValue = @"";
    [self setBusy:YES];
    const unsigned generation = ++_loadGeneration;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool
        {
            Settings::Summary summary;
            try { summary = Settings::ReadSummary(GetConfigPath()); }
            catch (...)
            {
                summary.valid = false;
                summary.accountHint = "暂时无法读取本机设置";
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                if (generation != _loadGeneration) return;
                [self setBusy:NO];
                [self applySummary:summary];
                if (_window.visible) [_window makeFirstResponder:_account];
            });
        }
    });
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
- (void)save:(id)sender
{
    (void)sender;
    if (_busy || !_save.enabled) return;
    Settings::PreferencesPatch patch;
    const bool autoAuth = _autoAuth.state == NSControlStateValueOn;
    const bool powerSaving = _power.state == NSControlStateValueOn;
    const int interval = _interval.indexOfSelectedItem == 0 ? 30 : _interval.indexOfSelectedItem == 2 ? 120 : 60;
    if (autoAuth != _loadedPreferences.autoAuth) patch.autoAuth = autoAuth;
    if (powerSaving != _loadedPreferences.powerSaving) patch.powerSaving = powerSaving;
    if (interval != _loadedPreferences.stableInterval) patch.stableInterval = interval;
    const std::string account = _account.stringValue.UTF8String;
    const std::string password = _password.stringValue.UTF8String;
    const std::string confirmation = _confirmation.stringValue.UTF8String;
    const bool credentialsEdited = !account.empty() || !password.empty();
    _saving = YES;
    [self setBusy:YES];
    _feedback.textColor = NSColor.secondaryLabelColor;
    _feedback.stringValue = @"正在保存本机设置…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool
        {
            std::string error;
            bool ok = false;
            try { ok = Settings::SavePatch(GetConfigPath(), account, password, confirmation, patch, error); }
            catch (...) { error = "保存设置时发生错误，请稍后重试"; }
            Settings::Summary summary;
            if (ok)
            {
                try { summary = Settings::ReadSummary(GetConfigPath()); }
                catch (...)
                {
                    summary.valid = false;
                    summary.accountHint = "设置已保存，暂时无法重新读取";
                }
                try
                {
                    History::Record("settings", credentialsEdited ? "账号配置已更新" : "监测设置已更新",
                        "已保存本机配置");
                }
                catch (...) {}
                WakeSignal::Notify(WakeSignal::Settings);
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                _saving = NO;
                [self setBusy:NO];
                if (ok)
                {
                    _account.stringValue = @"";
                    _password.stringValue = @"";
                    _confirmation.stringValue = @"";
                    [self applySummary:summary];
                    _feedback.textColor = NSColor.secondaryLabelColor;
                    _feedback.stringValue = summary.valid ? @"已保存，后台正在应用新设置。" : @"设置已保存，请重新打开窗口查看最新配置。";
                }
                else
                {
                    _feedback.textColor = NSColor.systemRedColor;
                    _feedback.stringValue = U(error);
                }
                if (_standalone && !_window.visible) [NSApp terminate:nil];
            });
        }
    });
}
- (void)cancel:(id)sender { (void)sender; [_window close]; }
- (void)windowWillClose:(NSNotification*)notification
{
    (void)notification;
    ++_loadGeneration;
    _account.stringValue = @""; _password.stringValue = @""; _confirmation.stringValue = @"";
    if (_standalone && !_saving) [NSApp terminate:nil];
}
@end

@interface SZUHistoryController : NSObject <NSWindowDelegate>
{
    NSWindow* _window;
    NSTextView* _text;
    BOOL _standalone;
}
- (void)show:(BOOL)standalone;
- (void)refresh:(id)sender;
- (void)copy:(id)sender;
@end
@implementation SZUHistoryController
- (void)show:(BOOL)standalone
{
    _standalone = standalone;
    if (!_window)
    {
        _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,760,560)
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
            backing:NSBackingStoreBuffered defer:NO];
        _window.title = @"AutoSZUWeb · 最近故障与恢复"; _window.releasedWhenClosed = NO; _window.delegate = self;
        [_window setMinSize:NSMakeSize(540,380)];
        NSView* view = _window.contentView;
        NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(16,60,728,484)];
        scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable; scroll.hasVerticalScroller = YES; scroll.borderType = NSBezelBorder;
        _text = [[NSTextView alloc] initWithFrame:scroll.contentView.bounds];
        _text.editable = NO; _text.selectable = YES; _text.font = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular];
        _text.textColor = NSColor.labelColor; _text.backgroundColor = NSColor.textBackgroundColor;
        _text.textContainerInset = NSMakeSize(14,14); _text.autoresizingMask = NSViewWidthSizable;
        _text.verticallyResizable = YES; _text.horizontallyResizable = NO; _text.textContainer.widthTracksTextView = YES;
        scroll.documentView = _text; [view addSubview:scroll]; [scroll release];
        Button(view,@"刷新记录",NSMakeRect(16,16,112,32),self,@selector(refresh:));
        Button(view,@"复制记录",NSMakeRect(140,16,112,32),self,@selector(copy:));
        NSTextField* hint = Label(view,@"仅保存在本机，不会自动上传",NSMakeRect(276,22,450,20)); hint.textColor = NSColor.secondaryLabelColor;
        [_window center];
    }
    [self refresh:nil]; [_window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
}
- (void)refresh:(id)sender
{ (void)sender; _text.string = U(History::Text(History::DefaultPath())); [_text scrollRangeToVisible:NSMakeRange(0,0)]; }
- (void)copy:(id)sender
{
    (void)sender; NSPasteboard* board = NSPasteboard.generalPasteboard;
    [board clearContents]; [board setString:_text.string forType:NSPasteboardTypeString];
}
- (void)windowWillClose:(NSNotification*)notification
{ (void)notification; if (_standalone) [NSApp terminate:nil]; }
@end

namespace AppWindows
{
    void ShowSettings(bool standalone)
    {
        Application(standalone); static SZUPreferencesController* controller = [[SZUPreferencesController alloc] init];
        [controller show:standalone]; if (standalone) [NSApp run];
    }
    void ShowHistory(bool standalone)
    {
        Application(standalone); static SZUHistoryController* controller = [[SZUHistoryController alloc] init];
        [controller show:standalone]; if (standalone) [NSApp run];
    }
}
