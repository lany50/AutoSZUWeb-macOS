#include "network_observation.h"

#include <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

namespace Observation
{
    namespace
    {
        std::string String(CFTypeRef value)
        {
            if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
            char buffer[1024] = {};
            return CFStringGetCString(static_cast<CFStringRef>(value), buffer,
                sizeof(buffer), kCFStringEncodingUTF8) ? buffer : "";
        }
        CFDictionaryRef Dictionary(SCDynamicStoreRef store, CFStringRef key)
        {
            CFPropertyListRef value = SCDynamicStoreCopyValue(store, key);
            if (value && CFGetTypeID(value) == CFDictionaryGetTypeID())
                return static_cast<CFDictionaryRef>(value);
            if (value) CFRelease(value);
            return nullptr;
        }
    }

    Environment ReadEnvironment()
    {
        Environment env;
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) == 0)
        {
            for (auto* item = list; item; item = item->ifa_next)
            {
                if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
                    !(item->ifa_flags & IFF_UP) || !(item->ifa_flags & IFF_RUNNING) ||
                    (item->ifa_flags & IFF_LOOPBACK)) continue;
                const std::string name = item->ifa_name;
                if (name.rfind("en", 0) != 0) continue;
                auto* address = reinterpret_cast<sockaddr_in*>(item->ifa_addr);
                const auto ip = ntohl(address->sin_addr.s_addr);
                if (ip == 0 || (ip >> 16) == 0xa9fe) continue;
                char buffer[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer));
                if (env.localIp.empty()) { env.interfaceName = name; env.localIp = buffer; }
            }
            freeifaddrs(list);
        }
        SCDynamicStoreRef store = SCDynamicStoreCreate(nullptr, CFSTR("AutoSZUWeb"), nullptr, nullptr);
        if (!store) return env;
        auto* global = Dictionary(store, CFSTR("State:/Network/Global/IPv4"));
        if (global)
        {
            env.defaultInterface = String(CFDictionaryGetValue(global, CFSTR("PrimaryInterface")));
            env.tunnelDefault = env.defaultInterface.rfind("utun", 0) == 0 ||
                env.defaultInterface.rfind("ppp", 0) == 0 ||
                env.defaultInterface.rfind("ipsec", 0) == 0;
            CFRelease(global);
        }
        auto* proxies = SCDynamicStoreCopyProxies(store);
        if (proxies)
        {
            env.systemProxyKnown = true;
            const CFStringRef switches[] = {kSCPropNetProxiesHTTPEnable, kSCPropNetProxiesHTTPSEnable,
                kSCPropNetProxiesSOCKSEnable, kSCPropNetProxiesProxyAutoConfigEnable,
                kSCPropNetProxiesProxyAutoDiscoveryEnable};
            for (const auto key : switches)
            {
                auto* value = CFDictionaryGetValue(proxies, key);
                int enabled = 0;
                if (value && CFGetTypeID(value) == CFNumberGetTypeID())
                    CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &enabled);
                env.systemProxyConfigured |= enabled != 0;
            }
            CFRelease(proxies);
        }
        // Prefer the configured physical service order when multiple adapters are up.
        SCPreferencesRef prefs = SCPreferencesCreate(nullptr, CFSTR("AutoSZUWeb"), nullptr);
        SCNetworkSetRef set = prefs ? SCNetworkSetCopyCurrent(prefs) : nullptr;
        CFArrayRef order = set ? SCNetworkSetGetServiceOrder(set) : nullptr;
        if (order)
        {
            for (CFIndex i = 0; i < CFArrayGetCount(order); ++i)
            {
                auto service = static_cast<CFStringRef>(CFArrayGetValueAtIndex(order, i));
                auto key = CFStringCreateWithFormat(nullptr, nullptr,
                    CFSTR("State:/Network/Service/%@/IPv4"), service);
                auto* ipv4 = Dictionary(store, key);
                CFRelease(key);
                if (!ipv4) continue;
                const auto name = String(CFDictionaryGetValue(ipv4, CFSTR("InterfaceName")));
                const auto addresses = static_cast<CFArrayRef>(CFDictionaryGetValue(ipv4, CFSTR("Addresses")));
                if (name.rfind("en", 0) == 0 && addresses &&
                    CFGetTypeID(addresses) == CFArrayGetTypeID() && CFArrayGetCount(addresses) > 0)
                {
                    const auto address = String(CFArrayGetValueAtIndex(addresses, 0));
                    if (!address.empty() && address.rfind("169.254.", 0) != 0)
                    {
                        env.interfaceName = name; env.localIp = address;
                        env.router = String(CFDictionaryGetValue(ipv4, CFSTR("Router")));
                        auto dnsKey = CFStringCreateWithFormat(nullptr, nullptr,
                            CFSTR("State:/Network/Service/%@/DNS"), service);
                        auto* dns = Dictionary(store, dnsKey);
                        CFRelease(dnsKey);
                        if (dns)
                        {
                            auto servers = static_cast<CFArrayRef>(CFDictionaryGetValue(dns, CFSTR("ServerAddresses")));
                            if (servers && CFGetTypeID(servers) == CFArrayGetTypeID())
                                for (CFIndex j = 0; j < CFArrayGetCount(servers); ++j)
                                    env.dnsServers.push_back(String(CFArrayGetValueAtIndex(servers, j)));
                            CFRelease(dns);
                        }
                        CFRelease(ipv4);
                        break;
                    }
                }
                CFRelease(ipv4);
            }
        }
        if (set) CFRelease(set);
        if (prefs) CFRelease(prefs);
        CFRelease(store);
        return env;
    }
}
