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
