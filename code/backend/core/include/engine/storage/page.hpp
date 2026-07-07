#pragma once

// Faithful port of rusql-core/src/storage/page.rs.

#include <cstdint>
#include <optional>
#include <vector>

namespace engine {

constexpr std::size_t PAGE_SIZE = 16384;
constexpr std::uint32_t MAGIC = 0x52444200; // "RDB\0"
constexpr std::uint16_t VERSION = 1;
constexpr std::uint8_t FLAG_COMPRESSED = 0x01; // LZ4 compression

struct PageHeader {
    std::uint32_t magic = MAGIC;
    std::uint16_t version = VERSION;
    std::uint8_t flags = 0;
    std::uint32_t row_count = 0;
    std::uint32_t page_count = 0;

    static PageHeader make() { return PageHeader{}; }

    std::vector<std::uint8_t> to_bytes() const;
    static std::optional<PageHeader> from_bytes(const std::uint8_t* bytes, std::size_t len);

    bool is_compressed() const { return (flags & FLAG_COMPRESSED) != 0; }
};

} // namespace engine
