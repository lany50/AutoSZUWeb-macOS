#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace EPortal
{
    // Dr.COM ePortal uses result=1 for success, result=0 for rejection.
    // Never let a rejection with result=0 become a successful login.
    inline bool LoginSucceeded(const nlohmann::json& response)
    {
        const auto it = response.find("result");
        if (it != response.end())
        {
            if (it->is_number_integer())
                return *it == 1;
            if (it->is_string())
                return it->get<std::string>() == "1";
            return false;
        }
        const auto message = response.find("msg");
        return message != response.end() && message->is_string() &&
            message->get<std::string>() == "认证成功";
    }
}
