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
    ("protocol", ["--proto"], "MySQL wire protocol: SHOW, OK packets, affected rows", "MySQL 프로토콜: SHOW·OK 패킷·영향 행 수"),
    ("protocol_types", ["--proto", "--types"], "MySQL wire protocol: typed result columns", "MySQL 프로토콜: 열 타입"),
]


def main():
    exe = os.path.abspath(sys.argv[1])
    tests = None
    if "--tests" in sys.argv:
        i = sys.argv.index("--tests")
        tests = {"cases": int(sys.argv[i + 1]), "assertions": int(sys.argv[i + 2].replace(",", ""))}
    rows = []
    for name, flags, en, ko in CORPORA:
        path = os.path.join(HERE, "corpus", name + ".sql")
        if not os.path.exists(path):
            continue
        r = subprocess.run([sys.executable, "-u", os.path.join(HERE, "vs_mysql.py"), exe, path] + flags, capture_output=True, text=True, encoding="utf-8", errors="replace")
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
        "bugsFound": 194,
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
