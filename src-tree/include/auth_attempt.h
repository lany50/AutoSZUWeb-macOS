#pragma once

#include "single_instance.h"
#include "status.h"

namespace Auth
{
    // Owned until portal confirmation, authentication and connectivity verification finish.
    class Attempt
    {
        Runtime::SingleInstanceGuard processLock;
        bool acquired = false;
    public:
        explicit Attempt(const std::filesystem::path& directory)
            : processLock(directory / "authentication.lock")
        {
            acquired = processLock.IsPrimary() && Status::TryBeginAuth();
        }
        ~Attempt() { if (acquired) Status::EndAuth(); }
        bool Acquired() const { return acquired; }
    };
}
