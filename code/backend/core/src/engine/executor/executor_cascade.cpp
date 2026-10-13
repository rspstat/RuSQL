// The child rows a foreign-key action changes (ON DELETE / ON UPDATE CASCADE, SET NULL, SET DEFAULT).
//
// They used to be edited in place: a SET NULL / ON UPDATE CASCADE overwrote the field of the row that every other session and every snapshot reads, and a cascaded
// delete in a transaction ended the row without telling the transaction manager. Nothing of it was in the undo log, so a ROLLBACK (or ROLLBACK TO SAVEPOINT) gave the parent
// row back and left the children as the cascade had made them -- deleted, nulled, or pointing at a parent key that is no more -- and a crash could not replay the cascade from the
// redo log either. Here a cascade is written like an UPDATE / DELETE of the child table: the old version is ended with the id of the statement (or of the transaction), a new
// version takes its place, and both go to the transaction manager (log_update / log_delete), so rollback, savepoints, snapshots, redo and the indexes see the same thing.

#include <algorithm>

#include "engine/executor/executor.hpp"

namespace engine {

void Executor::cascade_write(SharedDatabase& s, const std::string& table, const std::vector<std::size_t>& positions, const std::function<void(Row&)>& change,
                             std::uint64_t version_id) {
    if (positions.empty()) return;
    auto tit = s.tables.find(table);
    if (tit == s.tables.end()) return;
    std::vector<Row>& rows = tit->second;
    const TableSchema* schema = s.catalog.get_table(table);

    // the column the primary-key tree, the undo log and the position cache are keyed by (see exec_insert_inner), and whether that is the whole primary key
    std::string key_col = schema && !schema->columns.empty() ? schema->columns.front().name : std::string();
    std::size_t pk_count = 0;
    if (schema) {
        for (auto& c : schema->columns) {
            if (!c.primary_key) continue;
            if (pk_count == 0) key_col = c.name;
            pk_count++;
        }
    }
    const bool single_pk = pk_count == 1;
    auto key_of = [&](const Row& r) {
        auto it = r.find(key_col);
        return it != r.end() ? it->second : std::string();
    };

    const std::string id = std::to_string(version_id);
    const bool deleting = !change;
    // A row that no snapshot can still need goes for good (as in a DELETE outside any transaction when nobody else is running); otherwise it is only ended.
    const bool erase_for_good = deleting && !txn.is_active() && s.active_txn_ids->lock()->empty();

    std::vector<Row> olds, news;
    for (std::size_t pos : positions) {
        Row& row = rows[pos];
        Row old = row; // the image before this statement ended it (_xmax "0")
        if (deleting) {
            txn.log_delete(table, key_of(old), row_to_json(old));
            if (!erase_for_good) row["_xmax"] = id;
            olds.push_back(std::move(old));
        } else {
            Row fresh = old;
            change(fresh);
            fresh["_xmin"] = id;
            fresh["_xmax"] = "0";
            txn.log_update(table, key_of(old), row_to_json(old), row_to_json(fresh));
            row["_xmax"] = id;
            olds.push_back(std::move(old));
            news.push_back(std::move(fresh));
        }
    }

    auto idx_it = s.indexes.find(table);
    std::vector<CompositeIndex*> composite;
    for (auto& [k, ci] : s.composite_indexes) {
        if (ci.table == table) composite.push_back(&ci);
    }

    if (deleting) {
        // an ended row is still in the table: the tree entry is rewritten with its new _xmax, as a DELETE does; a row that goes for good leaves every index
        if (erase_for_good) {
            std::vector<char> doomed(rows.size(), 0);
            for (std::size_t pos : positions) doomed[pos] = 1;
            std::size_t keep = 0;
            for (std::size_t i = 0; i < rows.size(); i++) {
                if (doomed[i]) continue;
                if (keep != i) rows[keep] = std::move(rows[i]);
                keep++;
            }
            rows.resize(keep);
            if (auto pit = s.row_pk_pos.find(table); pit != s.row_pk_pos.end()) pit->second.clear(); // (the positions have moved; the cache fills again as rows are looked up)
            index_replace_rows(s, table, olds, std::vector<Row>{}, key_col);
            if (idx_it != s.indexes.end() && single_pk) {
                for (auto& old : olds) idx_it->second.remove(key_of(old));
            }
            for (auto* ci : composite) {
                for (auto& old : olds) ci->remove_row(old);
            }
        } else {
            std::vector<Row> ended;
            for (std::size_t pos : positions) ended.push_back(rows[pos]);
            if (idx_it != s.indexes.end() && single_pk) {
                for (auto& row : ended) idx_it->second.insert(key_of(row), row_to_json(row));
            }
            for (std::size_t i = 0; i < ended.size(); i++) {
                for (auto* ci : composite) {
                    ci->remove_row(olds[i]);
                    ci->insert_row(ended[i]);
                }
            }
            index_replace_rows(s, table, olds, ended, key_col);
            if (auto pit = s.row_pk_pos.find(table); pit != s.row_pk_pos.end() && single_pk) {
                for (auto& old : olds) pit->second.erase(key_of(old));
            }
        }
    } else {
        // the new versions go to the end of the table; the old ones stay, ended
        const std::size_t first_new = rows.size();
        for (auto& fresh : news) rows.push_back(fresh);
        if (idx_it != s.indexes.end() && single_pk) {
            for (auto& fresh : news) idx_it->second.insert(key_of(fresh), row_to_json(fresh));
        }
        for (std::size_t i = 0; i < news.size(); i++) {
            for (auto* ci : composite) {
                if (ci->key_from_row(olds[i]) != ci->key_from_row(news[i])) ci->remove_row(olds[i]);
                ci->insert_row(news[i]);
            }
        }
        index_replace_rows(s, table, olds, news, key_col);
        if (auto pit = s.row_pk_pos.find(table); pit != s.row_pk_pos.end() && single_pk) {
            for (std::size_t i = 0; i < news.size(); i++) pit->second[key_of(news[i])] = first_new + i;
        }
    }
}

} // namespace engine
