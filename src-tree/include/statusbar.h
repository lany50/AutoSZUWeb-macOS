#pragma once

// macOS 菜单栏图标（须在主线程调用）。
namespace StatusBar
{
    void ShowDiagnosticWindow();
    // 创建状态栏图标与菜单；无图形会话时返回 false，调用方应退回纯后台模式。
    bool Install();

    // 进入 NSApplication 事件循环（阻塞）。
    void Run();
}
