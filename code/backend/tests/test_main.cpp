#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

// With RUSQL_TEST_TRACE set, every test case writes its name to stderr when it starts (and flushes): when a run does not come back, the last name in the output is the test that is
// stuck; a test that took two seconds or more says how long. Without the variable nothing is printed.
namespace {
struct TraceListener : Catch::TestEventListenerBase {
    using Catch::TestEventListenerBase::TestEventListenerBase;

    void testCaseStarting(Catch::TestCaseInfo const& info) override {
        if (!enabled()) return;
        std::fprintf(stderr, "[test] %s\n", info.name.c_str());
        std::fflush(stderr);
        started_ = std::chrono::steady_clock::now();
    }

    void testCaseEnded(Catch::TestCaseStats const& stats) override {
        if (!enabled()) return;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
        if (ms >= 2000) {
            std::fprintf(stderr, "[slow] %lld ms: %s\n", static_cast<long long>(ms), stats.testInfo.name.c_str());
            std::fflush(stderr);
        }
    }

private:
    static bool enabled() {
        static const bool on = std::getenv("RUSQL_TEST_TRACE") != nullptr;
        return on;
    }
    std::chrono::steady_clock::time_point started_;
};
} // namespace
CATCH_REGISTER_LISTENER(TraceListener)
