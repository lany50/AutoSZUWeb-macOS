#pragma once

#include <string>

// 探测指定 IPv4 TCP 端点；主要用于 NetworkCheck 和回环测试。
bool ProbeTcpEndpoint(const std::string& ip, int port, int timeoutMs = 2000);

// 多目标 HTTP 204 验证外网；不会因为 DNS 端口可达就报告在线。
bool NetworkCheck(int timeoutMs = 2000);

// 解析主机名并尝试 TCP 连接，用于快速判断某认证站点当前是否可达（不可达则跳过整套认证）。
bool HostReachable(const std::string& host, int port, int timeoutMs = 1200);

// 取得优先物理网卡的 IPv4，排除代理/VPN 虚拟地址；失败返回空字符串。
std::string GetLocalIp();
