#pragma once

#include <string>

// 系统通知：只在 macOS 实现，其它平台为空实现。
namespace Notify
{
    // 申请通知权限，主线程调用一次即可（重复调用无副作用）。
    void Setup();

    // 弹一条通知。sound=true 时带提示音。可从任意线程调用。
    void Post(const std::string& title, const std::string& body, bool sound);
}
