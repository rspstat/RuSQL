"""Unit tests of the MCP tool suggest_indexes against a fake engine (no server needed): py -3 -m unittest code/test/mcp/test_suggest_indexes.py

The fake answers the four things the tool asks the engine for -- information_schema.columns, information_schema.statistics, ANALYZE TABLE and EXPLAIN ANALYZE --
in the engine's own output format, so the reading of the statement and the decisions are tested, not the engine.
"""
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "mcp"))
import mcp_server  # noqa: E402

TABLES = {
    "orders": {"columns": ["id", "customer_id", "status", "total", "created_at", "region_manager_id"],
               "indexes": {"PRIMARY": ["id"]},
               "rows": 10000,
               "distinct": {"id": 10000, "customer_id": 1000, "status": 2, "total": 5000, "created_at": 9000, "region_manager_id": 50}},
    "customers": {"columns": ["id", "name", "email"], "indexes": {"PRIMARY": ["id"], "idx_name": ["name"]}, "rows": 1000,
                  "distinct": {"id": 1000, "name": 1000, "email": 1000}},
    "tiny": {"columns": ["id", "code"], "indexes": {"PRIMARY": ["id"]}, "rows": 20, "distinct": {"id": 20, "code": 20}},
}


def box(headers, rows):
    widths = [max(len(str(x)) for x in [h] + [r[i] for r in rows]) for i, h in enumerate(headers)]
    bar = "+" + "+".join("-" * (w + 2) for w in widths) + "+"
    def line(cells):
        return "| " + " | ".join(str(c).ljust(w) for c, w in zip(cells, widths)) + " |"
    return "\n".join(["OK", bar, line(headers), bar] + [line(r) for r in rows] + [bar, f"{len(rows)} row(s) returned.", "(0.001 sec)"])


class FakeEngine:
    def __init__(self):
        self.statements = []

    def __call__(self, sql, db=""):
        self.statements.append(sql)
        m = re.search(r"TABLE_NAME = '(\w+)'", sql)
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
        if sql.startswith("EXPLAIN ANALYZE"):
            return "\n".join(["OK", "QUERY PLAN", "  Access: Table scan on orders", "  Estimated rows : 100  Actual rows : 100", "  Execution time : 4.000 ms",
                              "  Step 1 Scan orders : 10000 rows, 3.600 ms, 90%", "  Step 2 Sort : 100 rows, 0.400 ms, 10%", "(0.004 sec)"])
        raise AssertionError("unexpected statement: " + sql)


class SuggestIndexes(unittest.TestCase):
    def setUp(self):
        self.engine = FakeEngine()
        self.saved = mcp_server._run
        mcp_server._run = self.engine

    def tearDown(self):
        mcp_server._run = self.saved

    def suggest(self, sql):
        return mcp_server.suggest_indexes_for(sql)

    def creates(self, result):
        return [s["create_index"] for s in result["suggestions"]]

    def test_equality_on_an_unindexed_column(self):
        r = self.suggest("SELECT * FROM orders WHERE customer_id = 42")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);"])
        self.assertEqual(r["suggestions"][0]["rows_read_with_index_estimate"], 10)
        self.assertEqual(r["suggestions"][0]["rows_in_table"], 10000)
        self.assertEqual(r["plan"]["steps"][0]["what"], "Scan orders")
        self.assertIn("90%", r["slowest_step"])

    def test_equality_columns_first_then_one_range(self):
        r = self.suggest("SELECT * FROM orders WHERE created_at > '2026-01-01' AND customer_id = 7 AND region_manager_id = 3")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id_region_manager_id_created_at ON orders (customer_id, region_manager_id, created_at);"])

    def test_a_range_adds_no_row_estimate(self):
        # how much a range keeps is not known: a range alone has no estimate, beside an equality the estimate is the equality's
        r = self.suggest("SELECT * FROM orders WHERE total BETWEEN 10 AND 20")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_total ON orders (total);"])
        self.assertNotIn("rows_read_with_index_estimate", r["suggestions"][0])
        r = self.suggest("SELECT * FROM orders WHERE customer_id = 7 AND total > 5")
        self.assertEqual(r["suggestions"][0]["rows_read_with_index_estimate"], 10)
        # a range on a column of few values is still suggested: the range may keep little of the table
        r = self.suggest("SELECT * FROM orders WHERE status = 'open' AND created_at > '2026-01-01'")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_status_created_at ON orders (status, created_at);"])

    def test_already_indexed_is_not_suggested_again(self):
        r = self.suggest("SELECT * FROM orders WHERE id = 5")
        self.assertEqual(r["suggestions"], [])
        self.assertEqual(r["already_indexed"][0]["index"], "PRIMARY")
        r = self.suggest("SELECT * FROM customers WHERE name = 'a'")
        self.assertEqual(r["already_indexed"][0]["index"], "idx_name")

    def test_a_small_table_is_left_alone(self):
        r = self.suggest("SELECT * FROM tiny WHERE code = 3")
        self.assertEqual(r["suggestions"], [])
        self.assertTrue(any("only 20 rows" in n for n in r["notes"]))

    def test_a_column_that_does_not_narrow_is_not_suggested(self):
        r = self.suggest("SELECT * FROM orders WHERE status = 'open'")
        self.assertEqual(r["suggestions"], [])
        self.assertTrue(any("not suggested" in n for n in r["notes"]))

    def test_function_around_a_column(self):
        r = self.suggest("SELECT * FROM orders WHERE YEAR(created_at) = 2026")
        self.assertEqual(r["suggestions"], [])
        self.assertTrue(any("wraps a column" in n for n in r["notes"]))

    def test_like_with_and_without_a_leading_wildcard(self):
        r = self.suggest("SELECT * FROM customers WHERE email LIKE '%@x.com'")
        self.assertEqual(r["suggestions"], [])
        self.assertTrue(any("wildcard" in n for n in r["notes"]))
        r = self.suggest("SELECT * FROM customers WHERE email LIKE 'ab%'")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_customers_email ON customers (email);"])

    def test_or_is_explained(self):
        r = self.suggest("SELECT * FROM orders WHERE customer_id = 1 OR region_manager_id = 2")
        self.assertEqual(r["suggestions"], [])
        self.assertTrue(any("OR" in n for n in r["notes"]))

    def test_order_by_follows_the_equality_column(self):
        r = self.suggest("SELECT * FROM orders WHERE customer_id = 7 ORDER BY created_at DESC")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id_created_at ON orders (customer_id, created_at);"])
        self.assertIn("sorted by created_at", r["suggestions"][0]["why"])

    def test_order_by_alone_needs_a_limit(self):
        r = self.suggest("SELECT * FROM orders ORDER BY created_at")
        self.assertEqual(r["suggestions"], [])
        self.assertTrue(any("without a LIMIT" in n for n in r["notes"]))
        r = self.suggest("SELECT * FROM orders ORDER BY created_at LIMIT 10")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_created_at ON orders (created_at);"])

    def test_join_column_of_the_other_table(self):
        r = self.suggest("SELECT c.name FROM customers c JOIN orders o ON o.customer_id = c.id WHERE c.name = 'a'")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);"])
        self.assertTrue(any(a["table"] == "customers" for a in r["already_indexed"]))

    def test_update_and_delete_are_read_but_not_explained(self):
        r = self.suggest("UPDATE orders SET status = 'x' WHERE customer_id = 9")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);"])
        self.assertNotIn("plan", r)
        self.assertFalse(any(s.startswith("EXPLAIN") for s in self.engine.statements))
        r = self.suggest("DELETE FROM orders WHERE customer_id = 9")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);"])

    def test_long_column_names_are_found_in_analyze_output(self):
        # ANALYZE TABLE cuts column names to 12 characters: region_manager_id is shown as region_manag
        r = self.suggest("SELECT * FROM orders WHERE region_manager_id = 3")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_region_manager_id ON orders (region_manager_id);"])
        self.assertEqual(r["suggestions"][0]["rows_read_with_index_estimate"], 200)

    def test_group_by_alone(self):
        r = self.suggest("SELECT customer_id, COUNT(*) FROM orders GROUP BY customer_id")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);"])

    def test_strings_and_comments_do_not_confuse_the_reading(self):
        r = self.suggest("SELECT * FROM orders /* WHERE id = 1 */ WHERE customer_id = 3 -- AND status = 'x'\n AND status <> 'where id = 2'")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);"])

    def test_other_statements_are_refused(self):
        self.assertIn("error", self.suggest("INSERT INTO orders (id) VALUES (1)"))
        self.assertIn("error", self.suggest(""))

    def test_the_tool_is_registered_and_returns_json(self):
        import json
        names = [t.name for t in mcp_server.mcp._tool_manager.list_tools()]
        self.assertIn("suggest_indexes", names)
        out = json.loads(mcp_server.suggest_indexes("SELECT * FROM orders WHERE customer_id = 1"))
        self.assertEqual(out["statement"], "SELECT")


if __name__ == "__main__":
    unittest.main()
