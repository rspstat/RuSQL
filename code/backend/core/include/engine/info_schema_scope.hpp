#pragma once

// R95 -- how a SELECT of information_schema tables is run.
//
// The rows of an information_schema table are made when the SELECT runs (processlist, tables, columns ...), and the SELECT then reads them like the rows of any
// table: they are put in a temporary table. Until R94 that temporary table lived in the SHARED database (SharedDatabase::tables / ::catalog) -- under a structural lock
// that is only SHARED, so while other sessions read and write the same maps (an UPDATE's cascade walks the catalog, ...). That is a data race: "Internal error:
// invalid unordered_map<K, T> key" now and then, a crash under a hard enough load (test_info_schema_concurrency.cpp, code/test/crash/fuzz_kill.py).
//
// Sending such a SELECT to the exclusive path is not the answer (R94, measured): every poll of information_schema.processlist -- the Activity panel, the MCP
// list_locks / kill_session, the tests of KILL -- would then wait for any long statement to end, which is exactly when the list is needed.
//
// So: a SELECT whose FROM list is information_schema tables only, with no subquery anywhere in it, runs on a PRIVATE database (InfoSchemaScratch): the temporary
// tables are made there, and nothing of the shared maps is changed. Any other SELECT that names an information_schema table (one in a list with a real table, one
// with a subquery) still makes its temporary tables in the shared maps, which is only safe under the exclusive lock -- Executor::table_lock_set_for sends it there.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/parser/ast.hpp"
#include "engine/row.hpp"

namespace engine {

struct SharedDatabase;

enum class InfoSchemaUse {
    None,    // no information_schema table in the FROM list
    Private, // information_schema tables only, nothing in it reads another table: the private database
    Mixed,   // an information_schema table next to a real table, a subquery or a lock clause: the shared maps (the exclusive path)
};

// `tables`: the tables of the FROM list and of its joins, qualified (database.table). `derived`: the FROM list holds a derived table, a LATERAL join or a view alias.
InfoSchemaUse info_schema_use(const std::vector<std::string>& tables, bool derived, const std::vector<SelectColumn>& columns, const std::optional<CondExpr>& condition,
                              const std::optional<CondExpr>& having, const std::vector<Join>& joins, bool for_update, bool for_share);

// Does the information_schema table (qualified name) make its rows from the ROWS of the other tables -- TABLE_ROWS and the sizes of `tables`, the cardinality of `statistics`, the versions of
// `row_versions`? Such a SELECT reads every table's rows like a SELECT of all of them: the lock set of the statement holds the data lock of each (Executor::table_lock_set_for), or a
// session that appends a row version at the same moment moves the vector under the reader.
bool info_schema_reads_rows(const std::string& table);

// A private database for one SELECT: the temporary tables of the statement go in it, and it hands them back when the statement is over (also when it stops: KILL, an
// error). It borrows the transaction bookkeeping of `real` (ids, the open transactions, the snapshots, the accounts) so that the SELECT sees the same world; the
// tables themselves are never those of `real`. A few of these are kept for the next SELECT, so there is no lock between sessions that run one at the same time.
class InfoSchemaScratch {
public:
    // `txn_id`: the transaction the SELECT runs in (0: none); what it locked in the private database goes with it.
    InfoSchemaScratch(const SharedDatabase& real, std::uint64_t txn_id);
    ~InfoSchemaScratch();
    InfoSchemaScratch(const InfoSchemaScratch&) = delete;
    InfoSchemaScratch& operator=(const InfoSchemaScratch&) = delete;

    SharedDatabase& db() { return *db_; }
    void add_table(const std::string& name, const std::vector<ColumnDef>& columns, std::vector<Row> rows);

private:
    std::unique_ptr<SharedDatabase> db_;
    std::vector<std::string> names_;
    std::uint64_t txn_id_;
};

} // namespace engine
