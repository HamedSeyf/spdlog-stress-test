#pragma once

#include <atomic>

#include "test_base.h"


// ============================================================
// ShutdownTest
// ============================================================
class ShutdownTest final : public TestBase
{
public:
    static constexpr const char* k_name = "shutdown";
    const char* name() const noexcept override { return k_name; }

protected:
    void run(const std::atomic<bool>& /*stop*/, bool /*stress*/) override
    {
        // We do not do anything here! As described in the documentation and in the new architecture: test objects are not allowed to shutdown spdlog. That's moderator's job being main.cpp in this project.
        // The other part of the original code was raw->info("shutdown now"); which is done in a hot loop inside a spawned thread. Again not useful in this case and worth wiping out.
    }
};
