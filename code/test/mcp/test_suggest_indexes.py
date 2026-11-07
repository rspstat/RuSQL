"""Unit tests of the MCP tool suggest_indexes against a fake engine (no server needed): py -3 -m unittest code/test/mcp/test_suggest_indexes.py

The reading of a statement is the engine's (SUGGEST INDEX, tested in C++ by test_index_advice.cpp): the fake answers it in the engine's own output format, together with the four other
things the tool asks the engine for -- information_schema.columns, information_schema.statistics, ANALYZE TABLE and EXPLAIN [ANALYZE] -- so what is tested here is what the tool makes of
those answers: the JSON it returns, the plan, the trial of the suggestions (verify), the OR rewrite, and that an UPDATE or DELETE is never run.
"""
import json
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "mcp"))
import mcp_server  # noqa: E402

TABLES = {
    "orders": {"columns": ["id", "customer_id", "status", "total", "created_at"],
               "indexes": {"PRIMARY": ["id"]},
               "rows": 10000,
               "distinct": {"id": 10000, "customer_id": 1000, "status": 2, "total": 5000, "created_at": 9000}},
    "customers": {"columns": ["id", "name", "email"], "indexes": {"PRIMARY": ["id"], "idx_name": ["name"]}, "rows": 1000,
                  "distinct": {"id": 1000, "name": 1000, "email": 1000}},
    "big": {"columns": ["id", "k"], "indexes": {"PRIMARY": ["id"]}, "rows": 500000, "distinct": {"id": 500000, "k": 5000}},
}

SUGGEST_HEADERS = ["kind", "table_name", "columns", "detail", "create_index", "rows_in_table", "rows_read_with_index", "block", "needs_rewrite"]


def box(headers, rows):
    widths = [max(len(str(x)) for x in [h] + [r[i] for r in rows]) for i, h in enumerate(headers)]
    bar = "+" + "+".join("-" * (w + 2) for w in widths) + "+"

    def line(cells):
        return "| " + " | ".join(str(c).ljust(w) for c, w in zip(cells, widths)) + " |"
    return "\n".join(["OK", bar, line(headers), bar] + [line(r) for r in rows] + [bar, f"{len(rows)} row(s) returned.", "(0.001 sec)"])


def suggest(table, columns, why, create, rows, read="", block="", rewrite=""):
    return ["suggest", table, columns, why, create, str(rows), str(read), block, rewrite]


class FakeEngine:
    def __init__(self):
        self.statements = []
        self.advice_rows = []   # what SUGGEST INDEX answers
        self.advice_error = ""  # ... or this error
        self.created = []       # the indexes CREATE INDEX made and DROP INDEX did not take away yet
        self.fail_drop = False  # DROP INDEX answers an error

    def __call__(self, sql, db=""):
        self.statements.append(sql)
        m = re.search(r"TABLE_NAME = '(\w+)'", sql)
        if sql.startswith("SUGGEST INDEX"):
            if self.advice_error:
                return "ERR\n" + self.advice_error + "\n---END---"
            return box(SUGGEST_HEADERS, self.advice_rows or [["note", "", "", "Nothing to suggest", "", "", "", "", ""]])
        if "information_schema.columns" in sql:
            return box(["COLUMN_NAME"], [[c] for c in TABLES[m.group(1)]["columns"]])
        if "information_schema.statistics" in sql:
            rows = [[name, k + 1, col] for name, cols in TABLES[m.group(1)]["indexes"].items() for k, col in enumerate(cols)]
            return box(["INDEX_NAME", "SEQ_IN_INDEX", "COLUMN_NAME"], rows)
        m = re.match(r"ANALYZE TABLE (\w+)", sql)
        if m:
            t = TABLES[m.group(1)]
            lines = ["OK", f"| ANALYZE: {m.group(1)} ({t['rows']} rows) |", "| column | distinct | nulls | min | max | p25 | p50 | p75 |"]
            for c in t["columns"]:
                lines.append(f"| {c[:12].ljust(12)} | {str(t['distinct'][c]).rjust(8)} |        0 | 1          | 9          | 2          | 5          | 7          |")
            return "\n".join(lines + ["(0.001 sec)"])
        m = re.match(r"CREATE INDEX (\S+) ON (\w+) \((.+?)\);", sql)
        if m:
            self.created.append((m.group(1), m.group(2)))
            return "OK\nIndex created."
        m = re.match(r"DROP INDEX (\S+) ON (\w+);", sql)
        if m:
            if self.fail_drop:
                return "ERR\nthe drop failed\n---END---"
            self.created = [c for c in self.created if c[0] != m.group(1)]
            return "OK\nIndex dropped."
        if sql.startswith("EXPLAIN ANALYZE"):
            if self.created:  # (an index on the table: the scan is gone)
                return "\n".join(["OK", "QUERY PLAN", "  Access: Index lookup on orders", "  Estimated rows : 10  Actual rows : 10", "  Execution time : 0.500 ms",
                                  "  Step 1 Index lookup orders : 10 rows, 0.400 ms, 80%", "  Step 2 Sort : 10 rows, 0.100 ms, 20%", "(0.001 sec)"])
            return "\n".join(["OK", "QUERY PLAN", "  Access: Table scan on orders", "  Estimated rows : 100  Actual rows : 100", "  Execution time : 4.000 ms",
                              "  Step 1 Scan orders : 10000 rows, 3.600 ms, 90%", "  Step 2 Sort : 100 rows, 0.400 ms, 10%", "(0.004 sec)"])
        if sql.startswith("EXPLAIN "):  # EXPLAIN UPDATE / DELETE: the estimate, nothing is run
            if self.created:
                return "\n".join(["OK", "| Statement: UPDATE |", "| Table: orders |", "| Rows (total): 10000 |", "| Est. cost: 24.6 |", "| Access: Index Scan idx (status = 1) |"])
            return "\n".join(["OK", "| Statement: UPDATE |", "| Table: orders |", "| Rows (total): 10000 |", "| Est. cost: 5000.0 |", "| Access: Seq Scan |"])
        raise AssertionError("unexpected statement: " + sql)


class SuggestIndexes(unittest.TestCase):
    def setUp(self):
        self.engine = FakeEngine()
        self.saved = mcp_server._run
        mcp_server._run = self.engine

    def tearDown(self):
        mcp_server._run = self.saved

    def suggest(self, sql, verify=False):
        return mcp_server.suggest_indexes_for(sql, "shop", verify)

    # ── the engine's reading becomes the answer ──────────────────────────────────────────────────────────────────────────────────────────────────────────
    def test_the_suggestions_of_the_engine_are_the_answer(self):
        self.engine.advice_rows = [
            suggest("orders", "customer_id, status", "searched by customer_id (=), status (=)", "CREATE INDEX idx_orders_customer_id_status ON orders (customer_id, status);", 10000, 5),
            ["already", "customers", "name", "already served by the index idx_name", "", "1000", "", "", ""],
            ["note", "", "", "A LIKE on name starts with a wildcard.", "", "", "", "", ""],
        ]
        r = self.suggest("SELECT * FROM orders WHERE customer_id = 7 AND status = 1")
        self.assertEqual(r["statement"], "SELECT")
        s = r["suggestions"][0]
        self.assertEqual(s["table"], "orders")
        self.assertEqual(s["columns"], ["customer_id", "status"])
        self.assertEqual(s["create_index"], "CREATE INDEX idx_orders_customer_id_status ON orders (customer_id, status);")
        self.assertEqual(s["rows_in_table"], 10000)
        self.assertEqual(s["rows_read_with_index_estimate"], 5)
        self.assertNotIn("in", s)
        self.assertEqual(r["already_indexed"], [{"table": "customers", "columns": ["name"], "index": "idx_name"}])
        self.assertEqual(r["notes"], ["A LIKE on name starts with a wildcard."])
        self.assertIn("orders", r["tables"])
        self.assertEqual(r["tables"]["orders"]["rows"], 10000)
        self.assertEqual(self.engine.statements[0], "SUGGEST INDEX SELECT * FROM orders WHERE customer_id = 7 AND status = 1")  # (the statement as it was written)
        self.assertEqual(r["plan"]["access"], ["Access: Table scan on orders"])

    def test_the_block_a_suggestion_is_for_is_kept(self):
        self.engine.advice_rows = [suggest("orders", "customer_id", "searched by customer_id (=) (in an IN subquery)", "CREATE INDEX idx_orders_customer_id ON orders (customer_id);", 10000, 10, block="an IN subquery")]
        r = self.suggest("SELECT * FROM customers WHERE id IN (SELECT customer_id FROM orders WHERE customer_id = 3)")
        self.assertEqual(r["suggestions"][0]["in"], "an IN subquery")
        self.assertEqual(r["blocks"], ["an IN subquery"])

    def test_a_suggestion_without_an_estimate_has_none(self):
        self.engine.advice_rows = [suggest("orders", "created_at", "searched by created_at (range)", "CREATE INDEX idx_orders_created_at ON orders (created_at);", 10000)]
        s = self.suggest("SELECT * FROM orders WHERE created_at > 5")["suggestions"][0]
        self.assertNotIn("rows_read_with_index_estimate", s)
        self.assertNotIn("needs_rewrite", s)

    def test_an_error_of_the_engine_is_returned_as_it_is(self):
        self.engine.advice_error = "Unexpected token near FROM"
        r = self.suggest("SELECT FROM WHERE")
        self.assertIn("Unexpected token", r["error"])

    def test_other_statements_are_refused_without_asking_the_engine(self):
        for sql in ("INSERT INTO orders VALUES (1)", "DROP TABLE orders", ""):
            self.assertIn("error", self.suggest(sql), sql)
        self.assertEqual(self.engine.statements, [])

    def test_a_with_and_a_union_are_asked(self):
        self.engine.advice_rows = [["note", "", "", "Nothing to suggest", "", "", "", "", ""]]
        self.assertEqual(self.suggest("WITH x AS (SELECT 1) SELECT * FROM x")["statement"], "SELECT")
        self.assertEqual(self.suggest("(SELECT id FROM orders) UNION (SELECT id FROM customers)")["statement"], "SELECT")

    def test_the_tool_is_registered_and_returns_json(self):
        self.engine.advice_rows = [suggest("orders", "status", "searched by status (=)", "CREATE INDEX idx_orders_status ON orders (status);", 10000, 5000)]
        out = json.loads(mcp_server.suggest_indexes("SELECT * FROM orders WHERE status = 1", "shop"))
        self.assertEqual(out["suggestions"][0]["table"], "orders")

    # ── an UPDATE or DELETE: planned, never run ──────────────────────────────────────────────────────────────────────────────────────────────────────────
    def test_an_update_gets_the_estimated_plan_and_is_not_run(self):
        self.engine.advice_rows = [suggest("orders", "status", "searched by status (=)", "CREATE INDEX idx_orders_status ON orders (status);", 10000, 5000)]
        r = self.suggest("UPDATE orders SET total = 0 WHERE status = 1")
        self.assertEqual(r["statement"], "UPDATE")
        self.assertEqual(r["plan"]["estimated_cost"], 5000.0)
        self.assertEqual(r["plan"]["access"], ["Access: Seq Scan"])
        self.assertTrue(any("not run" in n for n in r["notes"]))
        self.assertIn("EXPLAIN UPDATE orders SET total = 0 WHERE status = 1", self.engine.statements)
        self.assertFalse([s for s in self.engine.statements if s.startswith(("UPDATE", "EXPLAIN ANALYZE UPDATE"))])

    def test_verify_of_an_update_compares_the_estimates_and_runs_nothing(self):
        self.engine.advice_rows = [suggest("orders", "status", "searched by status (=)", "CREATE INDEX idx_orders_status ON orders (status);", 10000, 5000)]
        r = self.suggest("UPDATE orders SET total = 0 WHERE status = 1", verify=True)
        trial = r["verification"]["each_index_alone"][0]
        self.assertEqual(trial["estimated_cost"], {"before": 5000.0, "after": 24.6})
        self.assertEqual(trial["verdict"], "cheaper (estimated)")
        self.assertIn("NOT run", r["verification"]["note"])
        self.assertEqual(self.engine.created, [])  # (the trial index was dropped)
        self.assertFalse([s for s in self.engine.statements if re.match(r"(UPDATE|DELETE|EXPLAIN ANALYZE (UPDATE|DELETE))", s)])

    def test_a_delete_is_planned_too(self):
        self.engine.advice_rows = []
        r = self.suggest("DELETE FROM orders WHERE status = 1")
        self.assertEqual(r["statement"], "DELETE")
        self.assertIn("EXPLAIN DELETE FROM orders WHERE status = 1", self.engine.statements)

    def test_explain_query_runs_a_select_but_only_plans_an_update_or_delete(self):
        mcp_server.explain_query("SELECT * FROM orders", "shop")
        mcp_server.explain_query("update orders set total = 1 where id = 2;", "shop")
        mcp_server.explain_query("EXPLAIN ANALYZE DELETE FROM orders WHERE id = 2", "shop")
        self.assertEqual(self.engine.statements, ["EXPLAIN ANALYZE SELECT * FROM orders", "EXPLAIN update orders set total = 1 where id = 2", "EXPLAIN DELETE FROM orders WHERE id = 2"])
        self.engine.statements.clear()
        self.assertIn("Refused", mcp_server.explain_query("INSERT INTO orders VALUES (1)", "shop"))
        self.assertEqual(self.engine.statements, [])

    # ── verify: a trial of the suggestions ───────────────────────────────────────────────────────────────────────────────────────────────────────────────
    def test_verify_tries_each_index_and_drops_it(self):
        self.engine.advice_rows = [suggest("orders", "status", "searched by status (=)", "CREATE INDEX idx_orders_status ON orders (status);", 10000, 5000)]
        r = self.suggest("SELECT * FROM orders WHERE status = 1", verify=True)
        v = r["verification"]
        self.assertEqual(v["before"]["access"], ["Access: Table scan on orders"])
        trial = v["each_index_alone"][0]
        self.assertEqual(trial["create_index"], "CREATE INDEX idx_orders_status ON orders (status);")
        self.assertEqual(trial["execution_ms"], {"before": 4.0, "after": 0.5})
        self.assertEqual(trial["speedup"], 8.0)
        self.assertEqual(trial["verdict"], "faster")
        self.assertEqual(self.engine.created, [])  # (every index that was made is gone)
        self.assertTrue(any(s.startswith("CREATE INDEX __try_") for s in self.engine.statements))
        self.assertTrue(any(s.startswith("DROP INDEX __try_") for s in self.engine.statements))
        self.assertIn("dropped again", v["note"])

    def test_verify_off_creates_nothing(self):
        self.engine.advice_rows = [suggest("orders", "status", "searched by status (=)", "CREATE INDEX idx_orders_status ON orders (status);", 10000, 5000)]
        r = self.suggest("SELECT * FROM orders WHERE status = 1")
        self.assertNotIn("verification", r)
        self.assertFalse([s for s in self.engine.statements if s.startswith(("CREATE INDEX", "DROP INDEX"))])
        self.assertIn("Nothing was created", r["caveat"])

    def test_verify_tries_two_indexes_together_too(self):
        self.engine.advice_rows = [
            suggest("orders", "customer_id", "joined on customer_id", "CREATE INDEX idx_orders_customer_id ON orders (customer_id);", 10000, 10),
            suggest("customers", "email", "searched by email (=)", "CREATE INDEX idx_customers_email ON customers (email);", 1000, 1),
        ]
        v = self.suggest("SELECT * FROM orders JOIN customers ON customers.id = orders.customer_id WHERE customers.email = 'a'", verify=True)["verification"]
        self.assertEqual(len(v["each_index_alone"]), 2)
        self.assertEqual(len(v["all_together"]["create_index"]), 2)
        self.assertEqual(self.engine.created, [])

    def test_verify_measures_the_rewrite_of_an_or_with_both_branch_indexes(self):
        self.engine.advice_rows = [
            suggest("orders", "customer_id", "branch 1 of an OR (searched by customer_id)", "CREATE INDEX idx_orders_customer_id ON orders (customer_id);", 10000, 10, rewrite="yes"),
            suggest("orders", "status", "branch 2 of an OR (searched by status)", "CREATE INDEX idx_orders_status ON orders (status);", 10000, 5000, rewrite="yes"),
        ]
        r = self.suggest("SELECT * FROM orders WHERE customer_id = 7 OR status = 1", verify=True)
        self.assertEqual(r["rewrites"][0]["sql"], "SELECT * FROM orders WHERE customer_id = 7 UNION SELECT * FROM orders WHERE status = 1;")
        rewrite = r["verification"]["or_rewrite"]
        self.assertEqual(rewrite["rewrite"], r["rewrites"][0]["sql"].rstrip(";"))
        self.assertEqual(len(rewrite["create_index"]), 2)
        self.assertNotIn("each_index_alone", r["verification"])  # (an index that serves a branch only says nothing about the statement as it is)
        self.assertEqual(self.engine.created, [])

    def test_an_or_that_cannot_be_rewritten_says_so(self):
        self.engine.advice_rows = [
            suggest("orders", "customer_id", "branch 1 of an OR", "CREATE INDEX idx_orders_customer_id ON orders (customer_id);", 10000, 10, rewrite="yes"),
            suggest("orders", "status", "branch 2 of an OR", "CREATE INDEX idx_orders_status ON orders (status);", 10000, 5000, rewrite="yes"),
        ]
        r = self.suggest("SELECT * FROM orders WHERE (customer_id = 7 OR status = 1) ORDER BY id", verify=True)  # (an ORDER BY: the UNION would change it)
        self.assertNotIn("rewrites", r)
        self.assertIn("rewrite", r["verification"]["or_rewrite"]["note"])

    def test_the_text_of_an_or_with_an_or_in_a_string_is_not_cut(self):
        self.assertIsNone(mcp_server._or_rewrite("SELECT * FROM orders WHERE status = 'a or b' OR customer_id = 3"))

    def test_verify_names_an_index_it_could_not_drop(self):
        self.engine.advice_rows = [suggest("orders", "status", "searched by status (=)", "CREATE INDEX idx_orders_status ON orders (status);", 10000, 5000)]
        self.engine.fail_drop = True
        v = self.suggest("SELECT * FROM orders WHERE status = 1", verify=True)["verification"]
        self.assertEqual(len(v["left_behind"]), 1)
        self.assertIn("__try_", v["left_behind"][0])
        self.assertIn("drop them yourself", v["note"])

    def test_verify_leaves_a_big_table_alone(self):
        self.engine.advice_rows = [suggest("big", "k", "searched by k (=)", "CREATE INDEX idx_big_k ON big (k);", 500000, 100)]
        v = self.suggest("SELECT * FROM big WHERE k = 7", verify=True)["verification"]
        self.assertIn("skipped", v["each_index_alone"][0])
        self.assertFalse([s for s in self.engine.statements if s.startswith("CREATE INDEX")])

    def test_verify_says_so_when_there_is_nothing_to_try(self):
        self.engine.advice_rows = [["note", "", "", "Nothing to suggest", "", "", "", "", ""]]
        r = self.suggest("SELECT * FROM orders", verify=True)
        self.assertEqual(r["verification"], {"note": "There is no index to try."})


if __name__ == "__main__":
    unittest.main()
