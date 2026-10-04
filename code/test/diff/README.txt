Build-to-build differential test
===============================
For a change that must not alter any result (a faster aggregate / GROUP BY / DISTINCT / join path, a different row
format, ...): run the same random SELECT corpus on two engine_server builds and compare what they print.

  python diff_builds.py <old engine_server.exe> <new engine_server.exe> [--queries N] [--seed S] [--rows R]
                        [--parallel | --modes seq,par] [--exact] [--skip-chain-refs] [--max-report K] [--show N] [--log-new FILE]

Both servers get the same schema (4 tables: t, u, v, w), data (NULLs, numeric-looking text such as '7' / '007' / '7.0',
empty strings), mutations and queries, and every answer is compared as text (the "(0.001 sec)" timing line is ignored).
The queries are generated from the seed: every aggregate function (FILTER, DISTINCT forms), GROUP BY / HAVING, DISTINCT,
ORDER BY / LIMIT / OFFSET in every combination, window functions, subqueries, FOR UPDATE, statements inside an explicit
transaction at every isolation level, and joins (inner / left, up to three tables, ON clauses with one or several
conditions, WHERE clauses that read one table or several, identifier-looking literals that name a column of another
table, ambiguous bare names), subqueries in WHERE and in UPDATE / DELETE (scalar comparisons that return one row, several,
none, NULL or an error; EXISTS / NOT EXISTS / IN / NOT IN, correlated or not) and `<indexed equality> AND ...` conditions.
A statement that fails must fail with the same message on both builds.

  default            both servers run with RUSTDB_PARALLEL=0: ties in ORDER BY come out in the same order, the text must match
  --parallel         the thread pool on for both, answers compared as sets of lines (an unstable parallel sort orders tied
                     rows differently, and a floating-point SUM over another row order prints 4711 or 4711.0000)
  --modes seq,par    first server sequential, second with the thread pool; with --exact the text itself is compared:
                     pass the same build twice to prove it gives one answer whatever the parallelism
  --skip-chain-refs  no ON clause that reads a table joined earlier: builds before b3ca102 answered `t JOIN u ON u.id = t.grp
                     JOIN w ON w.k = u.id` wrongly (the left column of the second join was read by its bare name from the
                     FROM table), so such queries differ from the fixed build by design
  --ignore-header    compare the rows only (not the result-column names, separator lines or cell padding): for a change
                     of how columns are NAMED, to show that no value moved
  --log-new FILE     the new server's stderr (for builds that print debug output)
  --rows R           table size (default 400); above 3000 the generator drops LEFT JOINs, three-way joins and multi-condition
                     ON clauses, which are nested loops (minutes per query) in older builds

Exit code 1 and the query plus both outputs for the first --max-report differences. A seed is deterministic.

diff_builds.py cannot say WHICH of two builds is right. verify_orderby_distinct.py checks one build by itself, on the same
corpus (tables that share column names, columns spelled `t.id`): every DISTINCT statement returns the distinct rows of the
statement without DISTINCT, every ORDER BY statement returns the rows of the statement without it, in order of its first key
when that key is a selected column.

  python verify_orderby_distinct.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

verify_aggregates.py does the same for aggregates over `table.column` arguments (COUNT, COUNT DISTINCT, SUM, AVG, MIN, MAX,
grouped or not, over INNER / LEFT / RIGHT / FULL OUTER joins of tables that share column names, with aliases and WHERE):
every group of the answer is recomputed from the un-aggregated rows of the same FROM, and the result columns have to be named
as the statement wrote them (--no-header-check for builds before that).

  python verify_aggregates.py <engine_server.exe> [--queries N] [--seed S] [--rows R] [--no-header-check]

To check that the comparison can see a bug, plant one on purpose in the new build (for instance read an unqualified column
from the last table that has it instead of the first) and run the same seed: it has to report differences.
