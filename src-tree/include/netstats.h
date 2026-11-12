#pragma once

#include <string>

// 网卡流量计数：仅用于菜单栏展示，不参与认证逻辑。
namespace NetStats
{
    struct Counters
    {
        unsigned long long rxBytes = 0;
        unsigned long long txBytes = 0;
        unsigned long long rxPackets = 0;
        unsigned long long txPackets = 0;
        std::string interfaceName;
        unsigned interfaceIndex = 0;
        long long linkChangedAt = 0;
        bool valid = false;
    };

    struct Rate
    {
        double rx = 0;
        double tx = 0;
    };

    class Sampler
    {
        Counters previous;
        double previousTime = 0;
    public:
        void Reset() { previous = {}; previousTime = 0; }
        Rate Sample(const Counters& current, double steadySeconds)
        {
            Rate rate;
            const double elapsed = steadySeconds - previousTime;
            const bool sameInterface = current.interfaceName == previous.interfaceName &&
                current.interfaceIndex == previous.interfaceIndex && current.linkChangedAt == previous.linkChangedAt;
            const bool countersContinuous = current.rxPackets >= previous.rxPackets && current.txPackets >= previous.txPackets;
            if (current.valid && previous.valid && sameInterface && countersContinuous && elapsed >= 0.2 && elapsed <= 5)
            {
                const unsigned int rx = static_cast<unsigned int>(current.rxBytes) - static_cast<unsigned int>(previous.rxBytes);
                const unsigned int tx = static_cast<unsigned int>(current.txBytes) - static_cast<unsigned int>(previous.txBytes);
                rate.rx = static_cast<double>(rx) / elapsed;
                rate.tx = static_cast<double>(tx) / elapsed;
            }
            previous = current;
            previousTime = steadySeconds;
            return rate;
        }
    };

    // 读取指定网卡的收发字节数（NET_RT_IFLIST2）。
    // 实测：包计数是 64 位，但字节计数即使取 if_data64 仍按 32 位回绕，
    // 因此调用方必须用无符号减法求差值（这样跨回绕依然正确），
    // 不要直接把返回值当作累计流量。
    Counters Read(const std::string& interfaceName = std::string());

    // Uses the same active physical interface selection as campus detection.
    std::string PrimaryInterface();
}
