#include "engine/transaction/redo_log.hpp"

#include <filesystem>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace engine {

namespace {
void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; i++) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void put_str(std::vector<std::uint8_t>& b, const std::string& s) {
    put_u32(b, static_cast<std::uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
bool get_u32(const std::vector<std::uint8_t>& b, std::size_t& pos, std::uint32_t& out) {
    if (pos + 4 > b.size()) return false;
    out = 0;
    for (int i = 0; i < 4; i++) out |= static_cast<std::uint32_t>(b[pos + static_cast<std::size_t>(i)]) << (8 * i);
    pos += 4;
    return true;
}
bool get_str(const std::vector<std::uint8_t>& b, std::size_t& pos, std::string& out) {
    std::uint32_t len;
    if (!get_u32(b, pos, len) || pos + len > b.size()) return false;
    out.assign(reinterpret_cast<const char*>(b.data() + pos), len);
    pos += len;
    return true;
}
std::uint32_t fnv1a(const std::uint8_t* d, std::size_t n) {
    std::uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < n; i++) {
        h ^= d[i];
        h *= 16777619u;
    }
    return h;
}
} // namespace

RedoLog::~RedoLog() {
    if (fp_) std::fclose(fp_);
}

std::uint64_t RedoLog::append_batch(const std::vector<RedoOp>& ops) {
    if (ops.empty()) return appended_.load();
    std::vector<std::uint8_t> payload;
    put_u32(payload, static_cast<std::uint32_t>(ops.size()));
    for (auto& op : ops) {
        payload.push_back(static_cast<std::uint8_t>(op.kind));
        put_str(payload, op.table);
        put_str(payload, op.row_json);
        put_str(payload, op.xmax);
    }
    std::vector<std::uint8_t> rec;
    put_u32(rec, static_cast<std::uint32_t>(payload.size()));
    rec.insert(rec.end(), payload.begin(), payload.end());
    put_u32(rec, fnv1a(payload.data(), payload.size()));

    std::lock_guard<std::mutex> g(mu_);
    if (!fp_) {
        fp_ = std::fopen(path_.c_str(), "ab");
        if (!fp_) throw std::runtime_error("redo 로그 파일 열기 실패");
        if (!size_known_) {
            std::error_code ec;
            auto sz = fs::file_size(path_, ec);
            bytes_ = ec ? 0 : static_cast<std::uint64_t>(sz);
            size_known_ = true;
            bytes_ -= 0;
        }
    }
    if (std::fwrite(rec.data(), 1, rec.size(), fp_) != rec.size() || std::fflush(fp_) != 0) {
        std::fclose(fp_);
        fp_ = nullptr;
        throw std::runtime_error("redo 로그 기록 실패");
    }
    bytes_ += rec.size();
    return ++appended_;
}

void RedoLog::sync(std::uint64_t seq) {
    std::lock_guard<std::mutex> g(sync_mu_);
    if (synced_.load() >= seq) return; // a concurrent committer's fsync already covered this batch
    std::uint64_t target = appended_.load();
    std::FILE* fp;
    {
        std::lock_guard<std::mutex> g2(mu_);
        fp = fp_;
    }
    if (!fp) return;
#ifdef _WIN32
    bool ok = _commit(_fileno(fp)) == 0;
#else
    bool ok = fsync(fileno(fp)) == 0;
#endif
    if (!ok) throw std::runtime_error("redo 로그 fsync 실패");
    synced_ = target;
}

std::vector<RedoOp> RedoLog::read_all() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<RedoOp> out;
    std::ifstream f(path_, std::ios::binary);
    if (!f) return out;
    std::vector<std::uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::size_t pos = 0;
    while (pos < buf.size()) {
        std::uint32_t len;
        if (!get_u32(buf, pos, len) || pos + len + 4 > buf.size()) break;
        std::size_t start = pos;
        std::uint32_t stored = 0;
        std::size_t cpos = pos + len;
        if (!get_u32(buf, cpos, stored) || fnv1a(buf.data() + start, len) != stored) break; // torn/corrupt tail
        std::uint32_t count;
        std::size_t p = start;
        if (!get_u32(buf, p, count)) break;
        std::vector<RedoOp> batch;
        bool ok = true;
        for (std::uint32_t i = 0; i < count && ok; i++) {
            if (p >= start + len) { ok = false; break; }
            RedoOp op;
            op.kind = static_cast<RedoOp::Kind>(buf[p++]);
            ok = get_str(buf, p, op.table) && get_str(buf, p, op.row_json) && get_str(buf, p, op.xmax);
            batch.push_back(std::move(op));
        }
        if (!ok) break;
        out.insert(out.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
        pos = cpos;
    }
    return out;
}

void RedoLog::clear() {
    std::lock_guard<std::mutex> g(mu_);
    if (fp_) std::fclose(fp_);
    fp_ = nullptr;
    std::error_code ec;
    fs::remove(path_, ec);
    bytes_ = 0;
    size_known_ = true;
    synced_ = appended_.load();
}

} // namespace engine
