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
    "big": {"columns": ["id", "k"], "indexes": {"PRIMARY": ["id"]}, "rows": 500000, "distinct": {"id": 500000, "k": 5000}},
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
        self.created = []       # the indexes CREATE INDEX made and DROP INDEX did not take away yet
        self.fail_drop = False  # DROP INDEX answers an error

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

    def suggest_verified(self, sql):
        return mcp_server.suggest_indexes_for(sql, "", True)

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

    def test_or_over_two_columns_is_read_branch_by_branch(self):
        # the engine reads one index per statement: an index for each branch helps only when each branch is a statement of its own
        r = self.suggest("SELECT * FROM orders WHERE customer_id = 1 OR region_manager_id = 2")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);", "CREATE INDEX idx_orders_region_manager_id ON orders (region_manager_id);"])
        self.assertTrue(all(s["needs_rewrite"] for s in r["suggestions"]))
        self.assertEqual(r["rewrites"][0]["sql"], "SELECT * FROM orders WHERE customer_id = 1 UNION SELECT * FROM orders WHERE region_manager_id = 2;")

    def test_or_on_one_column_is_an_in(self):
        r = self.suggest("SELECT * FROM orders WHERE customer_id = 1 OR customer_id = 2")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);"])
        self.assertNotIn("needs_rewrite", r["suggestions"][0])
        self.assertNotIn("rewrites", r)

    def test_or_branches_with_ands_and_one_that_cannot_be_served(self):
        r = self.suggest("SELECT * FROM orders WHERE (customer_id = 1 AND status = 'open') OR region_manager_id = 2")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id_status ON orders (customer_id, status);", "CREATE INDEX idx_orders_region_manager_id ON orders (region_manager_id);"])
        r = self.suggest("SELECT * FROM orders WHERE customer_id = 1 OR YEAR(created_at) = 2026")
        self.assertEqual(r["suggestions"], [])
        self.assertTrue(any("OR" in n for n in r["notes"]))
        # a rewrite is only written for a plain statement
        r = self.suggest("SELECT * FROM orders WHERE customer_id = 1 OR region_manager_id = 2 ORDER BY id")
        self.assertNotIn("rewrites", r)

    def test_in_subquery_is_read_as_a_block_of_its_own(self):
        r = self.suggest("SELECT * FROM customers WHERE id IN (SELECT customer_id FROM orders WHERE status = 'open' AND total > 100)")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_status_total ON orders (status, total);"])
        self.assertEqual(r["suggestions"][0]["in"], "an IN subquery")
        self.assertEqual([b["in"] for b in r["blocks"]], ["the statement", "an IN subquery"])
        self.assertTrue(any(a["table"] == "customers" and a["index"] == "PRIMARY" for a in r["already_indexed"]))

    def test_correlated_exists_searches_the_inner_table_by_the_outer_column(self):
        r = self.suggest("SELECT * FROM customers c WHERE EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.id AND o.status = 'open')")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id_status ON orders (customer_id, status);"])
        self.assertIn("an EXISTS subquery", r["suggestions"][0]["why"])
        # the outer table is not asked for an index on the column it is joined by: its own id is the primary key
        self.assertFalse(any(s["table"] == "customers" for s in r["suggestions"]))
        r = self.suggest("SELECT * FROM customers c WHERE c.id IN (SELECT o.customer_id FROM orders o WHERE o.customer_id = c.id AND o.total > 5)")
        self.assertTrue(r["suggestions"])

    def test_derived_table_and_scalar_subquery(self):
        r = self.suggest("SELECT d.c FROM (SELECT customer_id AS c FROM orders WHERE customer_id = 5) d")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);"])
        self.assertEqual(r["suggestions"][0]["in"], "the derived table d")
        r = self.suggest("SELECT * FROM orders WHERE total > (SELECT AVG(total) FROM orders WHERE customer_id = 3)")
        self.assertEqual(sorted(self.creates(r)), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);", "CREATE INDEX idx_orders_total ON orders (total);"])
        self.assertEqual([s["in"] for s in r["suggestions"] if "in" in s], ["a scalar subquery"])

    def test_cte_bodies_are_read_and_the_cte_is_no_table(self):
        r = self.suggest("WITH recent AS (SELECT * FROM orders WHERE customer_id = 5) SELECT * FROM recent r JOIN customers c ON c.id = r.customer_id")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);"])
        self.assertEqual(r["suggestions"][0]["in"], "the CTE recent")
        self.assertNotIn("recent", r["tables"])
        self.assertEqual(r["statement"], "SELECT")
        self.assertIn("plan", r)
        r = self.suggest("WITH a AS (SELECT customer_id FROM orders WHERE customer_id = 5), b AS (SELECT id FROM customers WHERE name = 'x') SELECT * FROM a, b")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);"])

    def test_subqueries_inside_update_and_delete(self):
        r = self.suggest("UPDATE orders SET status = 'x' WHERE customer_id IN (SELECT id FROM customers WHERE email = 'a@b')")
        self.assertEqual(self.creates(r), ["CREATE INDEX idx_orders_customer_id ON orders (customer_id);", "CREATE INDEX idx_customers_email ON customers (email);"])
        self.assertNotIn("plan", r)

    def test_a_like_pattern_belongs_to_its_own_block(self):
        r = self.suggest("SELECT * FROM orders WHERE status LIKE '%x' AND customer_id IN (SELECT id FROM customers WHERE email LIKE 'ab%')")
        self.assertIn("CREATE INDEX idx_customers_email ON customers (email);", self.creates(r))
        self.assertTrue(any("wildcard" in n and "status" in n for n in r["notes"]))
        r = self.suggest("SELECT * FROM orders WHERE customer_id IN (SELECT id FROM customers WHERE email LIKE '%x')")
        self.assertFalse(any(s["table"] == "customers" for s in r["suggestions"]))
        self.assertTrue(any("wildcard" in n and "in an IN subquery" in n for n in r["notes"]))

    def test_verify_tries_each_index_and_drops_it(self):
        r = self.suggest_verified("SELECT * FROM orders WHERE customer_id = 42")
        v = r["verification"]
        self.assertEqual(v["before"]["execution_ms"], 4.0)
        alone = v["each_index_alone"][0]
        self.assertEqual(alone["create_index"], "CREATE INDEX idx_orders_customer_id ON orders (customer_id);")
        self.assertEqual(alone["verdict"], "faster")
        self.assertEqual(alone["speedup"], 8.0)
        self.assertEqual(alone["execution_ms"], {"before": 4.0, "after": 0.5})
        self.assertEqual(alone["access"]["before"], ["Access: Table scan on orders"])
        self.assertEqual(alone["access"]["after"], ["Access: Index lookup on orders"])
        self.assertEqual(alone["rows_read_estimate"], {"before": 100, "after": 10})
        # the table is as it was: every index that was built is gone, and each one was built under a name of its own
        self.assertEqual(self.engine.created, [])
        built = [s for s in self.engine.statements if s.startswith("CREATE INDEX")]
        self.assertEqual(len(built), 1)
        self.assertIn("__try_", built[0])
        self.assertEqual(sum(1 for s in self.engine.statements if s.startswith("DROP INDEX")), 1)
        self.assertNotIn("left_behind", v)
        self.assertIn("dropped", r["caveat"])
        # the time is the median of three runs of the statement, each its own statement (the first reading of the plan, the three before, the three after)
        self.assertEqual(sum(1 for s in self.engine.statements if s.startswith("EXPLAIN ANALYZE SELECT * FROM orders WHERE customer_id = 42 /* try ")), 3 + 3)

    def test_verify_off_creates_nothing(self):
        self.suggest("SELECT * FROM orders WHERE customer_id = 42")
        self.assertFalse(any(s.startswith(("CREATE INDEX", "DROP INDEX")) for s in self.engine.statements))

    def test_verify_tries_two_indexes_together_too(self):
        r = self.suggest_verified("SELECT * FROM orders o JOIN customers c ON c.id = o.customer_id WHERE o.status = 'x' AND o.total > 5 AND c.email = 'e'")
        v = r["verification"]
        self.assertEqual(len(v["each_index_alone"]), 3)
        self.assertEqual(len(v["all_together"]["create_index"]), 3)
        self.assertEqual(self.engine.created, [])

    def test_verify_measures_the_rewrite_of_an_or_with_both_branch_indexes(self):
        r = self.suggest_verified("SELECT * FROM orders WHERE customer_id = 1 OR region_manager_id = 2")
        v = r["verification"]
        self.assertNotIn("each_index_alone", v)
        self.assertEqual(len(v["or_rewrite"]["create_index"]), 2)
        self.assertEqual(v["or_rewrite"]["rewrite"], r["rewrites"][0]["sql"].rstrip(";"))
        self.assertEqual(v["or_rewrite"]["verdict"], "faster")
        self.assertEqual(self.engine.created, [])
        self.assertTrue(any(s.startswith("EXPLAIN ANALYZE SELECT * FROM orders WHERE customer_id = 1 UNION SELECT") for s in self.engine.statements))

    def test_verify_names_an_index_it_could_not_drop(self):
        self.engine.fail_drop = True
        r = self.suggest_verified("SELECT * FROM orders WHERE customer_id = 42")
        v = r["verification"]
        self.assertEqual(len(v["left_behind"]), 1)
        self.assertIn("__try_", v["left_behind"][0])
        self.assertIn("drop them yourself", v["note"])

    def test_verify_leaves_a_big_table_alone_and_does_not_run_an_update(self):
        r = self.suggest_verified("SELECT * FROM big WHERE k = 7")
        self.assertIn("skipped", r["verification"]["each_index_alone"][0])
        self.assertFalse(any(s.startswith("CREATE INDEX") for s in self.engine.statements))
        r = self.suggest_verified("UPDATE orders SET status = 'x' WHERE customer_id = 9")
        self.assertNotIn("verification", r)
        self.assertTrue(any("verify needs a SELECT" in n for n in r["notes"]))
        self.assertFalse(any(s.startswith("CREATE INDEX") for s in self.engine.statements))

    def test_verify_says_so_when_there_is_nothing_to_try(self):
        r = self.suggest_verified("SELECT * FROM orders WHERE id = 5")
        self.assertEqual(r["verification"], {"note": "There is no index to try."})

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
        self.assertNotIn("verification", out)
        out = json.loads(mcp_server.suggest_indexes("SELECT * FROM orders WHERE customer_id = 1", "", True))
        self.assertIn("verification", out)


if __name__ == "__main__":
    unittest.main()
