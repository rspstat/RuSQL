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

verify_joins.py checks joins against a model of what SQL says (a RIGHT JOIN ... USING writes the merged columns, what is left of the right table, then of the left, as MySQL does): three small tables that share column names, random statements over 1 to 4
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
      autoinc (the AUTO_INCREMENT numbers a statement reserves, skips and loses: VALUES, IGNORE, ON DUPLICATE KEY UPDATE, REPLACE, INSERT ... SELECT, failures),
      shift (an UPDATE that moves a UNIQUE / PRIMARY KEY value onto the value another row of the statement still holds: written row by row in key or ORDER BY order),
      where_errors (the WHERE of an UPDATE / DELETE: a division by zero, a text that is no number compared with a number; a WHERE that compares an indexed column is left out),
      arith_types (a DATE, DATETIME, TIME, ENUM and SET column read as the number it stands for: arithmetic, numeric functions, CAST, comparisons with numbers),
      float (FLOAT columns shown with six significant digits; CAST(x AS FLOAT)),
      any_value (ANY_VALUE under ONLY_FULL_GROUP_BY; R51: ORDER BY ANY_VALUE(x) of a column the select list does not hold, HAVING on the alias of ANY_VALUE(x)), view_order (UPDATE / DELETE through a view with ORDER BY and LIMIT),
      windows (ROWS UNBOUNDED PRECEDING / ROWS n PRECEDING, WINDOW w AS (...), OVER w, OVER (w ORDER BY ...); rows that tie in ORDER BY are peers),
      window_agg (window functions over GROUP BY / aggregates: SUM(SUM(v)) OVER, RANK() OVER (ORDER BY SUM(v)), inside expressions, ORDER BY RANK() OVER, DISTINCT, views, derived tables, errors),
      max_execution_time (the hint and SET max_execution_time stop a SELECT with error 3024; COUNT(*) over a cross join is left out: MySQL counts it in a join buffer within the limit),
      fk_names (CONSTRAINT <name>, ALTER TABLE ... DROP FOREIGN KEY <name>, a drop and an add in one ALTER, information_schema.referential_constraints),
      range_frames (R50: RANGE BETWEEN n PRECEDING AND m FOLLOWING and INTERVAL n unit frames, ASC and DESC, NULL keys, frames that hold no row, the frames MySQL refuses),
      temporal_values (R50: the number a date function gives -- DATE(x) + 0, TIME(x), NOW(), CAST(x AS DATE) -- and SUM / AVG of a date, time or DATETIME column),
      float_single (R50: a FLOAT column as the single-precision number: f = d, f = 0.1, f + 0, SUM(f)),
      target_table (R50: error 1093 for an UPDATE / DELETE / INSERT whose subquery reads the table it writes),
      having_alias (R52: HAVING names the alias of a select-list expression, aggregate or column, also one that is the name of a column of the table),
      self_join_write (R52: UPDATE / DELETE that join a table with itself, the comma form, and the SET value that names another table by its alias),
      subquery_item_order (R52: ORDER BY the alias or position of a scalar subquery of the select list, DISTINCT over one),
      like_escape (R54: the backslash in a pattern and in a string, LIKE ... ESCAPE 'c' on a string, a column and an expression, the prefix path of an index, SHOW TABLES LIKE; 41 statements),
      fk_parent (R54: DELETE / UPDATE of a parent row whose child table is large enough for the cache of foreign key values, RESTRICT / CASCADE / SET NULL, a NULL parent key; the setup builds its rows from a table of ten digits; 47 statements),
      multi_fk (R57: a column in several foreign keys -- a value has to be in every parent (a value that is in one only is refused), UPDATE of the column, DELETE of a parent row that a child refers to, each key listed under its own name (key_column_usage, referential_constraints, table_constraints), one key that cascades and one that nulls, both cascading, ON UPDATE CASCADE into a column that another key guards (refused unless the other parent has the new value, nothing changed), the second key added by ALTER TABLE with a name of its own (a name is used once), DROP FOREIGN KEY of one of them, DROP TABLE of a table either refers to; 54 statements),
      float_copy (R57: a FLOAT column is shown with six digits also when MIN / MAX / GREATEST / LEAST / COALESCE / IFNULL / NULLIF give it, and is copied as the number it holds -- widened to a DOUBLE -- into DOUBLE, DECIMAL, INT and VARCHAR columns (INSERT ... SELECT, also of an aggregate or a COALESCE; UPDATE ... SET), as it is into a FLOAT; 43 statements),
      float_mixed (R58: a FLOAT met by a DOUBLE, an integer column, a DECIMAL or a fraction in COALESCE / IFNULL / GREATEST / LEAST is the float widened (0.1 is 0.10000000149011612), NULLIF has the type of its first argument and compares the float widened, arithmetic and comparisons over a function that is a FLOAT read it widened, MIN / MAX / LAG / LEAD / FIRST_VALUE / LAST_VALUE / NTH_VALUE of a FLOAT column over a window are shown with six digits; 52 statements),
      using_chain (R58: the order of the columns of * over a chain of USING / NATURAL joins -- (a JOIN b USING (x)) JOIN c USING (y) shows y, x, ... --, the merged columns of one join in the order the left side has them, RIGHT JOIN ... USING swapping the sides, ON joins in between; 21 statements),
      fk_auto_names (R58: the names a foreign key without a name is given -- <table>_ibfk_<n> in the order written, only those without a name counted, ALTER ADD one more than the highest, DROP leaves the others, RENAME TABLE changes the prefix, a parent that is renamed is followed by its children -- and NO ACTION / RESTRICT in referential_constraints; 51 statements),
      errno (R58, run with --proto --errno: the error number a client gets for a statement that fails on both -- 1062, 1048, 1364, 1406, 1264, 1366, 1265, 1292, 1451, 1452, 3819, 3730, 1054, 1146, 1050, 1051, 1060, 1061, 1091, 1052, 1066, 1093, 1110, 1136, 1241, 1242, 1111, 1365, 1305, 1426, 1582, 1690, 1007, 1008, 1049, 3579, 1210, 1064; 49 statements),
      rollup (R60: GROUP BY ... WITH ROLLUP -- after the groups one row per prefix of the keys (the rest NULL) and a last row for all of them, the aggregates of such a row over all its rows (AVG and COUNT(DISTINCT) too), HAVING looking at those rows, ORDER BY / LIMIT / DISTINCT after them, a group of NULL keys that is not a rolled-up row, an input of no row without a total, a derived table, a view, UNION, the syntax errors, the first row's value of a column that is no key and the last row's in a rolled-up row (ANY_VALUE), no key dependence under ROLLUP; 39 statements),
      slow_log (R59: SET GLOBAL slow_query_log takes ON / OFF / TRUE / FALSE / 1 / 0 (also as text) and refuses another word or number with 1231, SET GLOBAL long_query_time takes a number, refuses text with 1232 and brings a number out of range into 0 .. 31536000 (MySQL warns and sets it), read back with @@global.*; FOUND_ROWS() is the number of rows after an aggregate whose answer is one row (RuSQL's text for it has no "1 row(s) returned." line), a HAVING that drops it, GROUP BY, DISTINCT, LIMIT; 53 statements),
      transactions (R71: statements inside a transaction -- SAVEPOINT / ROLLBACK TO / RELEASE (1305), DDL that commits the open transaction, START TRANSACTION READ ONLY / READ WRITE, SET [SESSION] TRANSACTION and SET transaction_isolation / transaction_read_only (1231, 1568, one transaction only), COMMIT / ROLLBACK AND [NO] CHAIN, FOR UPDATE; not compared: the isolation level of a session that set nothing (MySQL REPEATABLE-READ, RuSQL READ-COMMITTED) and SET @@transaction_isolation without SESSION; 136 statements),
      triggers_writes (R71: BEFORE / AFTER INSERT, UPDATE, DELETE triggers that change NEW (several assignments in one SET, IF / CASE / compound bodies), write an audit table or a table with a trigger of its own, REPLACE and ON DUPLICATE KEY UPDATE firing them, a trigger that fails takes the statement back, procedures that write in loops; 54 statements),
      views_update (R71: INSERT / UPDATE / DELETE through a view -- columns the view does not show, renamed columns, an expression column (1348 / 1471), a view with a WHERE, a view over a view, LIMIT / aggregate / DISTINCT views (1288), a join view DELETE (1395), WITH [CASCADED | LOCAL] CHECK OPTION (1369); not compared: UPDATE and INSERT through a join view; 51 statements),
      fk_actions (R71: RESTRICT / NO ACTION / CASCADE / SET NULL on DELETE and UPDATE of a parent, in a transaction, with REPLACE / ON DUPLICATE KEY, TRUNCATE of a referenced table (1701), DROP of one, FOREIGN_KEY_CHECKS = 0 (the parent side too), ALTER TABLE ADD / DROP FOREIGN KEY, the definition of a key (1824 / 3780 / 1830); 77 statements),
      dates_arith (R71: month ends, leap years, INTERVAL in every unit, DATEDIFF / TIMESTAMPDIFF / PERIOD_*, week numbers, DATE_FORMAT / STR_TO_DATE (%j), TIME limit 838:59:59, strings that are no dates, CONVERT_TZ, a DATE column against a date string and a DATETIME; not compared: INTERVAL before the date, DATE_SUB year 0, INTERVAL '1.5' DAY, UNIX_TIMESTAMP / FROM_UNIXTIME (time zone); 45 statements),
      replace_chain (R57: REPLACE of a row that has children below it: refused with nothing changed -- not even the rows the statement replaced before -- when a RESTRICT is anywhere down the chain, the whole chain goes when every table cascades, SET NULL in the middle, a table that is its own child, VALUES / SELECT / SET forms; 33 statements),
      fk_chain (R56: foreign keys over several tables, a <- b <- c ...: a cascade goes down the whole chain (CASCADE to the grandchildren, SET NULL below a CASCADE, SET NULL above one), a RESTRICT anywhere below refuses the statement before anything has changed (also for the whole table, in either order), the tables are asked in the order of their names (a diamond: c refers to a and to b), a cycle across two tables ends, SET NULL on a column that grandchildren refer to, ON UPDATE CASCADE through a chain and a RESTRICT below it, ROLLBACK and SAVEPOINT; every scenario has tables of its own; 62 statements),
      empty_columns (R56: an answer of no rows names its columns as MySQL names them -- columns, aliases, `*`, `t.*`, joins, aggregates, GROUP BY / HAVING, DISTINCT, derived tables, views, CTE (also with a column list), UNION, a select-list subquery, a window function, CASE, EXECUTE, SHOW TABLES LIKE, and from R57 USING / NATURAL / RIGHT JOIN ... USING (the merged columns first, then what is left of the right table for a RIGHT JOIN), a CALL, a string constant that names its column (`SELECT 'a'`), a select without FROM that has a WHERE, a UNION whose left side finds no rows; vs_mysql.py compares the names of an empty answer, and a column-less answer where MySQL has columns is a difference; 81 statements),
      self_fk (R55: a foreign key of a table to itself (emp.mgr -> emp.id): DELETE row by row in the order of the key or the ORDER BY -- RESTRICT refuses while a child is there, CASCADE deletes the children in turn and stops at 14 levels (error 3008), SET NULL rewrites them, a row that is its own parent is its own child --, the count of rows a statement deletes (a row a cascade deleted is not reached by the scan), a WHERE that an earlier SET NULL changed, two self references on one table, REPLACE, ON UPDATE CASCADE / SET NULL acting as RESTRICT, other tables below the rows a cascade deletes, a text key that ignores case; 79 statements),
      json_functions (R77: how a JSON document is written -- members ordered, `, ` and `: `, numbers as MySQL keeps them --, JSON_VALID / TYPE / LENGTH / KEYS / DEPTH, paths in JSON_EXTRACT / -> / ->> (last, ranges, * and **, quoted keys, several paths) and the errors 3141 / 3143 / 3149 / 3153 / 3154 / 3158 / 3165, JSON_SET / INSERT / REPLACE / REMOVE / ARRAY_APPEND / ARRAY_INSERT, JSON_MERGE_PATCH / PRESERVE, JSON_CONTAINS / CONTAINS_PATH / OVERLAPS / SEARCH, MEMBER OF, JSON_QUOTE / UNQUOTE / PRETTY, JSON_VALUE ... RETURNING, JSON_ARRAYAGG / OBJECTAGG, CAST AS JSON, comparing and sorting JSON values, a comma before the closing parenthesis; 245 statements; not compared: JSON_TYPE of a JSON null (the text NULL is SQL NULL here), a number written 1e3 in JSON_ARRAY, ORDER BY over several objects or arrays),
      privileges (R78, on the scoreboard, run with --proto --errno: 327 statements since R79, 0 differences; it was 115 statements with 90 different in R77, when GRANT was only recorded and no statement checked it). It uses `@name statement` lines -- the statement is run by that user on a connection of his own, password 'pw' -- and every scenario has a user of its own, because MySQL gives a connection the privileges of a database when it connects or says USE (a GRANT on a database during the connection shows after the next USE). CREATE / ALTER / RENAME / DROP USER, GRANT / REVOKE on a table, a database, everything and columns, WITH GRANT OPTION, SHOW GRANTS (also USING a role), what a user with one privilege may and may not do (1142 / 1143 / 1044 / 1227 / 1370), roles (CREATE ROLE, GRANT role, roles that hold roles, SET ROLE, SET DEFAULT ROLE, CURRENT_ROLE()), the errors of GRANT and REVOKE (1410 / 1147 / 1141 / 1269 / 3619 / 1221 / 1144 / 3523 / 3530 / 4027 / 1396). R79 adds a login that names a database (an account with no access to it is refused with 1044, a name that is not there with 1049; the server tells clients it takes the database in the handshake) and the columns DESCRIBE / SHOW COLUMNS / information_schema.columns show to an account that holds a column privilege only.
      text_number_in (R79, on the scoreboard: 84 statements, 0 differences): a text against a number in IN (subquery) and in CHECK -- a number on either side makes it a comparison of numbers (a text is read by the number it starts with: '007' IN (SELECT 7), 'abc' IN (SELECT 0), 7 IN (SELECT '007'); UPDATE / DELETE refuse a text that is no number with 1292), two texts compare as texts ('9' > '10'), also in ADD CHECK, UPDATE, REPLACE, INSERT ... ON DUPLICATE KEY UPDATE and for a row `(a, b) IN (SELECT x, y ...)`.
      dual_order_limit (R79, on the scoreboard: 53 statements, 0 differences): ORDER BY / LIMIT / OFFSET after a SELECT without FROM, alone and at the end of a UNION / INTERSECT / EXCEPT, in derived tables, subqueries and INSERT ... SELECT; a chain of three or more is read left to right with INTERSECT before UNION and EXCEPT and the ORDER BY / LIMIT at its end for the whole result.
      create_database (R79, on the scoreboard, run with --proto: 43 statements, 0 differences): CREATE DATABASE leaves the session in its database, CREATE TABLE ... AS SELECT reports the rows it copied (OK packet, ROW_COUNT()), CREATE DATABASE says 1 affected row and DROP DATABASE the number of tables and views it took away.
      join_view_writes (R80, on the scoreboard, run with --proto --errno: 68 statements, 0 differences; 65 differed): UPDATE and INSERT through a view over a join -- one base table, each row of it once, the WHERE of the view and of the statement, a view over a join view, a comma join, an alias of a table, ON DUPLICATE KEY UPDATE, INSERT ... SELECT -- and the errors 1393 / 1394 / 1348 / 1288 / 1471 / 1395 / 1221 / 1423.
      dual_paren_union (R80, on the scoreboard: 24 statements, 0 differences; 23 differed): FROM DUAL, and a bracketed UNION / INTERSECT / EXCEPT operand with an ORDER BY / LIMIT / OFFSET of its own.
      errno_more (R80, on the scoreboard, run with --proto --errno: 64 statements, 0 differences; 35 differed): the error number (and the check that comes first) of statements the other corpora had left different in number only -- ONLY_FULL_GROUP_BY 1055 / 1140, window functions 3579 / 3587 / 3588 / 3589 / 3593, the MOD operator and 1365, 1052 before 1093, DATETIME(3) past 9999-12-31 1441, unsigned arithmetic 1690, SELECT ... INTO a short variable 1265, ALTER ... NOT NULL over a NULL 1138 / 1265, ADD COLUMN ... NOT NULL (the zero of the type, the zero date 1292), the parent side of a foreign key before the child side 1451 / 1452.
      wrong_answers_r82 (R82, on the scoreboard, run with --proto --errno: 87 statements, 0 differences; 61 of them differed before the fixes -- the answers that were wrong without an error): hexadecimal / bit literals (0x4142, X'41', 0b101, b'101') as the bytes they spell and the unsigned integer where a number is asked (arithmetic, comparison with a number, CAST), _utf8mb4'x' / _binary 'x' / N'x', COMMENT of a column and a table (information_schema, SHOW CREATE TABLE, ALTER, LIKE), SELECT * / t.* over a LATERAL derived table (also an empty answer's column names), SHA2 of 224 / 384 / 512 bits, MEDIUMINT range, TINYTEXT / TEXT / TINYBLOB sizes and CHAR(n) (spaces at the end).
      collate_clause (R77, NOT on the scoreboard: it measures what is different): a constant or a column under another collation, BINARY, COLLATION() / CHARSET(), a column or a table declared with a collation, ALTER ... MODIFY ... COLLATE: 62 of 69 statements differ, the clause is not supported. The file is the starting point of the work that is left (RECOMMAND.md, table 7).
      tool_gaps (R81, NOT on the scoreboard: it measures what is different; run with --proto --errno): what real MySQL tools and hand-written SQL send that RuSQL does not answer like MySQL -- mysqldump (LOCK TABLES ... LOCAL, SELECT /*!40001 SQL_NO_CACHE */, SHOW CREATE DATABASE / PROCEDURE / FUNCTION / TRIGGER), schema tools (information_schema routines / parameters / triggers / check_constraints, key_column_usage without UNIQUE keys, views, SHOW FULL TABLES, SHOW FULL COLUMNS FROM t FROM db LIKE), a string as alias, select modifiers, sql_mode, small syntax: 43 of 48 statements differ (R82 fixed the silent wrong answers it had listed -- hex / bit literals, LATERAL, SHA2, COMMENT, MEDIUMINT / TINYTEXT / CHAR -- and moved them to wrong_answers_r82). The file is the starting point of the work that is left (RECOMMAND.md, rows marked R81).
  python two_sessions.py <engine_server.exe> [--level RC|RR|both] [--only NAME] [--show]
      two sessions A and B (A at the level asked for) run the same steps on RuSQL and on the real MySQL and every step's answer is compared (the rows, the rows a DML statement
      affected, the error number): what a transaction sees of what another session commits and of its own writes, which a single connection (vs_mysql.py) cannot ask.
      38 scenarios x 2 levels, 596 steps, 0 differences (KNOWN, the scenarios whose differences are known and not fixed, is empty since R74).
      Since R73 it also covers when the snapshot is taken (the first read, not BEGIN; a write, a locking read and a SELECT without a table take none; START TRANSACTION WITH CONSISTENT SNAPSHOT takes it at once;
      the source of an INSERT ... SELECT is read as a locking read). R74 adds: a statement that is not a plain SELECT reads its subqueries as the latest commit (INSERT / REPLACE / SET /
      UPDATE / DELETE), an INSERT ... SELECT locks the rows it reads for sharing, a locking read that waited reads what the other transaction committed, a connection that quits gives up
      its transaction. B gives up on a lock after 2 seconds (error 1205); a step "A~" runs A's statement in a thread (it waits for a lock), "A<" is its answer, "A!" closes A's
      connection and opens a new one. A step that waits more than 15 seconds comes back as error 2013.
      tests/test_repeatable_read.cpp keeps the same sequences without a MySQL.
  python make_scoreboard.py <engine_server.exe> [--tests CASES ASSERTIONS]
      runs every corpus (the protocol ones with --proto) and writes the numbers the app's Compat page shows to code/frontend/src/lib/compatSnapshot.json.
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
      CLIENT_FOUND_ROWS mode is the unit test [r44][affected]) and last insert id, binary values). Three differences are left: the values of SHOW TABLE STATUS other than the name / engine / row
      count, SHOW ENGINES lists one engine, and @@version is 5.7.0-rusql (on purpose, the handshake's; VERSION() says the same since R50).
  python vs_mysql.py <engine_server.exe> corpus/errno.sql --proto --errno
      also compares the error number of a statement that fails on both servers (pymysql's (errno, message)); the protocol used to answer every error with 1064. server/src/mysql.cpp tells
      the number and SQL state from how the engine's message begins or what it says (wire_error); about 20 statements of the other corpora still differ in number only because the engine fails
      them for another reason than MySQL does (syntax it does not take, a message that says nothing).
  python vs_mysql.py <engine_server.exe> corpus/protocol_types.sql --proto --types
      also compares the kind of every result column the client is told (integer, decimal, float, date, datetime, time, text -- what pymysql turns
      it into) with MySQL's. Four differences are left: ROUND(double column, 1) is told as a decimal, `2 * 3.5` = 7.0 reads as an integer,
      information_schema.columns has 28 columns of the database where MySQL has 32, VERSION().
  python prepared_wire.py <engine_server.exe> corpus/prepared_wire.txt [--show N]
                                       prepared statements through the BINARY protocol (COM_STMT_PREPARE / EXECUTE / CLOSE: what a Go, Java or PHP driver and mysql-connector's prepared cursor
                                       use) on RuSQL and on the real MySQL: the rows and the Python types they come back as, the column names, the affected rows and last insert id,
                                       the error number. A line is `SQL ;; (params) ;; (params)`: prepared once, executed once per tuple (ints, floats, strings, None, Decimal, date,
                                       datetime, time, bytes). 81 executions, 0 differences; not compared: the name of a select-list expression with a ? in it (MySQL keeps the ?), the
                                       last insert id of REPLACE / ON DUPLICATE KEY UPDATE with an explicit AUTO_INCREMENT value, the column count COM_STMT_PREPARE answers (RuSQL: 0).
  python mysql_probe.py <file.sql>     runs each line on MySQL and prints the outcome (to find out what MySQL says before writing a test)
  collation_model.py                   the ASCII weights of utf8mb4_0900_ai_ci, for the models inside the verify_*.py tools (generated by
                                       code/test/collation/gen_ascii_model.py from a dump of MySQL's WEIGHT_STRING)
  python dump_roundtrip.py <engine_server.exe> dump_schemas/shop.sql
                                       builds the schema on the real MySQL (database rt_dump), takes a real `mysqldump` of it (with --routines --triggers), loads
                                       that file into RuSQL over the MySQL protocol the way the mysql client does (DELIMITER, comments, /*!NNNNN */ kept), lists each
                                       statement RuSQL refused with its answer, compares every table's rows with MySQL's, and loads the file once more with the
                                       real `mysql` client (--force) to count its errors. On the R17 build: 92 of 102 statements are refused and no table loads.
