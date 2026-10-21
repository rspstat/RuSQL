Build-to-build differential test
===============================
For a change that must not alter any result (a faster aggregate / GROUP BY / DISTINCT / join path, a different row
format, ...): run the same random SELECT corpus on two engine_server builds and compare what they print.

  python diff_builds.py <old engine_server.exe> <new engine_server.exe> [--queries N] [--seed S] [--rows R]
                        [--parallel | --modes seq,par] [--exact] [--number-form] [--skip-chain-refs] [--max-report K] [--diff-lines L] [--show N] [--log-new FILE]

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
  --no-null-arithmetic  the corpus's `SET val = val + 1` skips NULL rows: builds before the NULL fix stored "NULL1" in the NULL rows,
                     which would make the two tables (and every later answer) differ; the remaining differences are `val * 2` over NULL
  --no-empty-strings the corpus writes NULL where it would write an empty string: builds before 2026-10-05 stored '' as NULL
  --order-as-sets    a statement with ORDER BY is compared as a set of lines and one with ORDER BY + LIMIT / OFFSET not at all: for a change
                     of where NULL sorts (builds before 2026-10-05 compared NULL as the text "NULL"), which moves rows inside an order and
                     across a LIMIT
  --skip-chain-refs  no ON clause that reads a table joined earlier: builds before b3ca102 answered `t JOIN u ON u.id = t.grp
                     JOIN w ON w.k = u.id` wrongly (the left column of the second join was read by its bare name from the
                     FROM table), so such queries differ from the fixed build by design
  --number-form      compare numbers by value (30.7500 is 30.75) and ignore the padding: for a change of the places a DECIMAL is written with
  --ignore-header    compare the rows only (not the result-column names, separator lines or cell padding): for a change
                     of how columns are NAMED, to show that no value moved
  --diff-lines L     how many lines only one of the two answers has are listed for a difference (default 3; all of them when many
                     differences are to be classified: every cell of the answers, not three lines)
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
as the statement wrote them (--no-header-check for builds before that). An aggregate of no value (SUM, AVG, MIN, MAX of an empty group or
only NULLs) is NULL, and a HAVING drops a group whose aggregate is NULL.

  python verify_aggregates.py <engine_server.exe> [--queries N] [--seed S] [--rows R] [--no-header-check]

verify_agg_expressions.py checks aggregates INSIDE select-list expressions, functions and CASE (`MAX(x) - MIN(y)`,
`ROUND(AVG(x), 1)`, `COALESCE(SUM(x), 0)`, `CASE WHEN COUNT(*) > 3 ...`) the same way: every group of the answer is computed
here from the un-aggregated rows of the same FROM, and a scalar statement has to return exactly one row. NULL goes through the expression
(`SUM(x) + 1` of nothing is NULL, `COALESCE(SUM(x), 0)` is 0, a CASE whose condition is NULL takes the ELSE).

  python verify_agg_expressions.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

verify_null_expressions.py checks arithmetic, comparisons and scalar functions over NULL values: `val + 1`, `val * price`, `grp / val`,
`(x - y) * z`, `ABS(x - y)`, `ROUND(x / y, 2)`, `x > y` over single tables and INNER / LEFT / RIGHT / FULL joins (the join padding is NULL),
aliased or not. The expression is computed here from the plain columns of the same FROM / WHERE (NULL operand -> NULL, x / 0 -> NULL),
and the SELECT answer, a WHERE on the expression and an UPDATE ... SET r = <expression> read back must all agree (an UPDATE over a row that divides by zero
is refused with error 1365 and writes nothing, as in MySQL: the tool checks that instead).

  python verify_null_expressions.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

verify_writes.py checks INSERT / REPLACE / INSERT ... ON DUPLICATE KEY UPDATE / UPDATE / DELETE, UPDATE ... JOIN, DELETE ... JOIN and MERGE
against a model of what MySQL does: two tables with NOT NULL, UNIQUE, a typed column and a RESTRICT foreign key, random statements over
values that include NULLs, numeric strings, fractions and text that does not fit. Every statement either succeeds or fails and changes
NOTHING (statement atomicity); after every statement both tables are read back and compared with the model.

  python verify_writes.py <engine_server.exe> [--statements N] [--seed S]

verify_joins.py checks joins against a model of what SQL says: three small tables that share column names, random statements over 1 to 4
table uses (a table used twice with an alias for each use, written `t x` or `t AS x`; INNER / LEFT / RIGHT / FULL OUTER / CROSS, the comma form,
NATURAL and USING; derived tables `(SELECT * FROM t WHERE ...) AS d` in FROM and in JOIN; ON conditions of several parts; WHERE with AND / OR /
NOT / IS NULL; `*`, `t.*`, qualified columns, COUNT(*)). Every answer is computed here by nested loops with three-valued logic and compared as a
multiset of rows together with the column names.

  python verify_joins.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

verify_compare.py checks comparisons, ordering, MIN / MAX and joins against MySQL's rules for the TYPES of the two sides (two strings compare
as strings, a number against anything compares as numbers, a string read by the number it starts with). Four tables hold the same random rows --
a VARCHAR column, an INT, a DECIMAL and a BIGINT (values around 2^53) -- with no index, with an index on every column, with hash indexes, and
with the VARCHAR column as the PRIMARY KEY. Each random WHERE (= <> < <= > >=, BETWEEN, IN, NOT IN, AND / OR; the literal written as a string or as
a number), ORDER BY, MIN / MAX and JOIN (text = text, number = number, text = number) is run on each table and compared with a reference computed
here from the rows with exact rational arithmetic.

  python verify_compare.py <engine_server.exe> [--statements N] [--seed S] [--rows R]

verify_correlated.py checks subqueries that name a column of the query around them against a model: EXISTS / NOT EXISTS / COUNT / SUM / MIN / MAX / IN / a subquery inside the
subquery, with the outer column on either side of a comparison, inside ABS / + k / * 2 / COALESCE, numbers and strings, NULLs, the outer table and the inner one under aliases or the
same table; DELETE and UPDATE of a copy of the table.

  python verify_correlated.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

verify_sort_group.py checks ORDER BY and GROUP BY against a model: rows sorted by a mix of columns, t.column, names the select list gives, positions and expressions (numbers
and strings, ASC / DESC, ties by the next key and the id); groups by an expression (spelled out, by its name or by position) sorted by a count, a sum, a position or an
expression of aggregates; a join sorted by an alias, a position or an expression; UNION [ALL] sorted by position or name.

  python verify_sort_group.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

verify_value_expressions.py checks expressions against a model: tables t(id, g, x, y, w, s) of random integers, two-place decimals and short strings (NULLs included),
and random expressions -- arithmetic, ABS, COALESCE / IFNULL / NULLIF, CASE (searched and simple), IF, conditions as values (comparison, IS [NOT] NULL / TRUE / FALSE, BETWEEN,
IN with a NULL in the list, LIKE, AND / OR / NOT), strings (CONCAT, UPPER) -- in the select list, WHERE (the rows where it is TRUE, and where NOT of it is TRUE), aggregates of
the expression, SUM / COUNT of a CASE with GROUP BY and HAVING, an expression around aggregates, UPDATE ... SET and INSERT of constants. Exact rational arithmetic, three-valued logic.

  python verify_value_expressions.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

verify_subqueries.py checks scalar subqueries against a model of what SQL says: tables t(id, v, g) and u(id, a, b, g) with random rows (NULLs included), and
random statements -- a scalar subquery in WHERE (uncorrelated and correlated), in the select list, in HAVING (with and without GROUP BY), IN / NOT IN
with a NULL in the list (three-valued), a subquery of two columns, UPDATE / DELETE with a subquery (an error changes nothing). Two rows are an error
("Subquery returns more than 1 row"), no row is NULL, two columns are an error ("Operand should contain 1 column(s)").

  python verify_subqueries.py <engine_server.exe> [--statements N] [--seed S]

verify_agg_arguments.py checks aggregates whose argument is an EXPRESSION (SUM(x * y), AVG(COALESCE(w, 7) - y), COUNT(DISTINCT ...), MIN / MAX) with exact
rational arithmetic: a table of integers, two-place decimals and strings with NULLs, and random expressions over + - * and parentheses, run as a whole-table
aggregate, GROUP BY, HAVING, a window function (PARTITION BY) and through a JOIN with table aliases.

  python verify_agg_arguments.py <engine_server.exe> [--queries N] [--seed S] [--rows R]

To check that the comparison can see a bug, plant one on purpose in the new build (for instance read an unqualified column
from the last table that has it instead of the first) and run the same seed: it has to report differences.

Comparison with a real MySQL (needs pymysql and a MySQL 8.4 on 127.0.0.1:3391; user root without a password)
------------------------------------------------------------------------------------------------------
  python vs_mysql.py <engine_server.exe> corpus/<name>.sql [--emit cases.inc]
      runs the statements of a corpus on RuSQL and on MySQL and lists the ones whose answers differ (a line is a statement, '#' a comment, '--' a setup
      statement run on both). --emit writes the cases of the C++ conformance tests code/backend/tests/mysql_conformance_<name>.inc: MySQL's answer is the
      expected one for every statement RuSQL answers the same; a statement it answers differently is only run, so the tables stay as MySQL had them.
      The corpora: functions, tables, constraints, collation (the default collation utf8mb4_0900_ai_ci), decimals (--exact, see below), temporal (fractional seconds,
      the forms of a date and a time, INTERVAL / EXTRACT units, column types and index options; a DATETIME(3) value is read with the three places the column has),
      routines (stored functions and procedures with typed parameters / variables / RETURNS, division by zero, prepared statements, triggers and LAST_INSERT_ID),
      schema_change (ALTER TABLE MODIFY / ADD / DROP COLUMN: the values converted, the indexes after them),
      autoinc (the AUTO_INCREMENT numbers a statement reserves, skips and loses: VALUES, IGNORE, ON DUPLICATE KEY UPDATE, REPLACE, INSERT ... SELECT, failures).
      The cells of RuSQL's answer are read the way the engine itself reads a result table (\\ \n \| and \s \t for a space or tab at either end of a value,
      see engine/result_text.hpp), so a value such as ' x ' or 'a|b' is compared as it is; corpus/tables.sql has probes for them (R40) and for CTE columns.
  --exact
      the cells must be the same TEXT, not only the same number (30.7500 is not 30.75, 21 is not 21.00). With --emit the rows are written as kind 5, which
      test_mysql_conformance.cpp compares so (mysql_conformance_decimals.inc, from corpus/decimals.sql: DECIMAL and integer results only -- a DOUBLE comes to a
      client as a float, so its text is not the engine's). Together with --proto the client's values are compared (Decimal('30.75'), 6.75), which is how
      probe files for the places of a number are checked.
  python vs_mysql.py <engine_server.exe> corpus/protocol.sql --proto
      the same comparison, but RuSQL is asked over its MySQL protocol (pymysql, port 13399) instead of the native port, so what the protocol layer of
      server/src/mysql.cpp makes of a statement is compared too (SHOW COLUMNS / INDEX / CREATE TABLE / TABLE STATUS, SHOW VARIABLES, the OK packet's
      affected rows (a number now compared, R44: the rows an UPDATE changed, ON DUPLICATE KEY UPDATE 1 / 2 / 0, REPLACE 2, a trigger's statements not counted; the
      CLIENT_FOUND_ROWS mode is the unit test [r44][affected]) and last insert id, binary values). Four differences are left: the values of SHOW TABLE STATUS other than the name / engine / row
      count, SHOW ENGINES lists one engine, SHOW COLLATION has no Pad_attribute column, and @@version is 5.7.0-rusql (on purpose, the handshake's).
  python vs_mysql.py <engine_server.exe> corpus/protocol_types.sql --proto --types
      also compares the kind of every result column the client is told (integer, decimal, float, date, datetime, time, text -- what pymysql turns
      it into) with MySQL's. Four differences are left: ROUND(double column, 1) is told as a decimal, `2 * 3.5` = 7.0 reads as an integer,
      information_schema.columns has 28 columns of the database where MySQL has 32, VERSION().
  python mysql_probe.py <file.sql>     runs each line on MySQL and prints the outcome (to find out what MySQL says before writing a test)
  collation_model.py                   the ASCII weights of utf8mb4_0900_ai_ci, for the models inside the verify_*.py tools (generated by
                                       code/test/collation/gen_ascii_model.py from a dump of MySQL's WEIGHT_STRING)
  python dump_roundtrip.py <engine_server.exe> dump_schemas/shop.sql
                                       builds the schema on the real MySQL (database rt_dump), takes a real `mysqldump` of it (with --routines --triggers), loads
                                       that file into RuSQL over the MySQL protocol the way the mysql client does (DELIMITER, comments, /*!NNNNN */ kept), lists each
                                       statement RuSQL refused with its answer, compares every table's rows with MySQL's, and loads the file once more with the
                                       real `mysql` client (--force) to count its errors. On the R17 build: 92 of 102 statements are refused and no table loads.
