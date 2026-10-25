// The cache of UNIQUE values (engine/unique_probe.hpp): lets a one-row INSERT skip the scan that looks for a row already holding its UNIQUE value or composite key.

#include <algorithm>
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

// The keys a table is probed by: every UNIQUE column that is not the primary key (that has its tree), a composite primary key, every UNIQUE (a, b), and -- for the parent
// side of a foreign key (fk_children_absent) -- every column that refers to another table (a value that may repeat: the cache counts the rows).
std::vector<UniqueProbeKeySpec> specs_of(const TableSchema& schema) {
    std::vector<UniqueProbeKeySpec> specs;
    for (auto& col : schema.columns) {
        if (col.unique && !col.primary_key) specs.push_back({{col.name}, false, true});
    }
    if (schema.primary_key_columns.size() > 1) specs.push_back({schema.primary_key_columns, true, false});
    for (auto& key : schema.unique_keys) specs.push_back({key.columns, true, true});
    for (auto& col : schema.columns) {
        if (!col.foreign_key) continue;
        const bool listed = std::any_of(specs.begin(), specs.end(), [&](const UniqueProbeKeySpec& s) { return !s.tuple && s.skip_null && s.columns.size() == 1 && s.columns[0] == col.name; });
        if (!listed) specs.push_back({{col.name}, false, true});
    }
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
// The entry of `table`. A cache that is built is returned as it is: after any DDL (a changed schema included) its stamp no longer fits, so the keys it was made for cannot be
// out of date while it is used. An entry that is not built looks at the schema again (`create`: and makes the entry when the table has keys to probe).
UniqueProbe* probe_for(UniqueProbes& all, const std::string& table, const TableSchema& schema, bool create = true) {
    if (auto it = all.by_table.find(table); it != all.by_table.end() && it->second.built) return &it->second;
    auto specs = specs_of(schema);
    if (specs.empty()) return nullptr;
    if (!create && !all.by_table.count(table)) return nullptr;
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
                                          bool skip_null, const std::function<const std::string*(const std::string&)>& value_of, ProbeUse use) {
    UniqueProbes& all = *s.unique_probes;
    if (!all.enabled) return false;
    std::uint64_t locks_id = 0, data_id = 0, sum = 0;
    if (!unique_probe_stamp(s, table, locks_id, data_id, sum)) return false;
    std::lock_guard<std::mutex> guard(all.mutex);
    UniqueProbe* p = probe_for(all, table, schema, /*create=*/use == ProbeUse::Lookup);
    if (!p || (use != ProbeUse::Lookup && !p->built)) return false;
    std::size_t which = p->specs.size();
    for (std::size_t i = 0; i < p->specs.size(); i++) {
        auto& spec = p->specs[i];
        if (spec.tuple == tuple && spec.skip_null == skip_null && spec.columns == columns) which = i;
    }
    if (which == p->specs.size()) return false;
    const auto rows_it = s.tables.find(table);
    const std::vector<Row>* rows = rows_it == s.tables.end() ? nullptr : &rows_it->second;
    // (inside the statement's own exclusive section the lock has been taken once already: the cache is current when nothing else has moved since)
    const bool current = p->built && p->locks_id == locks_id && p->data_id == data_id && (p->sum == sum || (use == ProbeUse::InsideExclusive && p->sum + 1 == sum));
    if (!current) {
        if (use != ProbeUse::Lookup) return false; // (a statement of several rows, or one that holds the table exclusively, does not make the cache)
        // Only INSERTs since the last look (the stamp moved once for each statement that wrote): they are on their way to a table worth probing.
        const bool only_inserts = p->seen && p->seen_locks_id == locks_id && p->seen_data_id == data_id && sum - p->seen_sum == p->insert_tails - p->seen_insert_tails;
        p->streak = only_inserts ? p->streak + 1 : 0;
        p->seen = true;
        p->seen_locks_id = locks_id;
        p->seen_data_id = data_id;
        p->seen_sum = sum;
        p->seen_insert_tails = p->insert_tails;
        if (p->streak < all.streak_needed) return false;
        // (the keys the cache is made for are the schema's of now: after a DDL they may be others)
        if (auto fresh = specs_of(schema); signature_of(fresh) != p->signature) {
            const auto tails = p->insert_tails;
            *p = UniqueProbe{};
            p->signature = signature_of(fresh);
            p->specs = std::move(fresh);
            p->insert_tails = tails;
            which = p->specs.size();
            for (std::size_t i = 0; i < p->specs.size(); i++) {
                auto& spec = p->specs[i];
                if (spec.tuple == tuple && spec.skip_null == skip_null && spec.columns == columns) which = i;
            }
            if (which == p->specs.size()) return false;
        }
        count_rows(*p, rows ? *rows : kNoRows, p->counts, &Executor::is_visible);
        p->built = true;
        p->locks_id = locks_id;
        p->data_id = data_id;
        p->sum = sum;
        p->streak = 0;
        all.builds++;
    } else if (all.check && use != ProbeUse::InsideExclusive) {
        check_against_rows(*p, rows, table, &Executor::is_visible);
    }
    auto key = key_of(p->specs[which], value_of);
    const bool absent = !key || p->counts[which].find(*key) == p->counts[which].end(); // (a NULL in the key: nothing can repeat it)
    if (absent) all.skipped++;
    return absent;
}

// The parent side of a foreign key: a DELETE / UPDATE of a parent row asks (holding the child's data lock exclusively, the statement's own section) whether any live row of the
// child table holds the value. A child table is scanned once for each parent row otherwise: O(rows of the child) even for a parent that nothing refers to, and an index on the
// column cannot rule a value out. The cache is the one of the UNIQUE values (stamped by the locks, kept by the writers that know what they change), made on the second parent
// statement that finds none current, and only for a table large enough for a scan to cost something.
bool Executor::fk_children_absent(SharedDatabase& s, const std::string& child_table, const std::string& column, const std::string& value) {
    if (value == EXECUTOR_NULL_VALUE) return true; // (a NULL refers to nothing)
    UniqueProbes& all = *s.unique_probes;
    if (!all.enabled) return false;
    const TableSchema* schema = s.catalog.get_table(child_table);
    const auto rows_it = s.tables.find(child_table);
    if (!schema || rows_it == s.tables.end()) return false;
    std::uint64_t locks_id = 0, data_id = 0, sum = 0;
    if (!unique_probe_stamp(s, child_table, locks_id, data_id, sum)) return false;
    std::lock_guard<std::mutex> guard(all.mutex);
    UniqueProbe* p = probe_for(all, child_table, *schema);
    if (!p) return false;
    std::size_t which = p->specs.size();
    for (std::size_t i = 0; i < p->specs.size(); i++) {
        auto& spec = p->specs[i];
        if (!spec.tuple && spec.skip_null && spec.columns.size() == 1 && spec.columns[0] == column) which = i;
    }
    if (which == p->specs.size()) return false;
    // (this statement holds the child's data lock exclusive: it has been counted once)
    const bool current = p->built && p->locks_id == locks_id && p->data_id == data_id && (p->sum == sum || p->sum + 1 == sum);
    if (!current) {
        constexpr std::size_t kMinRows = 1024; // (a scan of fewer rows costs less than counting them)
        if (rows_it->second.size() < kMinRows || ++p->child_streak < 2) return false;
        count_rows(*p, rows_it->second, p->counts, &Executor::is_visible);
        p->built = true;
        p->locks_id = locks_id;
        p->data_id = data_id;
        p->sum = sum;
        p->child_streak = 0;
        all.builds++;
    } else if (all.check) {
        check_against_rows(*p, &rows_it->second, child_table, &Executor::is_visible);
    }
    ValueOf of = [&](const std::string&) -> const std::string* { return &value; };
    auto key = key_of(p->specs[which], of);
    const bool absent = !key || p->counts[which].find(*key) == p->counts[which].end(); // (a NULL refers to nothing)
    if (absent) all.skipped++;
    return absent;
}

// A statement that holds the table's data lock exclusively changes the live rows: `removed` stop being live (a DELETE, the old versions of an UPDATE), `added` become live (an
// INSERT, the new versions). The cache that was current before this statement took the lock takes the change in, so that it stays current; any other cache is dropped. The rows
// must still hold the values they had (a pointer into the table is fine: the key columns are not what a statement ends a version with).
void Executor::unique_probe_apply(SharedDatabase& s, const std::string& table, const TableSchema& schema, const std::vector<const Row*>& removed,
                                  const std::vector<const Row*>& added) {
    UniqueProbes& all = *s.unique_probes;
    if (!all.enabled) return;
    std::uint64_t locks_id = 0, data_id = 0, sum = 0;
    if (!unique_probe_stamp(s, table, locks_id, data_id, sum)) return;
    std::lock_guard<std::mutex> guard(all.mutex);
    UniqueProbe* p = probe_for(all, table, schema, /*create=*/false);
    if (!p || !p->built) return;
    // (nobody but this statement has taken a lock since the cache was current: once for the statement's own section, or already updated by an earlier change of the section)
    if (p->locks_id == locks_id && p->data_id == data_id && (p->sum + 1 == sum || p->sum == sum)) {
        for (const Row* row : removed) {
            for (std::size_t i = 0; i < p->specs.size(); i++) {
                auto key = key_of(p->specs[i], row_values(*row));
                if (!key) continue;
                auto it = p->counts[i].find(*key);
                if (it == p->counts[i].end()) continue;
                if (--it->second == 0) p->counts[i].erase(it);
            }
        }
        for (const Row* row : added) {
            for (std::size_t i = 0; i < p->specs.size(); i++) {
                if (auto key = key_of(p->specs[i], row_values(*row))) p->counts[i][*key]++;
            }
        }
        p->sum = sum;
    } else {
        p->built = false;
        p->counts.clear();
    }
}

// The INSERT has the table's data lock exclusive and is about to append `rows` (every row it appends, none that it skipped).
void Executor::unique_probe_inserted(SharedDatabase& s, const std::string& table, const TableSchema& schema, const std::vector<Row>& rows) {
    UniqueProbes& all = *s.unique_probes;
    if (!all.enabled) return;
    {
        std::lock_guard<std::mutex> guard(all.mutex);
        if (UniqueProbe* p = probe_for(all, table, schema)) p->insert_tails++;
    }
    std::vector<const Row*> added;
    added.reserve(rows.size());
    for (auto& row : rows) added.push_back(&row);
    unique_probe_apply(s, table, schema, {}, added);
    if (all.check) {
        // (the rows are not in the table yet: count them in a copy of what it holds)
        std::uint64_t locks_id = 0, data_id = 0, sum = 0;
        if (!unique_probe_stamp(s, table, locks_id, data_id, sum)) return;
        std::lock_guard<std::mutex> guard(all.mutex);
        UniqueProbe* p = probe_for(all, table, schema);
        if (!p || !p->built || p->sum != sum) return;
        const auto rows_it = s.tables.find(table);
        std::vector<Row> everything = rows_it == s.tables.end() ? std::vector<Row>{} : rows_it->second;
        everything.insert(everything.end(), rows.begin(), rows.end());
        check_against_rows(*p, &everything, table, &Executor::is_visible);
    }
}

// A COMMIT (which holds the table's lock exclusively) changes no row that is live, so the cache that was current before it stays current.
void Executor::unique_probe_commit(SharedDatabase& s, const std::string& table) {
    UniqueProbes& all = *s.unique_probes;
    if (!all.enabled) return;
    std::uint64_t locks_id = 0, data_id = 0, sum = 0;
    if (!unique_probe_stamp(s, table, locks_id, data_id, sum)) return;
    std::lock_guard<std::mutex> guard(all.mutex);
    auto it = all.by_table.find(table);
    if (it == all.by_table.end() || !it->second.built) return;
    UniqueProbe& p = it->second;
    if (p.locks_id == locks_id && p.data_id == data_id && p.sum + 1 == sum) {
        p.sum = sum;
    } else {
        p.built = false;
        p.counts.clear();
    }
}

} // namespace engine
