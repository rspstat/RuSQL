// The cache of UNIQUE values (engine/unique_probe.hpp): lets a one-row INSERT skip the scan that looks for a row already holding its UNIQUE value or composite key.

#include <cstdlib>
#include <stdexcept>

#include "engine/executor/executor.hpp"
#include "engine/unique_probe.hpp"

namespace engine {

UniqueProbes::UniqueProbes() {
    if (const char* e = std::getenv("RUSQL_UNIQUE_PROBE")) enabled = std::string(e) != "0";
    if (const char* e = std::getenv("RUSQL_PROBE_CHECK")) check = std::string(e) == "1";
    if (const char* e = std::getenv("RUSQL_PROBE_STREAK")) streak_needed = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
}

namespace {
using ValueOf = std::function<const std::string*(const std::string&)>;
const std::vector<Row> kNoRows;

// The keys a table is probed by: every UNIQUE column that is not the primary key (that has its tree), a composite primary key, every UNIQUE (a, b).
std::vector<UniqueProbeKeySpec> specs_of(const TableSchema& schema) {
    std::vector<UniqueProbeKeySpec> specs;
    for (auto& col : schema.columns) {
        if (col.unique && !col.primary_key) specs.push_back({{col.name}, false, true});
    }
    if (schema.primary_key_columns.size() > 1) specs.push_back({schema.primary_key_columns, true, false});
    for (auto& key : schema.unique_keys) specs.push_back({key.columns, true, true});
    return specs;
}

std::string signature_of(const std::vector<UniqueProbeKeySpec>& specs) {
    std::string out;
    for (auto& spec : specs) {
        out += spec.tuple ? 'T' : 'C';
        out += spec.skip_null ? 'n' : 'p';
        for (auto& c : spec.columns) out += c + '\x1f';
        out += '\x1e';
    }
    return out;
}

// The key of a row under a spec as the scans compare it: the collation key of the value, a tuple's parts length-prefixed. nullopt: a NULL in a key that never repeats.
std::optional<std::string> key_of(const UniqueProbeKeySpec& spec, const ValueOf& value_of) {
    std::string encoded;
    for (auto& column : spec.columns) {
        const std::string* v = value_of(column);
        if (!v) {
            if (spec.skip_null) return std::nullopt;
            static const std::string empty;
            v = &empty;
        }
        if (spec.skip_null && *v == EXECUTOR_NULL_VALUE) return std::nullopt;
        if (!spec.tuple) return collation::key(*v);
        const std::string part = collation::key(*v);
        encoded += std::to_string(part.size());
        encoded += ':';
        encoded += part;
    }
    return encoded;
}

ValueOf row_values(const Row& row) {
    return [&row](const std::string& c) -> const std::string* {
        auto it = row.find(c);
        return it == row.end() ? nullptr : &it->second;
    };
}

using Live = bool (*)(const Row&); // Executor::is_visible: a row is live while no transaction has ended it
void count_rows(const UniqueProbe& p, const std::vector<Row>& rows, std::vector<std::unordered_map<std::string, std::uint32_t>>& counts, Live live) {
    counts.assign(p.specs.size(), {});
    for (auto& row : rows) {
        if (!live(row)) continue;
        for (std::size_t i = 0; i < p.specs.size(); i++) {
            if (auto key = key_of(p.specs[i], row_values(row))) counts[i][*key]++;
        }
    }
}
} // namespace

// How the locks that protect `table` stand: nobody can change the numbers while the caller holds any of them (a lookup holds the data lock shared, the INSERT's tail
// exclusive). Every statement that writes takes one of the three exclusively, so a stamp that has not moved means no other writer has been at the table.
bool Executor::unique_probe_stamp(SharedDatabase& s, const std::string& table, std::uint64_t& locks_id, std::uint64_t& data_id, std::uint64_t& sum) const {
    auto locks = s.table_locks.find(table);
    auto data = s.table_data_locks.find(table);
    if (locks == s.table_locks.end() || data == s.table_data_locks.end() || !locks->second || !data->second) return false;
    locks_id = locks->second->instance_id();
    data_id = data->second->instance_id();
    sum = shared->exclusive_count() + locks->second->exclusive_count() + data->second->exclusive_count();
    return true;
}

namespace {
UniqueProbe* probe_for(UniqueProbes& all, const std::string& table, const TableSchema& schema) {
    auto specs = specs_of(schema);
    if (specs.empty()) return nullptr;
    UniqueProbe& p = all.by_table[table];
    std::string signature = signature_of(specs);
    if (p.signature != signature) { // new table, or the keys of the table have changed
        p = UniqueProbe{};
        p.specs = std::move(specs);
        p.signature = std::move(signature);
    }
    return &p;
}

void check_against_rows(UniqueProbe& p, const std::vector<Row>* rows, const std::string& table, Live live) {
    std::vector<std::unordered_map<std::string, std::uint32_t>> fresh;
    count_rows(p, rows ? *rows : kNoRows, fresh, live);
    if (fresh != p.counts) throw std::logic_error("the cache of UNIQUE values of '" + table + "' is not what the rows hold");
}
} // namespace

bool Executor::unique_probe_proves_absent(SharedDatabase& s, const std::string& table, const TableSchema& schema, const std::vector<std::string>& columns, bool tuple,
                                          bool skip_null, const std::function<const std::string*(const std::string&)>& value_of) {
    UniqueProbes& all = *s.unique_probes;
    if (!all.enabled) return false;
    std::uint64_t locks_id = 0, data_id = 0, sum = 0;
    if (!unique_probe_stamp(s, table, locks_id, data_id, sum)) return false;
    std::lock_guard<std::mutex> guard(all.mutex);
    UniqueProbe* p = probe_for(all, table, schema);
    if (!p) return false;
    std::size_t which = p->specs.size();
    for (std::size_t i = 0; i < p->specs.size(); i++) {
        auto& spec = p->specs[i];
        if (spec.tuple == tuple && spec.skip_null == skip_null && spec.columns == columns) which = i;
    }
    if (which == p->specs.size()) return false;
    const auto rows_it = s.tables.find(table);
    const std::vector<Row>* rows = rows_it == s.tables.end() ? nullptr : &rows_it->second;
    const bool current = p->built && p->locks_id == locks_id && p->data_id == data_id && p->sum == sum;
    if (!current) {
        // Only INSERTs since the last look (the stamp moved once for each statement that wrote): they are on their way to a table worth probing.
        const bool only_inserts = p->seen && p->seen_locks_id == locks_id && p->seen_data_id == data_id && sum - p->seen_sum == p->insert_tails - p->seen_insert_tails;
        p->streak = only_inserts ? p->streak + 1 : 0;
        p->seen = true;
        p->seen_locks_id = locks_id;
        p->seen_data_id = data_id;
        p->seen_sum = sum;
        p->seen_insert_tails = p->insert_tails;
        if (p->streak < all.streak_needed) return false;
        count_rows(*p, rows ? *rows : kNoRows, p->counts, &Executor::is_visible);
        p->built = true;
        p->locks_id = locks_id;
        p->data_id = data_id;
        p->sum = sum;
        p->streak = 0;
        all.builds++;
    } else if (all.check) {
        check_against_rows(*p, rows, table, &Executor::is_visible);
    }
    auto key = key_of(p->specs[which], value_of);
    const bool absent = !key || p->counts[which].find(*key) == p->counts[which].end(); // (a NULL in the key: nothing can repeat it)
    if (absent) all.skipped++;
    return absent;
}

// The INSERT has the table's data lock exclusive and is about to append `rows` (every row it appends, none that it skipped). The cache that was current before this
// statement took the lock takes the rows in; any other cache is dropped.
void Executor::unique_probe_inserted(SharedDatabase& s, const std::string& table, const TableSchema& schema, const std::vector<Row>& rows) {
    UniqueProbes& all = *s.unique_probes;
    if (!all.enabled) return;
    std::uint64_t locks_id = 0, data_id = 0, sum = 0;
    if (!unique_probe_stamp(s, table, locks_id, data_id, sum)) return;
    std::lock_guard<std::mutex> guard(all.mutex);
    UniqueProbe* p = probe_for(all, table, schema);
    if (!p) return;
    p->insert_tails++;
    if (!p->built) return;
    if (p->locks_id == locks_id && p->data_id == data_id && p->sum + 1 == sum) { // nobody but this statement has taken a lock since the cache was current
        for (auto& row : rows) {
            for (std::size_t i = 0; i < p->specs.size(); i++) {
                if (auto key = key_of(p->specs[i], row_values(row))) p->counts[i][*key]++;
            }
        }
        p->sum = sum;
        if (all.check) {
            // (the rows are not in the table yet: count them in a copy of what it holds)
            const auto rows_it = s.tables.find(table);
            std::vector<Row> everything = rows_it == s.tables.end() ? std::vector<Row>{} : rows_it->second;
            everything.insert(everything.end(), rows.begin(), rows.end());
            check_against_rows(*p, &everything, table, &Executor::is_visible);
        }
    } else {
        p->built = false;
        p->counts.clear();
    }
}

} // namespace engine
