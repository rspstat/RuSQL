"""Runs every corpus against a real MySQL and writes the numbers the app's Compat page shows (code/frontend/src/lib/compatSnapshot.json).

  python make_scoreboard.py <engine_server.exe> [--tests CASES ASSERTIONS]

For each corpus it runs vs_mysql.py (the corpus on RuSQL and on MySQL 8.4 at 127.0.0.1:3391) and reads the last line it prints:
`statements=N different=M (both fail: K)`. `--tests` records the size of the C++ test run (cases and assertions) next to them.
"""
import datetime, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "frontend", "src", "lib", "compatSnapshot.json")
INC_DIR = os.path.join(HERE, "..", "..", "backend", "tests")

# name, flags, what it checks (EN, KO)
CORPORA = [
    ("tables", [], "Tables, joins, subqueries, windows, CTEs, views", "표·조인·서브쿼리·윈도·CTE·뷰"),
    ("functions", [], "Built-in functions (strings, math, dates, JSON)", "내장 함수 (문자열·수학·날짜·JSON)"),
    ("constraints", [], "Types, NOT NULL, UNIQUE, CHECK, foreign keys, strict mode", "타입·NOT NULL·UNIQUE·CHECK·외래키·엄격 모드"),
    ("collation", [], "Default collation utf8mb4_0900_ai_ci", "기본 정렬 규칙 utf8mb4_0900_ai_ci"),
    ("decimals", ["--exact"], "DECIMAL arithmetic, to the last digit", "DECIMAL 산술, 마지막 자리까지"),
    ("temporal", [], "Dates, times, fractional seconds", "날짜·시간·소수초"),
    ("routines", [], "Stored procedures, functions, triggers, prepared statements", "저장 프로시저·함수·트리거·준비된 문장"),
    ("schema_change", [], "ALTER TABLE and the indexes after it", "ALTER TABLE과 그 뒤의 인덱스"),
    ("autoinc", [], "AUTO_INCREMENT numbers a statement takes and loses", "문장이 쓰고 잃는 AUTO_INCREMENT 번호"),
    ("shift", [], "UPDATE that shifts UNIQUE / PRIMARY KEY values", "UNIQUE·PRIMARY KEY 값을 밀어 옮기는 UPDATE"),
    ("where_errors", [], "Division by zero / text-vs-number in UPDATE and DELETE", "UPDATE·DELETE의 0 나누기·글자-수 비교"),
    ("arith_types", [], "Dates, ENUM, SET read as numbers", "날짜·ENUM·SET을 수로 읽기"),
    ("float", [], "FLOAT columns shown with six digits", "FLOAT 열 6자리 표시"),
    ("any_value", [], "ANY_VALUE under ONLY_FULL_GROUP_BY", "ONLY_FULL_GROUP_BY 아래의 ANY_VALUE"),
    ("view_order", [], "UPDATE / DELETE through a view with ORDER BY", "ORDER BY가 붙은 뷰 경유 UPDATE·DELETE"),
    ("windows", [], "Window frames: short form, named windows (WINDOW w AS (...))", "윈도 프레임 단축형·이름 있는 윈도 (WINDOW w AS (...))"),
    ("window_agg", [], "Window functions over groups and aggregates, inside expressions", "집계·그룹 위의 윈도, 식 안의 윈도"),
    ("max_execution_time", [], "MAX_EXECUTION_TIME: hint, variable, error 3024", "MAX_EXECUTION_TIME: 힌트·변수·오류 3024"),
    ("fk_names", [], "Foreign key names, DROP FOREIGN KEY, referential_constraints", "외래키 이름·DROP FOREIGN KEY·referential_constraints"),
    ("range_frames", [], "RANGE frames with an offset (numbers, INTERVAL), frames that hold no row", "RANGE 프레임 오프셋(수·INTERVAL), 행이 없는 프레임"),
    ("temporal_values", [], "Date functions and SUM / AVG of dates read as numbers", "날짜 함수와 날짜의 SUM·AVG를 수로 읽기"),
    ("float_single", [], "FLOAT columns as single precision in arithmetic and comparisons", "FLOAT 열을 단정밀도로 읽는 산술·비교"),
    ("target_table", [], "Error 1093: a subquery that reads the table an UPDATE / DELETE / INSERT writes", "오류 1093: 쓰는 표를 읽는 하위 질의"),
    ("having_alias", [], "HAVING reads a select-list alias before a column of the same name", "HAVING이 같은 이름의 열보다 select 별칭을 먼저 읽기"),
    ("self_join_write", [], "UPDATE / DELETE joining a table with itself, SET values naming a table by its alias", "자기 자신과 조인하는 UPDATE·DELETE, 별칭으로 다른 표를 읽는 SET 값"),
    ("subquery_item_order", [], "ORDER BY / DISTINCT on a scalar subquery of the select list", "select 목록 하위 질의의 ORDER BY·DISTINCT"),
    ("like_escape", [], "LIKE: the backslash as escape character, ESCAPE 'c' (also on a pattern from a column)", "LIKE: 역슬래시 이스케이프, ESCAPE 'c' (열에서 읽은 패턴도)"),
    ("fk_parent", [], "Parent side of a foreign key: DELETE / UPDATE of a parent row, NULL parent keys, cascades", "외래키의 부모 쪽: 부모 행 DELETE·UPDATE, NULL 부모 키, 연쇄"),
    ("self_fk", [], "A foreign key of a table to itself: row-by-row RESTRICT, CASCADE (14 levels), SET NULL, ON UPDATE as RESTRICT", "자기 표를 가리키는 외래키: 행 단위 RESTRICT·CASCADE(14단계)·SET NULL, ON UPDATE는 RESTRICT처럼"),
    ("multi_fk", [], "A column in several foreign keys: each key checks and acts on its own, listed under its own name", "한 열에 여러 외래키: 키마다 따로 검사하고 따로 동작하고 제 이름으로 나열됨"),
    ("float_copy", [], "FLOAT values given by MIN / MAX / GREATEST / COALESCE and copied into other columns (INSERT ... SELECT, UPDATE)", "MIN·MAX·GREATEST·COALESCE가 내는 FLOAT 값과 다른 열로의 복사(INSERT ... SELECT, UPDATE)"),
    ("replace_chain", [], "REPLACE below a chain of foreign keys: refused with nothing changed, or the whole chain goes", "외래키 사슬 아래의 REPLACE: 아무것도 바뀌지 않고 거절되거나 사슬 전체가 지워짐"),
    ("fk_chain", [], "Foreign keys over several tables: cascades down the whole chain, RESTRICT below, cycles, ON UPDATE CASCADE through a chain", "여러 표에 걸친 외래키: 사슬 끝까지의 연쇄, 아래쪽 RESTRICT, 순환, 사슬을 지나는 ON UPDATE CASCADE"),
    ("empty_columns", [], "The columns of an answer of no rows (select list, joins, derived tables, CTE, UNION, EXECUTE, SHOW TABLES LIKE)", "행이 없는 결과의 열 이름(select 목록·조인·파생 표·CTE·UNION·EXECUTE·SHOW TABLES LIKE)"),
    ("float_mixed", [], "FLOAT with a DOUBLE / integer column / DECIMAL in COALESCE, GREATEST ..., arithmetic over such a value, windows MIN / MAX / LAG / LEAD over a FLOAT", "FLOAT가 DOUBLE·정수 열·DECIMAL과 섞인 COALESCE·GREATEST ..., 그 값의 산술, FLOAT 열의 윈도 MIN·MAX·LAG·LEAD"),
    ("using_chain", [], "The order of the columns of * over a chain of USING / NATURAL joins (RIGHT JOIN too)", "USING·NATURAL 조인 사슬의 * 열 순서(RIGHT JOIN 포함)"),
    ("fk_auto_names", [], "Names of foreign keys without a name (declaration order, DROP / ADD / RENAME), NO ACTION vs RESTRICT in referential_constraints", "이름 없는 외래키의 자동 이름(선언 순서·DROP·ADD·RENAME), referential_constraints의 NO ACTION·RESTRICT"),
    ("transactions", [], "Statements inside a transaction: SAVEPOINT / ROLLBACK TO, DDL that commits, READ ONLY, the isolation variables, COMMIT AND CHAIN", "트랜잭션 안의 문장: SAVEPOINT·ROLLBACK TO·커밋하는 DDL·READ ONLY·격리 수준 변수·COMMIT AND CHAIN"),
    ("triggers_writes", [], "Triggers that write: SET NEW.x in BEFORE INSERT / UPDATE, a failing trigger takes the statement back, procedures that write", "쓰는 트리거: BEFORE INSERT·UPDATE의 SET NEW.x, 실패한 트리거는 문장째 되돌림, 쓰는 프로시저"),
    ("views_update", [], "Writes through views: renamed columns, a view over a view, WITH CHECK OPTION, the errors of what a view cannot take", "뷰를 통한 쓰기: 이름 바꾼 열·뷰 위의 뷰·WITH CHECK OPTION·받지 못하는 뷰의 오류"),
    ("fk_actions", [], "Foreign key actions, TRUNCATE of a referenced table, FOREIGN_KEY_CHECKS = 0, the definition of a key (1824 / 3780 / 1830)", "외래키 동작·참조되는 표의 TRUNCATE·FOREIGN_KEY_CHECKS = 0·키 정의 검사(1824·3780·1830)"),
    ("dates_arith", [], "Date and time arithmetic: TIME limit 838:59:59, STR_TO_DATE %j, CONVERT_TZ, strings that are no dates, a date against a date string", "날짜·시간 계산: TIME 한계 838:59:59·STR_TO_DATE %j·CONVERT_TZ·날짜가 아닌 글자·날짜와 날짜 글자의 비교"),
    ("json_functions", [], "JSON: how a document is written, paths (last, ranges, wildcards), SET / ARRAY_APPEND / MERGE_PATCH / SEARCH / OVERLAPS / MEMBER OF, JSON_VALUE RETURNING, JSON_ARRAYAGG, the order of JSON values, the errors", "JSON: 문서를 쓰는 모양·경로(last·범위·와일드카드)·SET·ARRAY_APPEND·MERGE_PATCH·SEARCH·OVERLAPS·MEMBER OF·JSON_VALUE RETURNING·JSON_ARRAYAGG·JSON 값의 순서·오류"),
    ("errno", ["--proto", "--errno"], "MySQL wire protocol: the error number of a failing statement", "MySQL 프로토콜: 실패한 문장의 오류 번호"),
    ("privileges", ["--proto", "--errno"], "Users and privileges: CREATE / ALTER / RENAME / DROP USER, GRANT / REVOKE on a table, a database, everything and columns, SHOW GRANTS, what a user may and may not do (1142 / 1143 / 1044 / 1227 / 1370), roles (SET ROLE, default roles, CURRENT_ROLE())", "사용자·권한: CREATE / ALTER / RENAME / DROP USER, 표·데이터베이스·전체·열에 대한 GRANT / REVOKE, SHOW GRANTS, 사용자가 할 수 있는 것과 없는 것(1142 / 1143 / 1044 / 1227 / 1370), 역할(SET ROLE · 기본 역할 · CURRENT_ROLE())"),
    ("text_number_in", [], "A text against a number in IN (subquery) and in CHECK: a number on either side makes it numbers, two texts compare as texts ('9' > '10'), also in ADD CHECK / UPDATE / REPLACE", "IN (하위 질의)와 CHECK에서 글자와 숫자의 비교: 한쪽이 숫자면 숫자로, 둘 다 글자면 글자로('9' > '10'), ADD CHECK·UPDATE·REPLACE에서도"),
    ("dual_order_limit", [], "ORDER BY / LIMIT / OFFSET after a SELECT without FROM, alone and at the end of UNION / INTERSECT / EXCEPT, in derived tables, subqueries and INSERT ... SELECT", "FROM 없는 SELECT 뒤의 ORDER BY·LIMIT·OFFSET: 단독·UNION / INTERSECT / EXCEPT의 끝·파생 표·하위 질의·INSERT ... SELECT"),
    ("create_database", ["--proto"], "CREATE DATABASE leaves the session in its database; CREATE TABLE ... AS SELECT reports the copied rows as affected rows (OK packet, ROW_COUNT())", "CREATE DATABASE는 세션을 옮기지 않음, CREATE TABLE ... AS SELECT는 복사한 행 수를 영향 행 수로 알림(OK 패킷·ROW_COUNT())"),
    ("join_view_writes", ["--proto", "--errno"], "UPDATE / INSERT through a view over a join: one base table, each row once, the WHERE of the view, a view over a join view, and the errors (1393 / 1394 / 1348 / 1288 / 1471 / 1395 / 1221)", "조인 뷰를 통한 UPDATE·INSERT: 한 기본 표, 행마다 한 번, 뷰의 WHERE, 조인 뷰 위의 뷰, 그리고 오류(1393 / 1394 / 1348 / 1288 / 1471 / 1395 / 1221)"),
    ("dual_paren_union", [], "FROM DUAL, and a bracketed UNION / INTERSECT / EXCEPT operand with an ORDER BY / LIMIT of its own", "FROM DUAL, 그리고 자기 ORDER BY·LIMIT를 가진 괄호 안 UNION / INTERSECT / EXCEPT 피연산자"),
    ("errno_more", ["--proto", "--errno"], "Error numbers of statements that failed for another check or with a number the engine did not tell: ONLY_FULL_GROUP_BY, window functions, RANGE frames, MOD, 1052 before 1093, DATETIME overflow, unsigned arithmetic, SELECT INTO a short variable, ALTER ... NOT NULL, ADD COLUMN ... NOT NULL, the order of foreign key checks", "다른 검사 때문에 실패하거나 엔진이 번호를 알리지 못하던 문장의 오류 번호: ONLY_FULL_GROUP_BY·윈도 함수·RANGE 프레임·MOD·1052가 1093보다 먼저·DATETIME 넘침·부호 없는 산술·짧은 변수로의 SELECT INTO·ALTER ... NOT NULL·ADD COLUMN ... NOT NULL·외래키 검사 순서"),
    ("wrong_answers_r82", ["--proto", "--errno"], "Hexadecimal and bit literals as the bytes they spell (and the integer where a number is asked), COMMENT of a column and a table, SELECT * over a LATERAL derived table, SHA2 of 224 / 384 / 512 bits, MEDIUMINT / TINYTEXT / BLOB sizes and CHAR(n)", "16진·비트 리터럴(그 바이트, 숫자를 물으면 정수), 열·표 COMMENT, LATERAL 파생 표의 SELECT *, SHA2 224 / 384 / 512, MEDIUMINT / TINYTEXT / BLOB 크기와 CHAR(n)"),
    ("tool_compat_r83", ["--proto", "--errno"], "What mysqldump, mysqlshow and schema tools send: LOCK TABLES ... READ LOCAL, SELECT /*!40001 SQL_NO_CACHE */, SHOW CREATE DATABASE / PROCEDURE / FUNCTION, information_schema routines / parameters / triggers / check_constraints / key_column_usage (UNIQUE) / views / collations, SHOW FULL TABLES / COLUMNS, a string as a column name, SELECT modifiers and STRAIGHT_JOIN, SET sql_mode = '' without ONLY_FULL_GROUP_BY", "mysqldump · mysqlshow · 스키마 도구가 보내는 문장: LOCK TABLES ... READ LOCAL, SELECT /*!40001 SQL_NO_CACHE */, SHOW CREATE DATABASE / PROCEDURE / FUNCTION, information_schema의 routines / parameters / triggers / check_constraints / key_column_usage(UNIQUE) / views / collations, SHOW FULL TABLES / COLUMNS, 열 이름이 되는 문자열, SELECT 수식어와 STRAIGHT_JOIN, ONLY_FULL_GROUP_BY 없는 SET sql_mode = ''"),
    ("routines_r84", ["--proto", "--errno"], "Who may see a routine and its text (EXECUTE / ALTER ROUTINE / CREATE ROUTINE / SHOW_ROUTINE / SELECT / the account that made it), SHOW CREATE PROCEDURE / FUNCTION / TRIGGER and information_schema routines, parameters and triggers per account, the account and time a routine is made by, a function name as it was written", "누가 루틴과 그 본문을 보는가(EXECUTE / ALTER ROUTINE / CREATE ROUTINE / SHOW_ROUTINE / SELECT / 만든 계정), 계정별 SHOW CREATE PROCEDURE / FUNCTION / TRIGGER와 information_schema의 routines · parameters · triggers, 루틴을 만든 계정과 시각, 쓴 그대로의 함수 이름"),
    ("views_updatable_r84", ["--proto", "--errno"], "information_schema.views IS_UPDATABLE for views of 29 shapes and the errors of a write a view cannot take (UPDATE / DELETE 1288, INSERT 1471, a column of an expression 1348, DELETE through a join view 1395)", "29가지 모양의 뷰의 IS_UPDATABLE과 뷰가 받지 못하는 쓰기의 오류(UPDATE · DELETE 1288, INSERT 1471, 표현식 열 1348, 조인 뷰의 DELETE 1395)"),
    ("view_meta_r85", ["--proto", "--errno"], "What the CREATE of a view wrote (DEFINER, SQL SECURITY, ALGORITHM = TEMPTABLE takes no write), the time a table or a view was made, a view over a derived table (YES, no write), a write that reads a column of a view that is an expression (k + 1 AS k1: the expression is put in its place), gtid_mode", "뷰의 CREATE가 쓴 것(DEFINER, SQL SECURITY, ALGORITHM = TEMPTABLE은 쓰기를 못 받음), 표 · 뷰를 만든 시각, 파생 표 위의 뷰(YES, 쓰기는 안 됨), 표현식 열(k + 1 AS k1)을 읽는 쓰기(식을 그 자리에 넣음), gtid_mode"),
    ("join_view_expr_r86", ["--proto", "--errno"], "A write through a view over a JOIN that reads a column of the view that is an expression (ja.x * 2 AS dbl, ja.x + jb.y AS s, a CASE): the expression is put in its place in the WHERE and the new value, also in a view over such a view", "JOIN 위의 뷰에서 표현식 열(ja.x * 2 AS dbl, ja.x + jb.y AS s, CASE)을 읽는 쓰기: WHERE · 새 값에 식을 그 자리에 넣음, 그런 뷰 위의 뷰도"),
    ("create_time_r86", ["--proto", "--errno"], "Which statements give a table a new CREATE_TIME: ALTER TABLE (a rename alone does not), CREATE INDEX, DROP INDEX, OPTIMIZE TABLE do; TRUNCATE, ANALYZE, a write and a failed statement do not; an index on a column the table has not (1072) and DROP INDEX of one that is not there (1091) fail", "어느 문장이 표의 CREATE_TIME을 새로 만드는가: ALTER TABLE(이름 바꾸기만은 아님) · CREATE INDEX · DROP INDEX · OPTIMIZE TABLE은 바꾸고, TRUNCATE · ANALYZE · 쓰기 · 실패한 문장은 안 바꿈; 없는 열의 인덱스(1072)와 없는 인덱스 DROP(1091)는 실패"),
    ("index_drop_column_r86", ["--proto", "--errno"], "ALTER TABLE ... DROP COLUMN takes the column out of the indexes that read it (an index with no column left goes with it) and CREATE TABLE ... LIKE copies what is left; CREATE INDEX on a column the table has not is 1072, DROP INDEX of an index that is not there 1091", "ALTER TABLE ... DROP COLUMN은 그 열을 읽는 인덱스에서 열을 빼고(열이 남지 않은 인덱스는 같이 사라짐) CREATE TABLE ... LIKE는 남은 것을 복사; 없는 열의 CREATE INDEX는 1072, 없는 인덱스의 DROP INDEX는 1091"),
    ("drop_column_constraints_r87", ["--proto", "--errno"], "ALTER TABLE ... DROP COLUMN and the keys that read the column: a UNIQUE key and a composite PRIMARY KEY lose it (a key with no column left goes), a shorter key that would repeat is 1062 and nothing is changed, a column an FK needs is 1828 / 1829, the only column of the table 1090", "ALTER TABLE ... DROP COLUMN과 그 열을 읽는 키: UNIQUE 키와 복합 PRIMARY KEY에서 열이 빠지고(열이 안 남은 키는 사라짐), 짧아진 키가 중복되면 1062로 거절하며 아무것도 안 바뀜, FK가 필요로 하는 열은 1828 · 1829, 표의 하나뿐인 열은 1090"),
    ("alter_copy_rows_r88", ["--proto", "--errno"], "The OK packet of ALTER TABLE says how many rows MySQL copied when it copies the table (a column whose type changes in a way the storage cannot take in place, ADD CHECK, ADD FOREIGN KEY) and 0 when it works in place (renames, nullability, a longer VARCHAR in the same class, indexes, ADD / DROP COLUMN); ROW_COUNT() says the same", "ALTER TABLE의 OK 패킷이 MySQL이 표를 복사하는 경우(저장소가 그 자리에서 못 바꾸는 열 형 변경, ADD CHECK, ADD FOREIGN KEY)에는 복사한 행 수를, 그 자리에서 하는 경우(이름 바꾸기, NULL 허용, 같은 길이 단계의 더 긴 VARCHAR, 인덱스, ADD / DROP COLUMN)에는 0을 말함; ROW_COUNT()도 같음"),
    ("show_databases_r89", ["--proto", "--errno"], "SHOW DATABASES | SCHEMAS LIKE 'pattern' | WHERE condition: the schemas that match, by name, the column called `Database (pattern)` for LIKE and `Database` for WHERE; what is not that statement is a syntax error", "SHOW DATABASES | SCHEMAS LIKE '패턴' | WHERE 조건: 맞는 스키마를 이름순으로, LIKE는 열 이름이 `Database (패턴)`, WHERE는 `Database`; 그 문장이 아닌 것은 문법 오류"),
    ("check_text_r89", ["--proto", "--errno"], "A CHECK condition the way MySQL writes it (information_schema.check_constraints, SHOW CREATE TABLE): columns back-quoted in the case typed, every comparison and AND / OR in brackets, lower case words, strings as _utf8mb4'..', a NOT pushed into the comparison, function names, CASE, regexp_like; 90 conditions", "MySQL이 쓰는 CHECK 조건 글자(information_schema.check_constraints, SHOW CREATE TABLE): 열은 쓴 대로 백틱, 비교와 AND / OR마다 괄호, 소문자 낱말, 글자는 _utf8mb4'..', NOT은 비교 안으로, 함수 이름, CASE, regexp_like; 조건 90가지"),
    ("call_rows_r89", ["--proto", "--errno"], "CALL: the OK packet and ROW_COUNT() are the rows of the last statement of the body that sets a row count (a write: its rows; SELECT ... INTO: 1; SET @x, DDL, a result set: 0; a local SET, DECLARE, IF / WHILE: unchanged)", "CALL: OK 패킷과 ROW_COUNT()는 본문에서 행 수를 정하는 마지막 문장의 행 수(쓰기: 그 행 수, SELECT ... INTO: 1, SET @x · DDL · 결과 표: 0, 지역 SET · DECLARE · IF / WHILE: 그대로)"),
    ("union_derived_r90", ["--proto", "--errno"], "A derived table, CTE or view over a UNION / INTERSECT / EXCEPT / WITH has the columns of its first query (an unknown column over it is 1054, the second query's own names are not visible), a UNION column of a number and a string is a string, the two queries of a set operation must give the same number of columns (1222, also in a derived table, CTE, subquery, INSERT ... SELECT and CREATE TABLE ... AS SELECT), FROM (WITH ...) is a derived table", "UNION / INTERSECT / EXCEPT / WITH 위의 파생 표·CTE·뷰는 첫 질의의 열을 가짐(그 위의 없는 열은 1054, 둘째 질의의 이름은 안 보임), 숫자와 글자의 UNION 열은 글자 열, 집합 연산의 두 질의는 열 개수가 같아야 함(1222; 파생 표·CTE·하위 질의·INSERT ... SELECT·CREATE TABLE ... AS SELECT에서도), FROM (WITH ...)은 파생 표"),
    ("check_text_r90", ["--proto", "--errno"], "NOT REGEXP, NOT RLIKE and REGEXP_LIKE() in a CHECK condition written the way MySQL writes it: (not(regexp_like(`name`,_utf8mb4'x'))), regexp_like(..) with a flags argument, in AND / OR and under NOT", "CHECK 조건의 NOT REGEXP · NOT RLIKE · REGEXP_LIKE()를 MySQL이 쓰는 꼴로: (not(regexp_like(`name`,_utf8mb4'x'))), 플래그 인수가 있는 regexp_like(..), AND / OR 안과 NOT 아래"),
    ("signal_r90", ["--proto", "--errno"], "SIGNAL SQLSTATE '..' [SET MESSAGE_TEXT, MYSQL_ERRNO] in a procedure and in BEFORE / AFTER triggers (1644, 1643, a number of its own, class 01 is a warning, the statement of several rows is undone), loop labels that close with END LOOP | WHILE | REPEAT label, INSERT INTO t () VALUES () and INSERT INTO t VALUES () (every column takes its default)", "프로시저와 BEFORE / AFTER 트리거의 SIGNAL SQLSTATE '..' [SET MESSAGE_TEXT, MYSQL_ERRNO](1644 · 1643 · 자기 번호 · 01 부류는 경고 · 여러 행 문장은 통째로 취소), END LOOP | WHILE | REPEAT 라벨로 닫는 루프 라벨, INSERT INTO t () VALUES ()와 INSERT INTO t VALUES ()(모든 열이 기본값)"),
    ("hints_r90", ["--proto", "--errno"], "Index hints after a table in FROM or UPDATE (USE | FORCE | IGNORE INDEX | KEY [FOR JOIN | ORDER BY | GROUP BY] (names)) are read and change nothing", "FROM · UPDATE의 표 뒤 인덱스 힌트(USE | FORCE | IGNORE INDEX | KEY [FOR JOIN | ORDER BY | GROUP BY] (이름))는 읽고 답을 바꾸지 않음"),
    ("avg_expr_r91", ["--proto", "--errno"], "AVG in an expression or a comparison goes on with the quotient MySQL holds (9 digits, cut off: AVG over 1, 2, 2 is 1.666666666) and shows the places of the argument + 4: AVG(v) * 3 is 5.0000, AVG(v) = 1.6667 is false, AVG(v) / 1 is 1.66666667; in HAVING, ORDER BY, CASE, IFNULL / COALESCE / IF and on DECIMAL arguments; AVG(x) OVER () stays rounded", "식·비교 안의 AVG는 MySQL이 들고 있는 몫(9자리, 자름: 1, 2, 2의 AVG는 1.666666666)으로 이어 계산하고 인수의 자릿수 + 4로 보임: AVG(v) * 3은 5.0000, AVG(v) = 1.6667은 거짓, AVG(v) / 1은 1.66666667; HAVING · ORDER BY · CASE · IFNULL / COALESCE / IF · DECIMAL 인수에서도; AVG(x) OVER ()는 둥글린 채"),
    ("wrong_answers_r96", ["--proto", "--exact", "--errno"], "A column a USING join made one is ambiguous again when a later table has it too (1052), a FLOAT that goes into a string function (CONCAT(f, '') is 3.14159), PI() with its six fixed places in arithmetic and comparisons, COALESCE / IFNULL / GREATEST / LEAST / UNION with the places of the widest side, ROUND / TRUNCATE that stop at the places the number shows, HEX of a number (the hex of the integer it rounds to) with BIN / OCT / CONV reading a text, SUM / AVG / MIN / MAX of PI() shown with its places and BETWEEN / IN against it, a number written with an exponent (1e0) is a DOUBLE", "USING 조인이 하나로 만든 열을 뒤의 표가 또 가지면 모호함(1052), 글자 함수에 들어간 FLOAT(CONCAT(f, '')는 3.14159), 산술 · 비교에서 PI()의 고정 소수 여섯 자리, 가장 넓은 쪽의 자릿수로 쓰는 COALESCE · IFNULL · GREATEST · LEAST · UNION, 보이는 자릿수에서 멈추는 ROUND · TRUNCATE, 정수로 반올림해 읽는 HEX(BIN · OCT · CONV는 글자로 읽음), 고정 자릿수로 보이는 PI()의 SUM · AVG · MIN · MAX와 그에 대한 BETWEEN · IN, 지수로 쓴 리터럴(1e0)은 DOUBLE"),
    ("having_names_r96c", ["--proto", "--exact", "--errno"], "The names HAVING and WHERE read: HAVING with no GROUP BY and no aggregate filters rows, HAVING reads the select list (columns, aliases, * and t.*) and the columns grouped by (any other name is 1054, even an ambiguous one; two select items of one name are 1052), an aggregate in HAVING reads the tables, and a select-list alias is no name of WHERE, ON or the select list (1054)", "HAVING · WHERE이 읽는 이름: GROUP BY도 집계도 없는 HAVING은 행을 거름, HAVING은 선택 목록(열 · 별칭 · * · t.*)과 묶는 열만 읽음(그 밖의 이름은 모호한 것도 1054, 같은 이름의 선택 항목이 둘이면 1052), HAVING 안의 집계는 표의 열을 읽음, 선택 목록의 별칭은 WHERE · ON · 선택 목록 자신의 이름이 아님(1054)"),
    ("grid_r97", ["--proto", "--exact", "--errno"], "Grids of functions x edge values: typed literals DATE '..' / TIME '..' / TIMESTAMP '..', <=> and BETWEEN of texts and numbers, GREATEST / LEAST ties, LOG with a base below 1, RADIANS / DEGREES, POW / EXP / COT out of range, CAST of a text with an exponent, REGEXP with an empty pattern, FIND_IN_SET, bit operators of texts, DIV / MOD of NULL", "함수 x 극단값 격자: 형 있는 리터럴 DATE '..' · TIME '..' · TIMESTAMP '..', 글자와 수의 <=> · BETWEEN, GREATEST · LEAST 동점, 밑이 1 미만인 LOG, RADIANS · DEGREES, 범위를 넘는 POW · EXP · COT, 지수가 든 글자의 CAST, 빈 패턴의 REGEXP, FIND_IN_SET, 글자의 비트 연산, NULL의 DIV · MOD"),
    ("system_schema_r86", ["--proto", "--errno"], "The system schema mysql: listed in information_schema.schemata for a MySQL-protocol session, never made or dropped (3552)", "시스템 스키마 mysql: MySQL 프로토콜 세션의 information_schema.schemata에 보이고, 만들거나 지울 수 없음(3552)"),
    ("rollup", [], "GROUP BY ... WITH ROLLUP: a row for each prefix of the keys and a total, HAVING / ORDER BY / LIMIT / DISTINCT after it, in a view and a derived table", "GROUP BY ... WITH ROLLUP: 키의 접두사마다 한 행과 총계, 그 뒤의 HAVING·ORDER BY·LIMIT·DISTINCT, 뷰·파생 표 안에서"),
    ("slow_log", [], "SET GLOBAL slow_query_log / long_query_time (values, errors, a number out of range), FOUND_ROWS() after an aggregate", "SET GLOBAL slow_query_log / long_query_time(값·오류·범위 밖의 수), 집계 뒤의 FOUND_ROWS()"),
    ("protocol", ["--proto"], "MySQL wire protocol: SHOW, OK packets, affected rows", "MySQL 프로토콜: SHOW·OK 패킷·영향 행 수"),
    ("protocol_types", ["--proto", "--types"], "MySQL wire protocol: typed result columns", "MySQL 프로토콜: 열 타입"),
    ("prepared_wire", [], "Prepared statements through the binary protocol (COM_STMT_*): binary rows, typed parameters, errors", "바이너리 프로토콜의 준비된 문장(COM_STMT_*): 바이너리 행·형 있는 매개변수·오류"),
]


def main():
    exe = os.path.abspath(sys.argv[1])
    tests = None
    if "--tests" in sys.argv:
        i = sys.argv.index("--tests")
        tests = {"cases": int(sys.argv[i + 1]), "assertions": int(sys.argv[i + 2].replace(",", ""))}
    rows = []
    for name, flags, en, ko in CORPORA:
        path = os.path.join(HERE, "corpus", name + ".sql" if name != "prepared_wire" else name + ".txt")
        if not os.path.exists(path):
            continue
        if name == "prepared_wire":  # (the binary protocol: prepared_wire.py runs it, with the same last line)
            path = os.path.join(HERE, "corpus", name + ".txt")
        script = "prepared_wire.py" if name == "prepared_wire" else "vs_mysql.py"
        r = subprocess.run([sys.executable, "-u", os.path.join(HERE, script), exe, path] + flags, capture_output=True, text=True, encoding="utf-8", errors="replace")
        last = (r.stdout.strip().splitlines() or [""])[-1]
        m = re.search(r"statements=(\d+) different=(\d+) \(both fail: (\d+)\)", last)
        if not m:
            raise SystemExit("no result line for %s:\n%s\n%s" % (name, r.stdout[-500:], r.stderr[-500:]))
        rows.append({"name": name, "en": en, "ko": ko, "statements": int(m.group(1)), "different": int(m.group(2)), "bothFail": int(m.group(3))})
        print("%-16s %s" % (name, last), flush=True)
    cases = 0
    for f in os.listdir(INC_DIR):
        if f.startswith("mysql_conformance_") and f.endswith(".inc"):
            cases += sum(1 for line in open(os.path.join(INC_DIR, f), encoding="utf-8") if line.startswith("    {"))
    data = {
        "generated": datetime.date.today().isoformat(),
        "mysql": "8.4.9",
        "corpora": rows,
        "conformanceCases": cases,
        "tests": tests,
        "bugsFound": 206,
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
