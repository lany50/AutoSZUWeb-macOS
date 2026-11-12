#pragma once
#include <string>
#include "auth_policy.h"
#include "network_observation.h"

// 教学/办公区 SRun challenge 认证 (net.szu.edu.cn)。
// 成功返回 true; 失败返回 false 并填充 message 描述原因。
bool SrunLogin(const std::string& account, const std::string& password, std::string& message,
               Auth::Failure& failure, const Observation::Campus& context,
               const Operation::Cancelled& cancelled = {});

#ifdef AUTOSZUWEB_AUTH_TESTING
namespace SrunTesting
{
    using HttpTransport = std::function<CURLcode(const std::string&, std::string&)>;
    void SetHttpTransport(HttpTransport transport);
}
#endif
