#include <string>
#include <vector>

#include "catch.hpp"

#include <nlohmann/json.hpp>
#include <lz4/lz4.h>
#include <sha1/sha1.hpp>
#include <picosha2/picosha2.h>

// Smoke tests confirming each vendored dependency (see cpp/third_party/NOTICE.md)
// is wired into the build correctly, using well-known test vectors where available.

TEST_CASE("nlohmann::json basic round trip", "[vendored][json]") {
    nlohmann::json j;
    j["name"] = "Alice";
    j["age"] = 30;

    auto s = j.dump();
    auto parsed = nlohmann::json::parse(s);

    REQUIRE(parsed["name"] == "Alice");
    REQUIRE(parsed["age"] == 30);
}

TEST_CASE("lz4 compress/decompress round trip", "[vendored][lz4]") {
    const std::string input =
        "The quick brown fox jumps over the lazy dog. "
        "The quick brown fox jumps over the lazy dog.";

    std::vector<char> compressed(static_cast<size_t>(LZ4_compressBound(static_cast<int>(input.size()))));
    const int compressedSize = LZ4_compress_default(
        input.data(), compressed.data(), static_cast<int>(input.size()), static_cast<int>(compressed.size()));
    REQUIRE(compressedSize > 0);

    std::vector<char> decompressed(input.size());
    const int decompressedSize = LZ4_decompress_safe(
        compressed.data(), decompressed.data(), compressedSize, static_cast<int>(decompressed.size()));

    REQUIRE(decompressedSize == static_cast<int>(input.size()));
    REQUIRE(std::string(decompressed.data(), static_cast<size_t>(decompressedSize)) == input);
}

TEST_CASE("sha1 matches FIPS 180-1 test vector for \"abc\"", "[vendored][sha1]") {
    SHA1 sha1;
    sha1.update("abc");
    REQUIRE(sha1.final() == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST_CASE("sha256 (picosha2) matches FIPS 180-2 test vector for \"abc\"", "[vendored][sha256]") {
    std::string hex;
    picosha2::hash256_hex_string(std::string("abc"), hex);
    REQUIRE(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
