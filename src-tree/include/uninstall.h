#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Uninstall
{
    // Delete application data and legacy desktop templates with a matching origin
    // record. Preserve unrecognized desktop files. Report entries that could not be removed.
    void RemoveSharedData(std::vector<std::filesystem::path>& leftovers);

    // 从本机完全卸载：移除自启动注册 → 停止后台 → 删除共享数据 → 平台善后（钥匙串
    // 密钥、自启动副本、应用包本体等）。尽力而为；仍有残留时返回 false
    // 并通过 error 说明。
    bool RemoveAll(std::string& error);
}
