#include "catch.hpp"
#include "engine/result.hpp"

using engine::Result;
using engine::StringResult;

TEST_CASE("StringResult Ok/Err carry messages like Rust's Result<String,String>", "[result]") {
    auto ok = StringResult::Ok("done");
    REQUIRE(ok.is_ok());
    REQUIRE_FALSE(ok.is_err());
    REQUIRE(static_cast<bool>(ok));
    REQUIRE(ok.value() == "done");

    auto err = StringResult::Err("boom");
    REQUIRE(err.is_err());
    REQUIRE_FALSE(err.is_ok());
    REQUIRE_FALSE(static_cast<bool>(err));
    REQUIRE(err.error() == "boom");
}

TEST_CASE("Result<void,String> mirrors Rust's Result<(),String>", "[result]") {
    auto ok = Result<void, std::string>::Ok();
    REQUIRE(ok.is_ok());

    auto err = Result<void, std::string>::Err("duplicate table");
    REQUIRE(err.is_err());
    REQUIRE(err.error() == "duplicate table");
}
