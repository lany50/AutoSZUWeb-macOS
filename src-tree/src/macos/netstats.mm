#include "netstats.h"
#include "network_observation.h"

#include <cstring>
#include <vector>

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace NetStats
{
    std::string PrimaryInterface()
    {
        return Observation::ReadEnvironment().interfaceName;
    }

    Counters Read(const std::string& interfaceName)
    {
        Counters counters;

        const std::string name = interfaceName.empty()
            ? PrimaryInterface() : interfaceName;
        if (name.empty())
            return counters;

        int mib[6] = {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0};
        size_t length = 0;
        if (sysctl(mib, 6, nullptr, &length, nullptr, 0) != 0 || length == 0)
            return counters;

        std::vector<char> buffer(length);
        if (sysctl(mib, 6, buffer.data(), &length, nullptr, 0) != 0)
            return counters;

        const char* cursor = buffer.data();
        const char* end = cursor + length;
        while (cursor + sizeof(if_msghdr) <= end)
        {
            const if_msghdr* header = reinterpret_cast<const if_msghdr*>(cursor);
            if (header->ifm_msglen < sizeof(if_msghdr) || cursor + header->ifm_msglen > end)
                break;

            if (header->ifm_type == RTM_IFINFO2 && header->ifm_msglen >= sizeof(if_msghdr2))
            {
                const if_msghdr2* info =
                    reinterpret_cast<const if_msghdr2*>(cursor);
                char ifname[IFNAMSIZ] = {};
                if (if_indextoname(info->ifm_index, ifname) && name == ifname)
                {
                    // 注意：字节计数实际仍按 32 位回绕（包计数是 64 位），
                    // 取差值时必须用无符号减法，见 netstats.h 的说明。
                    counters.rxBytes = info->ifm_data.ifi_ibytes;
                    counters.txBytes = info->ifm_data.ifi_obytes;
                    counters.rxPackets = info->ifm_data.ifi_ipackets;
                    counters.txPackets = info->ifm_data.ifi_opackets;
                    counters.interfaceName = name;
                    counters.interfaceIndex = info->ifm_index;
                    counters.linkChangedAt = static_cast<long long>(info->ifm_data.ifi_lastchange.tv_sec) * 1000000 +
                        info->ifm_data.ifi_lastchange.tv_usec;
                    counters.valid = true;
                    break;
                }
            }
            cursor += header->ifm_msglen;
        }
        return counters;
    }
}
