#pragma once

#include <functional>

namespace Operation
{
    using Cancelled = std::function<bool()>;
}
