"""vs_mysql.py <rusql.exe> <corpus.sql> [--split] [--emit cases.inc] [--trace] [--proto [--types]] [--exact]

Runs the same statements on RuSQL and on a real MySQL (127.0.0.1:3391) and lists what differs.

Corpus: one statement per line; '#' comment; '--' prefix = setup run on both silently. Rows are compared as multisets unless the statement has
ORDER BY; numbers compare by value ('1' == '1.0000'); NULL is the text NULL. --split runs each item of a FROM-less `SELECT a, b, c` as a statement of its own.

--exact: the cells must be the same TEXT, not only the same number (30.7500 is not 30.75); with --emit the rows are written as kind 5, which the test compares so.
--emit writes the statements as the cases of a C++ conformance test (code/backend/tests/mysql_conformance_{functions,tables,constraints,collation}.inc, from corpus/{functions,tables,constraints,collation}.sql): MySQL's answer is the expected
one for every statement RuSQL answers the same; a statement RuSQL answers differently is only run (so the state of the tables stays the same).
"""
import datetime, decimal, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diff_builds as D
import pymysql

SETUP = [
    "CREATE TABLE t (id INT PRIMARY KEY, i INT, d DOUBLE, s VARCHAR(20), dt DATE, g INT)",
    "INSERT INTO t VALUES (1, 10, 1.5, 'apple', '2024-01-31', 1), (2, NULL, NULL, 'Apple', '2024-02-29', 1), (3, 30, 2.5, 'BANANA', NULL, 2),"
    " (4, 0, 0, 'banana', '2023-12-25', 2), (5, 7, 3.25, '10', '2024-03-05', 3), (6, -3, -1.5, '9', '2024-03-05', 3), (7, 7, 3.25, 'abc', '2020-02-29', 3),"
    " (8, NULL, 8.5, '', '2024-12-31', 4), (9, 100, 100, NULL, '2024-06-15', 4), (10, 12, 0.5, ' x ', '2024-06-16', 4)",
    "CREATE TABLE u (id INT PRIMARY KEY, k INT, name VARCHAR(10))",
    "INSERT INTO u VALUES (1, 10, 'one'), (2, 20, 'Two'), (3, NULL, 'three'), (4, 10, NULL)",
]

K_SETUP, K_ROWS, K_OK, K_ERR, K_RUN, K_EXACT = 0, 1, 2, 3, 4, 5  # K_EXACT: the rows, and the texts of the cells must be the same (--exact)


def cell(v, places=0):
    """A value of MySQL's answer as text; `places`: the digits of the fraction of a second the column has (a DATETIME(3) value is written with three)."""
    if v is None:
        return "NULL"
    if isinstance(v, (bytes, bytearray)):
        return v.decode("utf-8", "replace")
    if isinstance(v, datetime.datetime):
        return v.strftime("%Y-%m-%d %H:%M:%S") + ("." + "%06d" % v.microsecond)[:places + 1 if places else 0]
    if isinstance(v, datetime.timedelta):
        micro = abs(v.days * 86400 * 1000000 + v.seconds * 1000000 + v.microseconds)
        sign = "-" if v < datetime.timedelta(0) else ""
        total, micro = divmod(micro, 1000000)
        return "%s%02d:%02d:%02d%s" % (sign, total // 3600, total // 60 % 60, total % 60, ("." + "%06d" % micro)[:places + 1] if places else "")
    if isinstance(v, (datetime.date, datetime.time)):
        return str(v)
    if isinstance(v, bool):
        return "1" if v else "0"
    return str(v)


TYPE_CLASS = {1: "int", 2: "int", 3: "int", 8: "int", 9: "int", 13: "int", 0: "decimal", 246: "decimal", 4: "float", 5: "float", 10: "date", 7: "datetime", 12: "datetime", 11: "time"}
LAST_TYPES = {}  # id(cursor) -> the type classes of the columns of its last answer (--types)


def mysql_run(cur, sql):
    try:
        cur.execute(sql)
        if cur.description is None:
            LAST_TYPES.pop(id(cur), None)
            return ("ok", None)
        LAST_TYPES[id(cur)] = [TYPE_CLASS.get(d[1], "text") for d in cur.description]
        places = [d[5] if d[1] in (7, 11, 12) and d[5] else 0 for d in cur.description] # (the fraction of a second of a DATETIME / TIME / TIMESTAMP column)
        return ("rows", [[cell(v, places[i]) for i, v in enumerate(row)] for row in cur.fetchall()])
    except Exception as e:  # noqa
        return ("err", str(e)[:100])


def unescape_cell(cell: str) -> str:
    r"""결과 표의 셀 하나에서 이스케이프를 푼다: 엔진은 값의 역슬래시를 \\, 줄바꿈을 \n, 막대를 \|,
    값 양 끝의 공백/탭(패딩과 구별되지 않는)을 \s, \t 로 쓴다."""
    out, i = [], 0
    while i < len(cell):
        c = cell[i]
        if c == "\\" and i + 1 < len(cell) and cell[i + 1] in "\\|nst":
            out.append({"n": "\n", "s": " ", "t": "\t"}.get(cell[i + 1], cell[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def split_row(line: str) -> list[str]:
    r"""행 한 줄("| a   | b |")의 셀들. 이스케이프된 막대(\|)는 셀 경계가 아니고, 첫 막대 앞·마지막 막대 뒤 조각은 셀이 아니다."""
    pieces, cur, i = [], [], 0
    while i < len(line):
        c = line[i]
        if c == "\\" and i + 1 < len(line):
            cur.append(line[i:i + 2])
            i += 2
            continue
        if c == "|":
            pieces.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    pieces.append("".join(cur))
    return [unescape_cell(p.strip(" \t")) for p in pieces if p]


def rusql_run(db, sql):
    out = db.execute(sql).split("---END---")[0].strip().splitlines()
    if not out:
        return ("err", "?")
    if out[0] == "ERR":
        return ("err", out[1] if len(out) > 1 else "")
    rows, seen_header = [], False
    for line in out:
        if line.startswith("|"):
            cells = split_row(line.strip())
            if not seen_header:
                seen_header = True
                continue
            rows.append(cells)
    if seen_header:
        return ("rows", rows)
    return ("ok", None) if not any(l.startswith("|") for l in out) else ("rows", [])


def num(x):
    try:
        return decimal.Decimal(x)
    except Exception:  # noqa
        return None


EXACT = "--exact" in sys.argv  # the cells must be the same text (what a client sees: 30.75 is not 30.7500)


def same_cell(a, b):
    if a == b:
        return True
    if EXACT:
        return False
    na, nb = num(a), num(b)
    if na is not None and nb is not None:
        try:
            return abs(na - nb) <= decimal.Decimal("1e-9") * max(1, abs(na))
        except Exception:  # noqa
            return False
    return False


def first_difference(sql, a, b):
    """None when the two answers agree, else a short text naming the first row that differs."""
    empty_a = a[0] == "ok" or (a[0] == "rows" and not a[1])
    empty_b = b[0] == "ok" or (b[0] == "rows" and not b[1])
    if a[0] != b[0] and not (empty_a and empty_b):
        return "kind " + a[0] + " / " + b[0]
    if a[0] == "err" or a[0] == "ok" or (empty_a and empty_b):
        return None
    ra, rb = list(a[1]), list(b[1])
    if "ORDER BY" not in sql.upper():
        ra, rb = sorted(ra), sorted(rb)
    if len(ra) != len(rb):
        return "%d rows / %d rows" % (len(ra), len(rb))
    for k, (x, y) in enumerate(zip(ra, rb)):
        if len(x) != len(y) or not all(same_cell(p, q) for p, q in zip(x, y)):
            return "row %d: mysql %s / rusql %s" % (k, x, y)
    return None


def split_items(line):
    """`SELECT a, f(b, c), 'x,y'` -> ['SELECT a', 'SELECT f(b, c)', "SELECT 'x,y'"] (only for a SELECT without FROM)."""
    body = line[7:]
    items, depth, cur, quote = [], 0, [], None
    for ch in body:
        if quote:
            cur.append(ch)
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            items.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    items.append("".join(cur).strip())
    return ["SELECT " + i for i in items]


def c_escape(text, after_escape=False):
    """The text as the inside of a C string literal: UTF-8 bytes above 127 and control characters as three digit octal escapes (and a digit after one as one too)."""
    out = []
    escaped = after_escape  # (True: the text follows an octal escape, so a digit at its start is written as one too)
    for byte in text.encode("utf-8"):
        ch = chr(byte)
        if ch == '"':
            out.append('\\"')
            escaped = False
        elif ch == "\\":
            out.append("\\\\")
            escaped = False
        elif ch == "\n":
            out.append("\\n")
            escaped = False
        elif ch == "\r":
            out.append("\\r")
            escaped = False
        elif ch == "\t":
            out.append("\\t")
            escaped = False
        elif 0x20 <= byte < 0x7F and not (escaped and ch.isdigit()):
            out.append(ch)
            escaped = False
        else:
            out.append("\\%03o" % byte)
            escaped = True
    return "".join(out)


def encode_rows(rows):
    # each row ends with the C escape 036, its cells are separated by the C escape 037 (octal escapes cannot run into a digit that follows)
    return "".join("\\037".join(c_escape(x, True) for x in r) + "\\036" for r in rows)


def case(kind, sql, rows=""):
    return '    {%d, "%s", "%s"},\n' % (kind, c_escape(sql), rows)


def main():
    args = sys.argv[1:]
    exe, corpus = args[0], args[1]
    emit_path = args[args.index("--emit") + 1] if "--emit" in args else None
    show = int(args[args.index("--show") + 1]) if "--show" in args else 10 ** 9
    emitted = []
    proto = "--proto" in args
    if proto:
        import shutil, socket, subprocess, time
        data = os.path.join(os.environ.get("TEMP", "/tmp"), "vs_mysql_rusql")
        shutil.rmtree(data, ignore_errors=True)
        env = dict(os.environ, RUSTDB_PARALLEL="0")
        proc = subprocess.Popen([exe, "--port", "17991", "--mysql-port", "13399", "--data-dir", data], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        for _ in range(400):
            try:
                socket.create_connection(("127.0.0.1", 13399), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        rconn = pymysql.connect(host="127.0.0.1", port=13399, user="root", password="root", autocommit=True, charset="utf8mb4")
        rcur = rconn.cursor()

        class ProtoDb:
            def execute(self, sql):
                return rcur.execute(sql)
        db = ProtoDb()
    else:
        proc, db = D.start(exe, 17990, os.path.join(os.environ.get("TEMP", "/tmp"), "vs_mysql_rusql"), False)
    conn = pymysql.connect(host="127.0.0.1", port=3391, user="root", password="", autocommit=True, charset="utf8mb4")
    cur = conn.cursor()
    try:
        cur.execute("DROP DATABASE IF EXISTS v")
        cur.execute("CREATE DATABASE v")
        cur.execute("USE v")
        db.execute("CREATE DATABASE IF NOT EXISTS v")
        db.execute("USE v")
        for s in SETUP:
            cur.execute(s)
            db.execute(s)
        total = diff = both_err = 0
        shown = 0
        for line in open(corpus, encoding="utf-8").read().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("--"):
                stmt = line[2:].strip()
                mysql_run(cur, stmt)
                (mysql_run(rcur, stmt) if proto else rusql_run(db, stmt))
                emitted.append(case(K_SETUP, stmt))
                continue
            items = split_items(line) if line.upper().startswith("SELECT ") and " FROM " not in line.upper() and "--split" in args else [line]
            for stmt in items:
                total += 1
                if "--trace" in args:
                    print("RUN: " + stmt, file=sys.stderr, flush=True)
                m, r = mysql_run(cur, stmt), (mysql_run(rcur, stmt) if proto else rusql_run(db, stmt))
                if m[0] == "err" and r[0] == "err":
                    both_err += 1
                why = first_difference(stmt, m, r)
                if why is None and proto and "--types" in args and m[0] == "rows" and r[0] == "rows" and LAST_TYPES.get(id(cur)) != LAST_TYPES.get(id(rcur)):
                    why = "column types mysql=%s / rusql=%s" % (LAST_TYPES.get(id(cur)), LAST_TYPES.get(id(rcur)))
                if why is not None:
                    diff += 1
                    if shown < show:
                        shown += 1
                        print("DIFF: " + stmt)
                        print("   " + str(why) + (" | mysql=" + str(m[1]) if m[0] == "err" else "") + (" | rusql=" + str(r[1]) if r[0] == "err" else ""))
                    emitted.append(case(K_RUN, stmt))
                elif m[0] == "rows":
                    emitted.append(case(K_EXACT if EXACT else K_ROWS, stmt, encode_rows(m[1]) if m[1] else ""))
                elif m[0] == "ok":
                    emitted.append(case(K_OK, stmt))
                else:
                    emitted.append(case(K_ERR, stmt))
        print("statements=%d different=%d (both fail: %d)" % (total, diff, both_err))
        if emit_path:
            with open(emit_path, "w", encoding="utf-8", newline="\n") as f:
                f.write("// generated by code/test/diff/vs_mysql.py --emit from MySQL 8.4's answers: {kind, statement, expected rows}\n")
                f.write("".join(emitted))
    finally:
        proc.kill()
        conn.close()


main()
