#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include <cstdio>
#include <cstdlib>

// With RUSQL_TEST_TRACE set, every test case writes its name to stderr when it starts (and flushes): when a run does not come back, the last name in the output is the test that is
// stuck. Without the variable nothing is printed.
namespace {
struct TraceListener : Catch::TestEventListenerBase {
    using Catch::TestEventListenerBase::TestEventListenerBase;
    void testCaseStarting(Catch::TestCaseInfo const& info) override {
        static const bool on = std::getenv("RUSQL_TEST_TRACE") != nullptr;
        if (on) {
            std::fprintf(stderr, "[test] %s\n", info.name.c_str());
            std::fflush(stderr);
        }
    }
};
} // namespace
CATCH_REGISTER_LISTENER(TraceListener)
