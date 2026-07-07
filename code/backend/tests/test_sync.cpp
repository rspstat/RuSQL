#include <memory>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "engine/sync.hpp"

using namespace engine;

TEST_CASE("Mutex<T> guards read/write access", "[sync]") {
    auto m = std::make_shared<Mutex<int>>(0);
    {
        auto g = m->lock();
        *g = 42;
    }
    REQUIRE(*m->lock() == 42);
}

TEST_CASE("Mutex<T> serializes concurrent increments", "[sync]") {
    auto m = std::make_shared<Mutex<int>>(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; i++) {
        threads.emplace_back([m] {
            for (int j = 0; j < 1000; j++) {
                auto g = m->lock();
                *g = *g + 1;
            }
        });
    }
    for (auto& t : threads) t.join();
    REQUIRE(*m->lock() == 8000);
}

TEST_CASE("RwLock<T> allows concurrent reads and exclusive writes", "[sync]") {
    auto rw = std::make_shared<RwLock<int>>(5);
    {
        auto r1 = rw->read();
        auto r2 = rw->read();
        REQUIRE(*r1 == 5);
        REQUIRE(*r2 == 5);
    }
    {
        auto w = rw->write();
        *w = 99;
    }
    REQUIRE(*rw->read() == 99);
}
