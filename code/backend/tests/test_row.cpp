#include "catch.hpp"
#include "engine/row.hpp"

TEST_CASE("Row is a string-to-string map mirroring Rust's HashMap<String,String>", "[row]") {
    engine::Row row;
    row["id"] = "1";
    row["name"] = "Alice";

    REQUIRE(row.size() == 2);
    REQUIRE(row.at("id") == "1");
    REQUIRE(row.at("name") == "Alice");
    REQUIRE(row.find("missing") == row.end());
}
