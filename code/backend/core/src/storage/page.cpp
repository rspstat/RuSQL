#include "engine/storage/page.hpp"

#include <cstring>

namespace engine {

namespace {
void push_le32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    for (int i = 0; i < 4; i++) buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void push_le16(std::vector<std::uint8_t>& buf, std::uint16_t v) {
    for (int i = 0; i < 2; i++) buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
std::uint32_t read_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t read_le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1] << 8);
}
} // namespace

std::vector<std::uint8_t> PageHeader::to_bytes() const {
    std::vector<std::uint8_t> buf;
    buf.reserve(32);
    push_le32(buf, magic);   // 0..4
    push_le16(buf, version); // 4..6
    buf.push_back(flags);   // 6
    buf.push_back(0);       // 7 (padding)
    push_le32(buf, row_count);  // 8..12
    push_le32(buf, page_count); // 12..16
    buf.resize(32, 0);          // 16..32 reserved
    return buf; // 32 bytes
}

std::optional<PageHeader> PageHeader::from_bytes(const std::uint8_t* bytes, std::size_t len) {
    if (len < 32) return std::nullopt;
    PageHeader h;
    h.magic = read_le32(bytes);
    if (h.magic != MAGIC) return std::nullopt;
    h.version = read_le16(bytes + 4);
    h.flags = bytes[6];
    h.row_count = read_le32(bytes + 8);
    h.page_count = read_le32(bytes + 12);
    return h;
}

} // namespace engine
