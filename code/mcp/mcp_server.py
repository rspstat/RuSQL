"""
RuSQL True MCP Server
stdio transport — Claude Desktop spawns this process and communicates via JSON-RPC.

Claude Desktop config:
  %APPDATA%\\Claude\\claude_desktop_config.json
  → see claude_desktop_config_example.json
"""
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

from mcp.server.fastmcp import FastMCP

# 접속정보는 Claude Desktop 설정의 "env" 필드로 오버라이드 가능 (RuSQL UI의 "Auto-connect
# Claude Desktop" 버튼이 지금 이 세션의 실제 host/port/계정으로 채워 씀 -- setup_mcp_config,
# code/frontend/src-tauri/src/main.rs). env가 없으면(수동 설정 등) 기존 기본값으로 폴백.
RUSQL_HOST = os.environ.get("RUSQL_HOST", "127.0.0.1")
RUSQL_PORT = int(os.environ.get("RUSQL_PORT", "7878"))
RUSQL_USER = os.environ.get("RUSQL_USER", "root")
RUSQL_PASS = os.environ.get("RUSQL_PASS", "root")
# launch_app이 앱이 안 켜져 있을 때 무엇을 실행할지 — 위와 동일하게 setup_mcp_config가 씀.
# 없으면(수동 설정 등) launch_app은 그냥 실패 메시지를 반환.
RUSQL_APP_PATH = os.environ.get("RUSQL_APP_PATH", "")

# RuSQL 서버가 막 (재)시작돼 리스닝 소켓이 아직 안 열려 있는 짧은 순간의 접속 실패를
# 흡수하기 위한 재시도 -- 이전엔 ConnectionRefusedError가 한 번이라도 나면 그 도구 호출
# 전체가 바로 실패했음.
_CONNECT_RETRIES = 3
_CONNECT_RETRY_DELAY_SEC = 0.5

mcp = FastMCP("RuSQL v2.3.0")


def _compute_native_password_token(password: str, nonce: bytes) -> bytes:
    """mysql_native_password-style challenge-response, matching engine_client's
    compute_native_password_token (client/src/main.cpp): the native protocol no longer
    accepts a plaintext password (Phase 19 security fix), only this token."""
    stage1 = hashlib.sha1(password.encode()).digest()
    stage2 = hashlib.sha1(stage1).digest()
    xor_key = hashlib.sha1(nonce + stage2).digest()
    return bytes(a ^ b for a, b in zip(stage1, xor_key))


class _Conn:
    def __init__(self):
        last_err = None
        for attempt in range(_CONNECT_RETRIES):
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                self.sock.connect((RUSQL_HOST, RUSQL_PORT))
                last_err = None
                break
            except OSError as e:
                last_err = e
                self.sock.close()
                if attempt < _CONNECT_RETRIES - 1:
                    time.sleep(_CONNECT_RETRY_DELAY_SEC)
        if last_err is not None:
            raise last_err
        self.sock.settimeout(30)
        banner = self._recv()  # welcome banner, includes a "NONCE <hex>" line
        nonce_hex = next((l.split(" ", 1)[1] for l in banner.splitlines() if l.startswith("NONCE ")), None)
        if not nonce_hex:
            raise RuntimeError("Server did not send an auth challenge (NONCE)")
        token = _compute_native_password_token(RUSQL_PASS, bytes.fromhex(nonce_hex))
        self._raw(f"AUTH {RUSQL_USER} {token.hex()}")

    def _raw(self, msg: str) -> str:
        self.sock.sendall((msg + "\n").encode())
        return self._recv()

    def _recv(self) -> str:
        buf = ""
        while True:
            buf += self.sock.recv(4096).decode(errors="replace")
            if "---END---" in buf:
                return buf.replace("---END---", "").strip()

    def exec(self, sql: str, db: str = "") -> str:
        if db:
            self._raw(f"USE {db};")
        q = sql.strip()
        if not q.endswith(";"):
            q += ";"
        return self._raw(q)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def _run(sql: str, db: str = "") -> str:
    print(f"[mcp] _run called: db={db!r} sql={sql[:60]!r}", file=sys.stderr, flush=True)
    try:
        c = _Conn()
        try:
            result = c.exec(sql, db) or "(empty result)"
            print(f"[mcp] result: {result[:80]!r}", file=sys.stderr, flush=True)
            return result
        finally:
            c.close()
    except ConnectionRefusedError:
        msg = f"Error: RuSQL is not running on {RUSQL_HOST}:{RUSQL_PORT}. Start the server first."
        print(f"[mcp] {msg}", file=sys.stderr, flush=True)
        return msg
    except Exception as e:
        msg = f"Error: {e}"
        print(f"[mcp] {msg}", file=sys.stderr, flush=True)
        return msg


def _unescape_cell(cell: str) -> str:
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


def _split_row(line: str) -> list[str]:
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
    return [_unescape_cell(p.strip(" \t")) for p in pieces if p]


def _parse_table_output(text: str) -> list[dict]:
    """RuSQL native 프로토콜 출력을 JSON 배열로 변환. 두 형식을 지원:
    박스 그림 표(SHOW DATABASES/TABLES, SELECT 등 — "+---+"/"| a | b |") 및 탭 구분
    (SHOW INDEX 등). 응답은 항상 "OK"/"ERR" 상태 줄로 시작하고 "(N.NNN sec)" 타이밍
    줄(과 종종 "N row(s) returned." 요약 줄)로 끝나므로, 실제 표 내용을 보기 전에
    이 앞뒤 줄들을 먼저 걷어낸다.

    Regression: 이전엔 첫 번째 줄(항상 "OK")을 헤더 행으로 착각해 탭 검사를 해서
    모든 응답이 파싱 실패로 처리되고 있었음(list_databases/list_tables/sample_data/
    get_indexes가 독스트링과 달리 실제 행 배열이 아니라 원본 텍스트를 그대로
    감싸서 반환하던 문제)."""
    lines = [l for l in text.splitlines() if l.strip()]
    if not lines:
        return []
    if lines[0].strip().upper().startswith("ERR"):
        return [{"error": text}]

    body = lines[1:] if lines[0].strip().upper() == "OK" else lines
    while body and (
        (body[-1].startswith("(") and body[-1].endswith(")"))
        or re.match(r"^\d+ row", body[-1])
    ):
        body.pop()
    if not body:
        return [{"result": text}]

    # 박스 그림 표: "+---+" 테두리로 둘러싸인 "| a | b |" 행들
    if body[0].startswith("+") and body[0].endswith("+"):
        content_lines = [l for l in body if l.startswith("|") and l.endswith("|")]
        if not content_lines:
            return [{"result": text}]
        headers = _split_row(content_lines[0])
        rows = []
        for line in content_lines[1:]:
            cells = _split_row(line)
            if len(cells) == len(headers):
                rows.append(dict(zip(headers, cells)))
        return rows

    # 탭 구분 표 (예: SHOW INDEX)
    header_line = body[0]
    if "\t" not in header_line:
        return [{"result": text}]
    headers = [h.strip() for h in header_line.split("\t")]
    rows = []
    for line in body[1:]:
        parts = [p.strip() for p in line.split("\t")]
        if len(parts) == len(headers):
            rows.append(dict(zip(headers, parts)))
    return rows


# ─── 위험한 SQL 안전장치 ─────────────────────────────────────────
# 완전한 SQL 파서가 아니라 휴리스틱 정규식 — 과탐(안전한 걸 위험하다고 잘못 판단)은
# 괜찮지만 미탐(위험한 걸 놓치는 것)은 피하는 쪽으로 설계. WHERE가 문자열 리터럴 안에
# 있어도 "있다"고 인식해 안전하다고 판단하는 정도의 오차는 감수한다.
_DANGEROUS_STATEMENT_RE = re.compile(r"^\s*(DROP|TRUNCATE)\b", re.IGNORECASE)
_DANGEROUS_NO_WHERE_RE = re.compile(r"^\s*(UPDATE|DELETE)\b(?![\s\S]*\bWHERE\b)", re.IGNORECASE)


def _dangerous_sql_reason(sql: str) -> str | None:
    s = sql.strip().rstrip(";")
    if not s:
        return None
    m = _DANGEROUS_STATEMENT_RE.match(s)
    if m:
        return f"{m.group(1).upper()} is irreversible and affects an entire table/database."
    m = _DANGEROUS_NO_WHERE_RE.match(s)
    if m:
        return f"{m.group(1).upper()} without a WHERE clause would affect every row in the table."
    return None


def _exec_and_format(sql: str, database: str = "") -> str:
    raw = _run(sql, database)
    # SELECT 계열 결과는 JSON 배열로 변환. 성공 응답은 항상 "OK"로 시작하므로
    # (과거엔 이 접두어 때문에 파싱 자체가 항상 스킵됐음), ERR이 아니면 일단
    # 파싱을 시도하고 실제로 표 형태로 파싱됐을 때만 JSON을 반환한다 — CREATE/
    # INSERT 같은 단순 상태 메시지는 표로 파싱 안 되니 원본 텍스트 그대로 반환.
    stripped = raw.strip()
    if not stripped.upper().startswith("ERR"):
        rows = _parse_table_output(stripped)
        if rows and not (len(rows) == 1 and ("result" in rows[0] or "error" in rows[0])):
            return json.dumps(rows, ensure_ascii=False)
    return raw


@mcp.tool()
def execute_sql(sql: str, database: str = "") -> str:
    """Execute any SQL query on RuSQL. Returns a JSON array of row objects for SELECT,
    or a plain status message for DDL/DML. Optionally specify a database to USE before executing.

    RuSQL is a MySQL-compatible custom engine with broad feature support, including
    AUTO_INCREMENT, ENUM, TINYINT/SMALLINT, BOOLEAN, CHECK constraints, FOREIGN KEY
    constraints, date functions (CURDATE/NOW/DATEDIFF/DATE_ADD/DATE_SUB/...), IF(cond, a, b),
    EXISTS/NOT EXISTS subqueries, and multi-table UPDATE/DELETE (`UPDATE t1, t2 SET ...`,
    `DELETE t1, t2 FROM t1 JOIN t2 ON ...`). See docs/mds/FUNCTIONS.md in the repo for the
    full feature list.

    DROP/TRUNCATE and UPDATE/DELETE without a WHERE clause are refused here - ask the user
    to explicitly confirm, then call confirm_dangerous_sql with the exact same SQL."""
    reason = _dangerous_sql_reason(sql)
    if reason:
        return (f"Refused: {reason} Ask the user to explicitly confirm this is intended, "
                f"then call confirm_dangerous_sql with the exact same SQL to run it.")
    return _exec_and_format(sql, database)


@mcp.tool()
def confirm_dangerous_sql(sql: str, database: str = "", via_editor: bool = False, tab: str = "") -> str:
    """Execute a SQL statement that execute_sql or execute_in_editor refused as dangerous
    (DROP/TRUNCATE, or UPDATE/DELETE with no WHERE clause). Only call this after the user
    has explicitly confirmed in this conversation that they want to proceed - never on your
    own judgment, and never pre-emptively before execute_sql/execute_in_editor has actually
    refused. Set via_editor=True to run it visibly in the RuSQL UI (like execute_in_editor,
    optionally in a specific `tab`) instead of invisibly against the engine (like execute_sql)."""
    if via_editor:
        params = {"query": sql}
        if tab:
            params["tab"] = tab
        return _send_ui_command("execute_in_editor", params)
    return _exec_and_format(sql, database)


# run_script: DDL, transaction control and locking commit (or end) the open transaction on the engine, as in MySQL, so a preview that ends with ROLLBACK cannot undo them.
_IMPLICIT_COMMIT_RE = re.compile(
    r"^\s*(CREATE|ALTER|DROP|TRUNCATE|RENAME|BEGIN|START|COMMIT|ROLLBACK|SAVEPOINT|RELEASE|LOCK|UNLOCK|SET\s+(?:SESSION\s+)?AUTOCOMMIT)\b", re.IGNORECASE)
_SCRIPT_ROW_LIMIT = 20  # rows of a SELECT shown per statement of a script


def _script_step(n: int, sql: str, raw: str) -> dict:
    """One statement of a script: whether it worked, and what it answered (the rows of a SELECT, or the status line without its timing)."""
    text = raw.strip()
    ok = not text.upper().startswith("ERR")
    body = [l for l in text.splitlines() if l.strip()]
    if body and body[0].strip().upper() in ("OK", "ERR"):
        body = body[1:]
    if body and re.match(r"^\(\d+(\.\d+)? sec\)$", body[-1].strip()):
        body.pop()
    step = {"n": n, "sql": sql, "ok": ok}
    if ok:
        rows = _parse_table_output(text)
        if rows and not (len(rows) == 1 and ("result" in rows[0] or "error" in rows[0])):
            step["rows"] = rows[:_SCRIPT_ROW_LIMIT]
            if len(rows) > _SCRIPT_ROW_LIMIT:
                step["rows_not_shown"] = len(rows) - _SCRIPT_ROW_LIMIT
            return step
        step["result"] = " ".join(body)
    else:
        step["error"] = " ".join(body) or text
    return step


@mcp.tool()
def run_script(statements: list[str], database: str = "", commit: bool = False, confirmed_dangerous: bool = False) -> str:
    """Run several SQL statements in order on ONE connection, inside one transaction, and report what each did
    (rows of a SELECT, "N row(s) updated", or the error). Use it for a change that has to happen as a whole.

    Two modes:
    - commit=False (the default) is a PREVIEW: the statements run for real inside a transaction, the answer shows what
      each one would do (including how many rows it touches), and then everything is rolled back - nothing is saved.
      Show the user the preview, and call again with commit=True once they agree.
    - commit=True runs the script and saves it, all or nothing: the first statement that fails rolls everything back
      and the statements after it are not run.

    A preview needs statements that a transaction can undo (INSERT/UPDATE/DELETE/SELECT). DDL (CREATE/ALTER/DROP/
    TRUNCATE/RENAME) and BEGIN/COMMIT/ROLLBACK end the transaction on the engine as in MySQL, so a script that holds
    one is refused as a preview; run it with commit=True after the user agreed. DROP/TRUNCATE and UPDATE/DELETE
    without a WHERE clause are refused as in execute_sql: ask the user, then pass confirmed_dangerous=True.
    Returns JSON: {mode, committed, failed_at, results: [...]}."""
    if not statements:
        return "Refused: statements is empty."
    statements = [s.strip().rstrip(";").strip() for s in statements]
    statements = [s for s in statements if s]
    if not commit:
        for i, sql in enumerate(statements, 1):
            if _IMPLICIT_COMMIT_RE.match(sql):
                return (f"Refused: statement {i} ({sql[:60]!r}) commits or ends the transaction on the engine, so a preview could not undo it. "
                        f"Run the script with commit=True once the user agreed, or preview only its INSERT/UPDATE/DELETE/SELECT statements.")
    if not confirmed_dangerous:
        for i, sql in enumerate(statements, 1):
            reason = _dangerous_sql_reason(sql)
            if reason:
                return (f"Refused: statement {i} ({sql[:60]!r}): {reason} Ask the user to explicitly confirm this is intended, "
                        f"then call run_script again with confirmed_dangerous=True.")
    mode = "commit" if commit else "preview"
    results: list[dict] = []
    failed_at = None
    committed = False
    try:
        c = _Conn()
        try:
            if database:
                used = c._raw(f"USE {database};")
                if used.strip().upper().startswith("ERR"):
                    return f"Error: {used.strip()}"
            began = c._raw("BEGIN;")
            if began.strip().upper().startswith("ERR"):
                return f"Error: could not start a transaction: {began.strip()}"
            for n, sql in enumerate(statements, 1):
                step = _script_step(n, sql, c._raw(sql + ";"))
                results.append(step)
                if not step["ok"]:
                    failed_at = n
                    break
            if failed_at is None and commit:
                done = c._raw("COMMIT;")
                committed = not done.strip().upper().startswith("ERR")
                if not committed:
                    results.append({"n": len(statements) + 1, "sql": "COMMIT", "ok": False, "error": done.strip()})
                    failed_at = len(statements) + 1
            if not committed:
                c._raw("ROLLBACK;")
        finally:
            c.close()
    except ConnectionRefusedError:
        return f"Error: RuSQL is not running on {RUSQL_HOST}:{RUSQL_PORT}. Start the server first."
    except Exception as e:
        return f"Error: {e}"
    out = {"mode": mode, "committed": committed, "failed_at": failed_at, "results": results}
    if failed_at is not None:
        out["note"] = "The statement above failed, so the whole script was rolled back and nothing was saved."
    elif not commit:
        out["note"] = "PREVIEW: everything was rolled back, nothing was saved. Call again with commit=True to apply it."
    return json.dumps(out, ensure_ascii=False)


@mcp.tool()
def list_databases() -> str:
    """List all databases available in RuSQL. Returns a JSON array."""
    raw = _run("SHOW DATABASES")
    rows = _parse_table_output(raw)
    return json.dumps(rows, ensure_ascii=False)


@mcp.tool()
def list_tables(database: str = "") -> str:
    """List all tables in the specified database. Returns a JSON array."""
    raw = _run("SHOW TABLES", database)
    rows = _parse_table_output(raw)
    return json.dumps(rows, ensure_ascii=False)


@mcp.tool()
def get_table_schema(table: str, database: str = "") -> str:
    """Get the CREATE TABLE DDL for a specific table."""
    return _run(f"SHOW CREATE TABLE {table}", database)


@mcp.tool()
def explain_query(sql: str, database: str = "") -> str:
    """Run EXPLAIN ANALYZE on a query and return structured execution plan info.
    Useful for diagnosing slow queries, checking index usage, and estimating row counts.
    A SELECT is run (EXPLAIN ANALYZE: real rows and time per step). An UPDATE or DELETE is NOT run: it gets the plan the engine estimates (EXPLAIN: access path and cost)."""
    explain_sql = sql.strip().rstrip(";")
    body = re.sub(r"^EXPLAIN\s+(?:ANALYZE\s+)?", "", explain_sql, flags=re.IGNORECASE)
    if _is_write(body):
        # EXPLAIN ANALYZE runs the statement: an UPDATE or DELETE is only planned (the engine's estimate: access path, cost), nothing is changed
        return _run(f"EXPLAIN {body}", database)
    if not re.match(r"\(?\s*(SELECT|WITH)\b", body, flags=re.IGNORECASE):
        return "Refused: explain_query reads a SELECT, UPDATE or DELETE statement."
    if not explain_sql.upper().startswith("EXPLAIN"):
        explain_sql = f"EXPLAIN ANALYZE {explain_sql}"
    return _run(explain_sql, database)


@mcp.tool()
def get_indexes(table: str, database: str = "") -> str:
    """Return all indexes defined on a table as a JSON array.
    Includes index name, type (BTREE/HASH), columns, and whether it is unique."""
    raw = _run(f"SHOW INDEX FROM {table}", database)
    rows = _parse_table_output(raw)
    return json.dumps(rows, ensure_ascii=False)


# ─── suggest_indexes ────────────────────────────────────────────────
# The reading of the statement is the engine's: `SUGGEST INDEX <statement>` (executor_advice.cpp) reads the parsed statement -- the main query, each CTE, derived table and subquery
# (IN, EXISTS, scalar, also a correlated one), the branches of a UNION, an OR branch by branch -- the columns and indexes of its tables and how many different values each column
# holds, and answers with one row for each suggestion, existing index and note. The desktop client shows the same answer under a plan, so the two cannot disagree. This tool asks
# the engine and adds what the engine does not say: the facts of the tables, the plan of the statement (EXPLAIN ANALYZE for a SELECT; for an UPDATE or DELETE EXPLAIN, which runs
# nothing) and, with verify=True, a trial of the suggestions (a temporary index, the plan before and after, the index dropped again). By default it creates nothing.
_CLAUSE_END = r"(?=\bWHERE\b|\bGROUP\s+BY\b|\bORDER\s+BY\b|\bHAVING\b|\bLIMIT\b|\bUNION\b|\bFOR\s+UPDATE\b|\bWINDOW\b|$)"
_VERIFY_MAX_ROWS = 200000     # verify=True builds an index: not on a table bigger than this
_VERIFY_MAX_INDEXES = 3       # ... and tries at most this many
_VERIFY_RUNS = 3              # EXPLAIN ANALYZE runs per measurement (the median is kept)


def _strip_sql(sql: str) -> str:
    """The statement without comments, strings and extra blanks (to tell its kind and to read the one shape the rewrite of an OR is made for)."""
    s = re.sub(r"/\*.*?\*/", " ", sql, flags=re.S)
    s = re.sub(r"--[^\n]*", " ", s)
    s = re.sub(r"'(?:[^']|'')*'", "''", s)
    s = re.sub(r'"(?:[^"]|"")*"', '""', s)
    return re.sub(r"\s+", " ", s).strip().rstrip(";").strip()


def _clause(stripped: str, keyword: str) -> str:
    m = re.search(rf"\b{keyword}\b\s+(.+?){_CLAUSE_END}", stripped, flags=re.IGNORECASE)
    return m.group(1).strip() if m else ""


def _matching_paren(text: str, start: int) -> int:
    """The place of the parenthesis that closes the one at `start` (-1: none)."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def _split_top_level(clause: str, word: str) -> list[str]:
    """The pieces of a WHERE / ON split at the AND (or OR) that is not inside parentheses (an AND inside BETWEEN is not a split)."""
    text = re.sub(r"(\bBETWEEN\s+\S+)\s+AND\s+", r"\1 __AND__ ", clause, flags=re.IGNORECASE)
    parts, depth, cur = [], 0, ""
    for token in re.split(rf"(\s+{word}\s+|\(|\))", text, flags=re.IGNORECASE):
        if token == "(":
            depth += 1
        elif token == ")":
            depth -= 1
        if depth == 0 and re.fullmatch(rf"\s+{word}\s+", token, flags=re.IGNORECASE):
            parts.append(cur.strip())
            cur = ""
        else:
            cur += token
    parts.append(cur.strip())
    return [p.replace("__AND__", "AND") for p in parts if p]


def _unparen(t: str) -> str:
    t = t.strip()
    while t.startswith("(") and t.endswith(")") and t.count("(") == t.count(")") and _matching_paren(t, 0) == len(t) - 1:
        t = t[1:-1].strip()
    return t


def _or_rewrite(sql: str) -> dict | None:
    """The UNION of the branches of an OR, for a plain SELECT over one table whose only test is that OR (each branch can then use an index of its own, as a statement of its own);
    nothing for any other shape. The text of the statement is cut, so it must not hold a string that has ` or ` in it."""
    text = re.sub(r"\s+", " ", re.sub(r"/\*.*?\*/|--[^\n]*", " ", sql, flags=re.S)).strip().rstrip(";").strip()
    stripped = _strip_sql(sql)
    if not stripped.upper().startswith("SELECT") or re.search(r"\(\s*SELECT\b|\bJOIN\b|\bUNION\b", stripped, flags=re.IGNORECASE):
        return None
    if any(_clause(stripped, k) for k in ("ORDER BY", "GROUP BY", "LIMIT", "HAVING")):
        return None
    if re.search(r"'[^']*\bor\b[^']*'", text, flags=re.IGNORECASE):
        return None
    m = re.match(r"^(SELECT .+? FROM \S+(?: (?:AS )?\w+)?) WHERE (.+)$", text, flags=re.IGNORECASE)
    if not m or "," in m.group(1).split(" FROM ", 1)[-1]:
        return None
    branches = _split_top_level(_unparen(m.group(2)), "OR")
    if len(branches) < 2 or len(_split_top_level(_unparen(m.group(2)), "AND")) != 1:
        return None
    return {"why": "each branch of the OR can use its own index as a statement of its own; UNION also removes rows that are the same in every selected column, which OR keeps",
            "sql": " UNION ".join(f"{m.group(1)} WHERE {b}" for b in branches) + ";"}


def _table_facts(table: str, database: str) -> dict:
    """Columns, indexes (each a list of columns, PRIMARY too), the row count and the distinct values per column of a table."""
    facts: dict = {"columns": [], "indexes": {}, "rows": None, "distinct": {}}
    where = f"TABLE_NAME = '{table}' AND TABLE_SCHEMA = DATABASE()"
    for r in _parse_table_output(_run(f"SELECT COLUMN_NAME FROM information_schema.columns WHERE {where} ORDER BY ORDINAL_POSITION", database)):
        if "COLUMN_NAME" in r:
            facts["columns"].append(r["COLUMN_NAME"])
    stats = _parse_table_output(_run(f"SELECT INDEX_NAME, SEQ_IN_INDEX, COLUMN_NAME FROM information_schema.statistics WHERE {where} ORDER BY INDEX_NAME, SEQ_IN_INDEX", database))
    for r in stats:
        if "INDEX_NAME" in r:
            facts["indexes"].setdefault(r["INDEX_NAME"], []).append(r["COLUMN_NAME"])
    raw = _run(f"ANALYZE TABLE {table}", database)
    m = re.search(r"ANALYZE: \S+ \((\d+) rows\)", raw)
    if m:
        facts["rows"] = int(m.group(1))
    shown = [c[:12] for c in facts["columns"]]  # ANALYZE TABLE cuts column names to 12 characters
    for line in raw.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) >= 3 and cells[0] in shown and shown.count(cells[0]) == 1 and cells[1].isdigit():
            facts["distinct"][facts["columns"][shown.index(cells[0])]] = int(cells[1])
    return facts


def _error_text(raw: str) -> str:
    return " ".join(l for l in raw.splitlines() if l.strip() and l.strip() not in ("ERR", "---END---", "ERROR"))[:300]


def _plan_summary(sql: str, database: str) -> dict:
    raw = _run(f"EXPLAIN ANALYZE {sql}", database)
    if raw.strip().upper().startswith(("ERR", "ERROR")):
        return {"error": _error_text(raw)}
    plan: dict = {"access": [], "steps": []}
    for line in raw.splitlines():
        text = line.strip().strip("|").strip()
        if text.startswith(("Access:", "Join:")):
            plan["access"].append(re.sub(r"\s+", " ", text))
        m = re.match(r"Estimated rows\s*:\s*(\d+)\s+Actual rows\s*:\s*(\d+)", text)
        if m:
            plan["estimated_rows"], plan["actual_rows"] = int(m.group(1)), int(m.group(2))
        m = re.match(r"Execution time\s*:\s*([\d.]+) (ms|sec)", text)
        if m:
            plan["execution_ms"] = float(m.group(1)) * (1000 if m.group(2) == "sec" else 1)
        m = re.match(r"Step (\d+) (.+?) : (\d+) rows, ([\d.]+) ms, (\d+)%", text)
        if m:
            plan["steps"].append({"step": int(m.group(1)), "what": m.group(2), "rows": int(m.group(3)), "ms": float(m.group(4)), "share_percent": int(m.group(5))})
    return plan


def _estimated_plan(sql: str, database: str) -> dict:
    """The plan of an UPDATE or DELETE as the engine estimates it (EXPLAIN runs nothing): how the rows are found and what the plan costs."""
    raw = _run(f"EXPLAIN {sql}", database)
    if raw.strip().upper().startswith(("ERR", "ERROR")):
        return {"error": _error_text(raw)}
    plan: dict = {"access": [], "estimated": True}
    for line in raw.splitlines():
        text = line.strip().strip("|").strip()
        if text.startswith(("Access:", "Join:")):
            plan["access"].append(re.sub(r"\s+", " ", text))
        m = re.match(r"Est\. cost\s*:\s*([\d.]+)", text)
        if m:
            plan["estimated_cost"] = float(m.group(1))
        m = re.match(r"Rows \(total\)\s*:\s*(\d+)", text)
        if m:
            plan["rows_in_table"] = int(m.group(1))
    if "estimated_cost" not in plan:
        return {"error": "The engine gave no plan for this statement: " + _error_text(raw)}
    return plan


class _Reading:
    """What the engine found, for the trial: the suggestions and the facts (rows) of the tables they are for."""

    def __init__(self, database: str):
        self.database = database
        self.facts: dict[str, dict] = {}
        self.suggestions: list[dict] = []

    def table(self, name: str) -> dict:
        if name not in self.facts:
            self.facts[name] = _table_facts(name, self.database)
        return self.facts[name]


def _engine_advice(sql: str, database: str) -> dict:
    """The answer of the engine's SUGGEST INDEX as {"suggestions", "already", "notes"}, or {"error"}."""
    raw = _run("SUGGEST INDEX " + sql.strip().rstrip(";"), database)
    if raw.strip().upper().startswith(("ERR", "ERROR")):
        return {"error": _error_text(raw) or "SUGGEST INDEX failed"}
    rows = _parse_table_output(raw)
    if rows and "kind" not in rows[0]:
        return {"error": "The engine did not answer SUGGEST INDEX as expected: " + raw.strip()[:200]}
    out: dict = {"suggestions": [], "already": [], "notes": []}

    def number(value):
        return int(value) if str(value).strip().isdigit() else None

    for r in rows:
        columns = [c.strip() for c in r.get("columns", "").split(",") if c.strip()]
        if r["kind"] == "suggest":
            entry: dict = {"table": r.get("table_name", ""), "columns": columns, "create_index": r.get("create_index", ""), "why": r.get("detail", "")}
            if r.get("block"):
                entry["in"] = r["block"]
            if number(r.get("rows_read_with_index")) is not None:
                entry["rows_read_with_index_estimate"] = number(r["rows_read_with_index"])
                entry["rows_in_table"] = number(r.get("rows_in_table"))
            if r.get("needs_rewrite") == "yes":
                entry["needs_rewrite"] = True
            out["suggestions"].append(entry)
        elif r["kind"] == "already":
            m = re.search(r"index (\S+)$", r.get("detail", ""))
            out["already"].append({"table": r.get("table_name", ""), "columns": columns, "index": m.group(1) if m else r.get("detail", "")})
        elif r["kind"] == "note":
            out["notes"].append(r.get("detail", ""))
    return out


def _median(values: list[float]) -> float:
    ordered = sorted(values)
    return ordered[len(ordered) // 2]


def _is_write(sql: str) -> bool:
    return re.match(r"\s*(UPDATE|DELETE)\b", sql, flags=re.IGNORECASE) is not None


def _measured_plan(sql: str, database: str) -> dict:
    """The plan of a SELECT with the time of its median run of a few (a comment makes each run its own statement, so that no cached answer is measured); of an UPDATE or DELETE
    the estimated plan, which runs nothing."""
    if _is_write(sql):
        return _estimated_plan(sql, database)
    plans = [_plan_summary(f"{sql} /* try {k} */", database) for k in range(_VERIFY_RUNS)]
    if any("error" in p for p in plans):
        return next(p for p in plans if "error" in p)
    plan = dict(plans[0])
    times = [p["execution_ms"] for p in plans if "execution_ms" in p]
    if times:
        plan["execution_ms"] = round(_median(times), 3)
    return plan


def _plan_change(before: dict, after: dict) -> dict:
    out: dict = {}
    if "execution_ms" in before and "execution_ms" in after:
        out["execution_ms"] = {"before": before["execution_ms"], "after": after["execution_ms"]}
        if after["execution_ms"] > 0:
            out["speedup"] = round(before["execution_ms"] / after["execution_ms"], 2)
    elif "estimated_cost" in before and "estimated_cost" in after:  # (an UPDATE / DELETE: nothing was run, the cost is the engine's estimate)
        out["estimated_cost"] = {"before": before["estimated_cost"], "after": after["estimated_cost"]}
        if after["estimated_cost"] > 0:
            out["speedup"] = round(before["estimated_cost"] / after["estimated_cost"], 2)
    if before.get("access") != after.get("access"):
        out["access"] = {"before": before.get("access", []), "after": after.get("access", [])}
    if "estimated_rows" in before or "estimated_rows" in after:
        out["rows_read_estimate"] = {"before": before.get("estimated_rows"), "after": after.get("estimated_rows")}
    speedup = out.get("speedup")
    scan_before = any("scan" in a.lower() for a in before.get("access", []))
    scan_after = any("scan" in a.lower() for a in after.get("access", []))
    if (speedup is not None and speedup >= 1.25) or (scan_before and not scan_after):
        out["verdict"] = "faster" if speedup is not None and speedup >= 1.25 else "uses the index"
        if "estimated_cost" in out and out["verdict"] == "faster":
            out["verdict"] = "cheaper (estimated)"
    elif speedup is not None and speedup <= 0.8:
        out["verdict"] = "slower"
    else:
        out["verdict"] = "no change"
    return out


def _verify(sql: str, rd: _Reading, rewrite_sql: str | None) -> dict:
    """Tries the suggested indexes on the statement: `before` is its plan now; each suggestion is built under a temporary name, the plan is measured again and the index
    is dropped; then all of them together (a join may need two); the branches of an OR together, measured on the rewrite. The tables are as they were afterwards; an index
    that could not be dropped is named. An UPDATE or DELETE is not run: its plans are the engine's estimates (EXPLAIN)."""
    database = rd.database
    before = _measured_plan(sql, database)
    if "error" in before:
        return {"error": "The statement could not be run for a measurement: " + before["error"]}
    left_behind: list[str] = []
    created: list[tuple[str, str]] = []

    def build(tag: int, s: dict) -> str | None:
        name = f"__try_{tag}_{s['table']}"
        out = _run(f"CREATE INDEX {name} ON {s['table']} ({', '.join(s['columns'])});", database)
        if out.strip().upper().startswith(("ERR", "ERROR")):
            return " ".join(l for l in out.splitlines() if l.strip() and l.strip() not in ("ERR", "---END---"))[:200]
        created.append((name, s["table"]))
        return None

    def drop_all():
        while created:
            name, table = created.pop()
            out = _run(f"DROP INDEX {name} ON {table};", database)
            if out.strip().upper().startswith(("ERR", "ERROR")):
                left_behind.append(f"{name} ON {table}")

    def too_big(s: dict) -> str | None:
        rows = rd.table(s["table"]).get("rows")
        if rows is not None and rows > _VERIFY_MAX_ROWS:
            return f"{s['table']} has {rows} rows: building an index for a trial is not worth it above {_VERIFY_MAX_ROWS}."
        return None

    def trial(group: list[dict], statement: str, tag: int) -> dict:
        """Builds the indexes of `group`, measures `statement`, drops them: the change from `before`."""
        entry: dict = {"create_index": [s["create_index"] for s in group] if len(group) > 1 else group[0]["create_index"]}
        for s in group:
            if reason := too_big(s):
                entry["skipped"] = reason
                return entry
        try:
            for k, s in enumerate(group):
                if problem := build(tag + k, s):
                    entry["error"] = problem
                    return entry
            after = _measured_plan(statement, database)
        finally:
            drop_all()
        entry.update(_plan_change(before, after) if "error" not in after else {"error": after["error"]})
        return entry

    chosen = rd.suggestions[:_VERIFY_MAX_INDEXES]
    plain = [s for s in chosen if not s.get("needs_rewrite")]
    branches = [s for s in chosen if s.get("needs_rewrite")]
    out: dict = {"before": before}
    try:
        if plain:
            out["each_index_alone"] = [trial([s], sql, 10 * i) for i, s in enumerate(plain, 1)]
        if len(plain) >= 2:
            out["all_together"] = trial(plain, sql, 100)
        if branches and rewrite_sql:
            out["or_rewrite"] = {"rewrite": rewrite_sql, **trial(branches, rewrite_sql, 200)}
        elif branches:
            out["or_rewrite"] = {"note": "The branches of the OR need a rewrite (a statement of each branch), which is not possible to write for this statement automatically: build the indexes and measure the rewrite yourself."}
    finally:
        drop_all()
    out["note"] = ("Each index was built under a temporary name, " + ("the statement was NOT run (the plans are the engine's estimates)" if _is_write(sql) else
                   f"the statement was measured (median of {_VERIFY_RUNS} runs)") + " and the index was dropped again; the tables are as they were.")
    if left_behind:
        out["left_behind"] = left_behind
        out["note"] += " These temporary indexes could not be dropped, drop them yourself: " + ", ".join(left_behind) + "."
    return out


def suggest_indexes_for(sql: str, database: str = "", verify: bool = False) -> dict:
    stripped = _strip_sql(sql)
    first = re.match(r"[\s(]*([A-Za-z]+)", stripped)
    kind = first.group(1).upper() if first else ""
    if kind not in ("SELECT", "UPDATE", "DELETE", "WITH"):
        return {"error": "suggest_indexes reads a SELECT, UPDATE or DELETE statement."}
    advice = _engine_advice(sql, database)
    if "error" in advice:
        return advice
    if kind == "WITH":
        kind = "SELECT"
    rd = _Reading(database)
    rd.suggestions = advice["suggestions"]
    names = list(dict.fromkeys([s["table"] for s in advice["suggestions"]] + [a["table"] for a in advice["already"]]))
    shown_tables = {}
    for t in names:
        facts = rd.table(t)
        if facts["columns"]:
            shown_tables[t] = {"rows": facts["rows"], "indexes": facts["indexes"]}
    result: dict = {"statement": kind, "tables": shown_tables, "suggestions": advice["suggestions"]}
    blocks = sorted({s.get("in", "the statement") for s in advice["suggestions"]})
    if blocks and blocks != ["the statement"]:
        result["blocks"] = blocks
    if advice["already"]:
        result["already_indexed"] = advice["already"]
    rewrite = _or_rewrite(sql) if any(s.get("needs_rewrite") for s in advice["suggestions"]) else None
    if rewrite:
        result["rewrites"] = [rewrite]
    if advice["notes"]:
        result["notes"] = list(dict.fromkeys(advice["notes"]))
    text = sql.strip().rstrip(";")
    if kind == "SELECT":
        result["plan"] = _plan_summary(text, database)
        slow = max(result["plan"].get("steps", []), key=lambda s: s["share_percent"], default=None)
        if slow and slow["share_percent"] >= 50:
            result["slowest_step"] = f"Step {slow['step']} ({slow['what']}) takes {slow['share_percent']}% of the time"
    else:
        result["plan"] = _estimated_plan(text, database)  # (EXPLAIN UPDATE / DELETE: the rows the statement looks for, nothing is run)
        result.setdefault("notes", []).append("The plan is the engine's estimate (EXPLAIN): an UPDATE or DELETE is not run by this tool.")
    if verify and advice["suggestions"]:
        result["verification"] = _verify(text, rd, rewrite["sql"].rstrip(";") if rewrite else None)
    elif verify:
        result["verification"] = {"note": "There is no index to try."}
    result["caveat"] = "A heuristic reading of the statement; the row estimates assume the columns are independent. " + (
        "The tables are as they were: the trial indexes were dropped." if verify and "verification" in result else
        "Nothing was created: run a CREATE INDEX yourself and EXPLAIN again to see the plan change (or ask for verify=True).")
    return result


@mcp.tool()
def suggest_indexes(sql: str, database: str = "", verify: bool = False) -> str:
    """Suggest indexes for a SELECT, UPDATE or DELETE: the engine reads the statement (also a WITH, subqueries in IN / EXISTS / the FROM list / a comparison, correlated ones,
    the branches of a UNION, and an OR branch by branch -- the same reading the desktop client shows under a plan), the columns and indexes of its tables and how many distinct
    values each column has, and this tool returns a JSON object with the CREATE INDEX statements that would let the statement stop scanning (equality columns first, then one
    range column, then ORDER BY), why, how many rows it would read with them, the indexes the tables already have, a rewrite where an OR needs one (the engine reads one index per
    statement), the reasons an index would not help (a function around a column, a leading wildcard, a small table) and the plan of the statement (EXPLAIN ANALYZE for a SELECT,
    EXPLAIN for an UPDATE / DELETE, which is not run).
    By default it creates nothing. With verify=True it TRIES the suggestions: it builds each one under a temporary name, measures the plan before and after, drops the index again
    and reports what changed. For a SELECT the plan is EXPLAIN ANALYZE (median of 3 runs: rows read, time, speedup); an UPDATE or DELETE is not run, its plans are the engine's
    estimates (EXPLAIN: access path and cost). It briefly writes to the database (the temporary index), so say so before using it on a busy one; a table above 200,000 rows is not
    tried."""
    return json.dumps(suggest_indexes_for(sql, database, verify), ensure_ascii=False)


# ─── Monitoring and structure tools (R54): list_locks, health_report, get_table_stats, get_fk_graph, trace_query ───────────────────────────────────────────
# Thin readers over what the engine already answers (information_schema, SHOW GLOBAL STATUS, ANALYZE TABLE, TRACE): they gather the answers of several statements into one
# JSON object, so that an AI client does not have to know which tables to ask or how to read a box-drawn table. None of them changes data (trace_query runs the statement
# inside a transaction that it rolls back).
def _rows(sql: str, database: str = "") -> list[dict]:
    """The rows a SELECT / SHOW answers, as dicts (no row: an empty list). An error answer raises RuntimeError with the engine's words."""
    raw = _run(sql, database).strip()
    if raw.upper().startswith(("ERR", "ERROR")):
        raise RuntimeError(" ".join(l.strip() for l in raw.splitlines() if l.strip() and l.strip().upper() != "ERR")[:300])
    rows = _parse_table_output(raw)
    if len(rows) == 1 and ("result" in rows[0] or "error" in rows[0]):
        return []
    return rows


def _num(value, default=None):
    """A number from a cell of text (the engine writes every value as text); `default` when it is not one."""
    try:
        return int(value)
    except (TypeError, ValueError):
        try:
            return float(value)
        except (TypeError, ValueError):
            return default


def _fail(e: Exception) -> str:
    return json.dumps({"error": str(e)}, ensure_ascii=False)


def list_locks_report() -> dict:
    """Open transactions, the row locks they hold, who waits for whom and the recent deadlocks, in one object."""
    trx = _rows("SELECT * FROM information_schema.innodb_trx")
    locks = _rows("SELECT * FROM information_schema.innodb_locks")
    waits = _rows("SELECT * FROM information_schema.innodb_lock_waits")
    deadlocks = _rows("SELECT * FROM information_schema.innodb_deadlocks")
    held: dict[str, list[dict]] = {}
    for lock in locks:
        held.setdefault(lock.get("LOCK_TRX_ID", ""), []).append({"table": lock.get("LOCK_TABLE"), "key": lock.get("LOCK_DATA"), "mode": lock.get("LOCK_MODE")})
    waiting = {w.get("REQUESTING_TRX_ID") for w in waits}
    chains = [{"waiting_trx": w.get("REQUESTING_TRX_ID"), "blocked_by_trx": w.get("BLOCKING_TRX_ID"), "blocker_holds": held.get(w.get("BLOCKING_TRX_ID", ""), [])} for w in waits]
    # the transactions that block others and wait for nobody themselves: ending one of them frees the rest
    root_blockers = sorted({w.get("BLOCKING_TRX_ID") for w in waits if w.get("BLOCKING_TRX_ID") not in waiting})
    out = {
        "summary": {"open_transactions": len(trx), "row_locks_held": len(locks), "waiting_transactions": len(waiting), "recent_deadlocks": len(deadlocks)},
        "transactions": [{"trx_id": t.get("TRX_ID"), "state": t.get("TRX_STATE"), "rows_locked": _num(t.get("TRX_ROWS_LOCKED")),
                          "waiting_for_trx": None if t.get("TRX_WAITING_FOR") in (None, "NULL", "") else t.get("TRX_WAITING_FOR"),
                          "locks": held.get(t.get("TRX_ID", ""), [])} for t in trx],
        "waits": chains,
        "root_blockers": root_blockers,
        "deadlocks": [{"requesting_trx": d.get("REQUESTING_TRX_ID"), "blocking_trx": d.get("BLOCKING_TRX_ID")} for d in deadlocks],
    }
    if root_blockers:
        out["advice"] = (f"Transaction(s) {', '.join(root_blockers)} hold row locks that other transactions wait for. They end by COMMIT or ROLLBACK from their own session; "
                         f"from here the session can be ended with KILL CONNECTION <id> (ids: SELECT * FROM information_schema.processlist; KILL rolls the transaction back). "
                         f"KILL QUERY <id> only stops the waiting statement.")
    elif trx and not waits:
        out["advice"] = "Transactions are open but nobody is waiting for a lock."
    elif not trx:
        out["advice"] = "No transaction is open: no row is locked."
    return out


@mcp.tool()
def list_locks() -> str:
    """Show the row locks of the server: the open transactions (state, how many rows each has locked, what it waits for), every row lock held (table, key, mode), who waits
    for whom, the transactions that block the others and wait for nobody (ending one of them frees the rest), and the recent deadlocks, as one JSON object with advice. Reads
    SHOW LOCKS' source tables (information_schema.innodb_trx / innodb_locks / innodb_lock_waits / innodb_deadlocks); it changes nothing."""
    try:
        return json.dumps(list_locks_report(), ensure_ascii=False)
    except (RuntimeError, OSError) as e:
        return _fail(e)


def health_report_data() -> dict:
    """The state of the server in one object: counters (SHOW GLOBAL STATUS), sessions, open transactions and waits, the last start's recovery, and a list of findings."""
    status = {r.get("Variable_name"): r.get("Value") for r in _rows("SHOW GLOBAL STATUS")}
    n = lambda key: _num(status.get(key), 0)
    uptime = n("Uptime")
    sessions = _rows("SELECT * FROM information_schema.processlist")
    locks = list_locks_report()
    recovery = (_rows("SELECT * FROM information_schema.recovery") or [{}])[0]
    try:
        databases = [{"name": r.get("TABLE_SCHEMA"), "tables": _num(r.get("n"), 0), "rows": _num(r.get("r"), 0)}
                     for r in _rows("SELECT TABLE_SCHEMA, COUNT(*) AS n, SUM(TABLE_ROWS) AS r FROM information_schema.tables GROUP BY TABLE_SCHEMA ORDER BY TABLE_SCHEMA")]
    except RuntimeError:
        databases = []
    kinds = {k[4:].lower(): _num(v, 0) for k, v in status.items() if k.startswith("Com_") and _num(v, 0)}
    requests, reads = n("Innodb_buffer_pool_read_requests"), n("Innodb_buffer_pool_reads")
    running = [s for s in sessions if s.get("COMMAND") not in ("Sleep", None)]
    long_running = [s for s in running if (_num(s.get("TIME"), 0) or 0) >= 5 and "processlist" not in (s.get("INFO") or "").lower()]
    report = {
        "server": {"uptime_seconds": uptime, "connections_total": n("Connections"), "connections_now": n("Threads_connected"), "connections_max": n("Max_used_connections"),
                   "running_now": n("Threads_running"), "failed_logins": n("Aborted_connects"), "slow_queries_total": n("Slow_queries"),
                   "refused_for_max_connections": n("Connection_errors_max_connections")},
        "statements": {"total": n("Questions"), "per_second": round(n("Questions") / uptime, 2) if uptime else None, "by_kind": kinds},
        "buffer_pool": {"pages_used": n("Innodb_buffer_pool_pages_data"), "pages_total": n("Innodb_buffer_pool_pages_total"), "read_requests": requests, "disk_reads": reads,
                        "hit_ratio": round(1 - reads / requests, 4) if requests else None},
        "redo_log_bytes": n("Innodb_redo_log_bytes"),
        "locks": {"row_lock_waits_total": n("Innodb_row_lock_waits"), "waiting_now": n("Innodb_row_lock_current_waits"), "deadlocks_total": n("Innodb_deadlocks"),
                  "open_transactions": locks["summary"]["open_transactions"], "row_locks_held": locks["summary"]["row_locks_held"], "root_blockers": locks["root_blockers"]},
        "sessions": [{"id": _num(s.get("ID")), "user": s.get("USER"), "host": s.get("HOST"), "db": None if s.get("DB") in (None, "NULL") else s.get("DB"), "command": s.get("COMMAND"),
                      "seconds": _num(s.get("TIME"), 0), "info": (s.get("INFO") or "")[:120] if s.get("INFO") not in (None, "NULL") else None} for s in sessions],
        "last_start": {"open_ms": _num(recovery.get("OPEN_MS")), "undo_ms": _num(recovery.get("UNDO_MS")), "redo_ms": _num(recovery.get("REDO_MS")),
                       "wal_transactions_replayed": _num(recovery.get("WAL_TXNS_REPLAYED")), "wal_transactions_undone": _num(recovery.get("WAL_TXNS_UNDONE")),
                       "redo_operations": _num(recovery.get("REDO_OPS")), "crash_detected": recovery.get("CRASH_DETECTED") == "YES"},
        "databases": databases,
    }
    findings = []
    if report["last_start"]["crash_detected"]:
        findings.append({"level": "info", "what": f"The last start recovered from a crash: {report['last_start']['wal_transactions_undone']} unfinished transaction(s) undone, "
                                                  f"{report['last_start']['redo_operations']} redo operation(s) replayed."})
    if locks["root_blockers"]:
        findings.append({"level": "warn", "what": f"Transaction(s) {', '.join(locks['root_blockers'])} block {len(locks['waits'])} other(s) that wait for a row lock (see list_locks)."})
    if n("Innodb_deadlocks"):
        findings.append({"level": "warn", "what": f"{n('Innodb_deadlocks')} deadlock(s) since the start: a transaction was rolled back to break each."})
    if long_running:
        findings.append({"level": "warn", "what": "Statement(s) running for 5 seconds or more: " + ", ".join(f"session {s.get('ID')} ({s.get('TIME')} s)" for s in long_running)})
    if n("Aborted_connects"):
        findings.append({"level": "info", "what": f"{n('Aborted_connects')} login(s) failed since the start."})
    if n("Slow_queries"):
        findings.append({"level": "info", "what": f"{n('Slow_queries')} statement(s) ran longer than long_query_time since the start (see slow_queries, then suggest_indexes)."})
    if n("Connection_errors_max_connections"):
        findings.append({"level": "warn", "what": f"{n('Connection_errors_max_connections')} connection(s) were refused because max_connections was reached (SET GLOBAL max_connections raises it)."})
    if report["redo_log_bytes"] > 4 * 1024 * 1024:
        findings.append({"level": "info", "what": f"The redo log holds {report['redo_log_bytes']} bytes: a checkpoint is due."})
    report["findings"] = findings
    report["status"] = "attention" if any(f["level"] == "warn" for f in findings) else "ok"
    return report


@mcp.tool()
def health_report() -> str:
    """One-call summary of the server's health, for "how is the server doing?": uptime, connections (now / most / total / failed logins), statements (total, per second, by
    kind), buffer pool (pages, hit ratio), redo log size, row-lock waits and deadlocks, open transactions and who blocks whom, every session and what it runs, what the last
    start did to recover (crash detected, transactions undone, redo replayed, how many ms), the databases with their table and row counts, and a list of findings
    (level info / warn) with an overall status "ok" or "attention". Reads SHOW GLOBAL STATUS and information_schema; it changes nothing."""
    try:
        return json.dumps(health_report_data(), ensure_ascii=False)
    except (RuntimeError, OSError) as e:
        return _fail(e)


def table_stats_for(table: str, database: str = "") -> dict:
    """Row count, the shape of each column (distinct values, NULLs, min / max / quartiles) and the indexes of a table. Runs ANALYZE TABLE, which refreshes the planner's statistics."""
    where = f"TABLE_NAME = '{table}' AND TABLE_SCHEMA = DATABASE()"
    columns = _rows(f"SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_KEY FROM information_schema.columns WHERE {where} ORDER BY ORDINAL_POSITION", database)
    if not columns:
        raise RuntimeError(f"Table '{table}' not found" + (f" in database '{database}'" if database else ""))
    index_rows = _rows(f"SELECT INDEX_NAME, SEQ_IN_INDEX, COLUMN_NAME, NON_UNIQUE FROM information_schema.statistics WHERE {where} ORDER BY INDEX_NAME, SEQ_IN_INDEX", database)
    indexes: dict[str, dict] = {}
    for r in index_rows:
        entry = indexes.setdefault(r["INDEX_NAME"], {"columns": [], "unique": r.get("NON_UNIQUE") == "0"})
        entry["columns"].append(r["COLUMN_NAME"])
    raw = _run(f"ANALYZE TABLE {table}", database)
    if raw.strip().upper().startswith(("ERR", "ERROR")):
        raise RuntimeError(" ".join(l.strip() for l in raw.splitlines() if l.strip() and l.strip().upper() != "ERR")[:300])
    rows = None
    m = re.search(r"ANALYZE: \S+ \((\d+) rows\)", raw)
    if m:
        rows = int(m.group(1))
    # the table of numbers: a header line ("column | distinct | nulls | min | ...") and one line per column, whose names the engine cuts to 12 characters
    stats: dict[str, dict] = {}
    header: list[str] = []
    for line in raw.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if cells and cells[0] == "column":
            header = cells
        elif header and len(cells) == len(header) and not line.lstrip().startswith("+"):
            stats[cells[0]] = dict(zip(header[1:], cells[1:]))
    names = [c["COLUMN_NAME"] for c in columns]
    shown = [n[:12] for n in names]
    result_columns = []
    for c in columns:
        name = c["COLUMN_NAME"]
        cut = name[:12]
        s = stats.get(cut) if shown.count(cut) == 1 else None
        entry = {"name": name, "type": c.get("COLUMN_TYPE"), "nullable": c.get("IS_NULLABLE") == "YES", "key": c.get("COLUMN_KEY") or None,
                 "indexes": [i for i, d in indexes.items() if name in d["columns"]]}
        if s:
            distinct, nulls = _num(s.get("distinct")), _num(s.get("nulls"))
            entry.update({"distinct": distinct, "nulls": nulls,
                          "null_fraction": round(nulls / rows, 4) if rows and nulls is not None else None,
                          "selectivity": round(distinct / rows, 4) if rows and distinct is not None else None,
                          "min": s.get("min"), "max": s.get("max"), "p25": s.get("p25"), "p50": s.get("p50"), "p75": s.get("p75")})
        result_columns.append(entry)
    return {"table": table, "database": database or None, "rows": rows, "columns": result_columns, "indexes": indexes,
            "note": "ANALYZE TABLE was run to get these numbers; it refreshes the planner's statistics and changes no data."}


@mcp.tool()
def get_table_stats(table: str, database: str = "") -> str:
    """Statistics of a table as one JSON object: the row count; for every column its type, whether it may be NULL, the distinct values, how many NULLs (and the fraction),
    the selectivity (distinct / rows: 1 means every row differs), min, max and the quartiles, and which indexes cover it; and the indexes with their columns and whether
    they are unique. Runs ANALYZE TABLE (it refreshes the planner's statistics, no data changes). Use it before writing a query or choosing an index."""
    try:
        return json.dumps(table_stats_for(table, database), ensure_ascii=False)
    except (RuntimeError, OSError) as e:
        return _fail(e)


def fk_graph_for(database: str = "", table: str = "", to_table: str = "") -> dict:
    """The foreign keys of a database as edges (child column -> parent column, with the ON DELETE / ON UPDATE rules and a ready JOIN condition), per table what it refers
    to and what refers to it, and -- with table and to_table -- the shortest chain of JOINs between two tables."""
    keys = _rows("SELECT CONSTRAINT_NAME, TABLE_NAME, COLUMN_NAME, REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME FROM information_schema.key_column_usage "
                 "WHERE TABLE_SCHEMA = DATABASE() AND REFERENCED_TABLE_NAME IS NOT NULL ORDER BY TABLE_NAME, CONSTRAINT_NAME, ORDINAL_POSITION", database)
    rules = {(r["CONSTRAINT_NAME"], r["TABLE_NAME"]): r for r in _rows(
        "SELECT CONSTRAINT_NAME, TABLE_NAME, UPDATE_RULE, DELETE_RULE FROM information_schema.referential_constraints WHERE CONSTRAINT_SCHEMA = DATABASE()", database)}
    tables = sorted(r["TABLE_NAME"] for r in _rows("SELECT TABLE_NAME FROM information_schema.tables WHERE TABLE_SCHEMA = DATABASE()", database))
    if not tables:
        raise RuntimeError("No tables" + (f" in database '{database}'" if database else " (give the database)"))
    edges = []
    for k in keys:
        rule = rules.get((k["CONSTRAINT_NAME"], k["TABLE_NAME"]), {})
        edges.append({"constraint": k["CONSTRAINT_NAME"], "child": k["TABLE_NAME"], "child_column": k["COLUMN_NAME"],
                      "parent": k["REFERENCED_TABLE_NAME"], "parent_column": k["REFERENCED_COLUMN_NAME"],
                      "on_delete": rule.get("DELETE_RULE"), "on_update": rule.get("UPDATE_RULE"),
                      "join": f"{k['TABLE_NAME']}.{k['COLUMN_NAME']} = {k['REFERENCED_TABLE_NAME']}.{k['REFERENCED_COLUMN_NAME']}"})
    detail = {t: {"references": [], "referenced_by": []} for t in tables}
    for e in edges:
        if e["child"] in detail:
            detail[e["child"]]["references"].append({"table": e["parent"], "via": e["join"], "on_delete": e["on_delete"], "on_update": e["on_update"]})
        if e["parent"] in detail:
            detail[e["parent"]]["referenced_by"].append({"table": e["child"], "via": e["join"], "on_delete": e["on_delete"], "on_update": e["on_update"]})
    out: dict = {"database": database or None, "tables": tables, "edges": edges,
                 "root_tables": [t for t in tables if not detail[t]["references"]],
                 "unconnected_tables": [t for t in tables if not detail[t]["references"] and not detail[t]["referenced_by"]]}
    if table:
        if table not in detail:
            raise RuntimeError(f"Table '{table}' not found")
        out["table"] = {"name": table, **detail[table]}
        out["edges"] = [e for e in edges if table in (e["child"], e["parent"])]
        if to_table:
            if to_table not in detail:
                raise RuntimeError(f"Table '{to_table}' not found")
            path = _fk_join_path(edges, table, to_table)
            out["join_path"] = None  # (no chain of foreign keys connects the two tables)
            if path is not None:
                chain, here = [table], table
                for e in path:
                    here = e["parent"] if e["child"] == here else e["child"]
                    chain.append(here)
                out["join_path"] = {"tables": chain, "edges": path,
                                    "sql": f"FROM {table}" + "".join(f" JOIN {chain[i + 1]} ON {e['join']}" for i, e in enumerate(path))}
    elif to_table:
        raise RuntimeError("to_table needs table")
    return out


def _fk_join_path(edges: list[dict], start: str, goal: str) -> list[dict] | None:
    """The shortest chain of foreign keys (either direction) from one table to another, by breadth-first search; [] when they are the same table, None when none connects them."""
    if start == goal:
        return []
    seen = {start}
    queue = [(start, [])]
    while queue:
        here, path = queue.pop(0)
        for e in edges:
            if here not in (e["child"], e["parent"]):
                continue
            there = e["parent"] if e["child"] == here else e["child"]
            if there in seen:
                continue
            if there == goal:
                return path + [e]
            seen.add(there)
            queue.append((there, path + [e]))
    return None


@mcp.tool()
def get_fk_graph(database: str = "", table: str = "", to_table: str = "") -> str:
    """The foreign key relationships of a database, for writing correct JOINs. Returns JSON: every foreign key as an edge (child table.column -> parent table.column, the
    constraint name, ON DELETE / ON UPDATE rules such as CASCADE and a ready JOIN condition "child.col = parent.col"), for each table what it refers to and what refers to
    it, the root tables (no foreign key out) and the tables with no relationships. With `table` only the edges that touch it are returned; with `table` and `to_table` also
    `join_path`: the shortest chain of foreign keys between the two tables (either direction) and the FROM ... JOIN ... ON text for it. Reads information_schema."""
    try:
        return json.dumps(fk_graph_for(database, table, to_table), ensure_ascii=False)
    except (RuntimeError, OSError) as e:
        return _fail(e)


_TRACE_WRITES_RE = re.compile(r"^\s*(INSERT|UPDATE|DELETE|REPLACE|MERGE)\b", re.IGNORECASE)
_TRACE_READS_RE = re.compile(r"^\s*(SELECT|WITH|TABLE|VALUES)\b|^\s*\(\s*SELECT\b", re.IGNORECASE)


def trace_query_for(sql: str, database: str = "") -> dict:
    """TRACE a statement: the table of the stages it went through (parse, locks, scan, joins, filter, execute, answer) with rows, ms and share. A SELECT runs as it is; an
    INSERT / UPDATE / DELETE / REPLACE / MERGE runs inside a transaction that is rolled back, so nothing is saved."""
    stmt = sql.strip().rstrip(";").strip()
    if not stmt:
        raise RuntimeError("sql is empty")
    if re.match(r"^\s*TRACE\b", stmt, re.IGNORECASE):
        stmt = re.sub(r"^\s*TRACE\s+", "", stmt, flags=re.IGNORECASE)
    writes = bool(_TRACE_WRITES_RE.match(stmt))
    if not writes and not _TRACE_READS_RE.match(stmt):
        raise RuntimeError("trace_query traces SELECT, WITH, INSERT, UPDATE, DELETE, REPLACE and MERGE statements (a DDL statement cannot be undone, so it is not traced)")
    raw = ""
    try:
        c = _Conn()
        try:
            if database:
                used = c._raw(f"USE {database};")
                if used.strip().upper().startswith("ERR"):
                    raise RuntimeError(" ".join(l for l in used.splitlines() if l.strip() and l.strip().upper() != "ERR"))
            if writes:
                began = c._raw("BEGIN;")
                if began.strip().upper().startswith("ERR"):
                    raise RuntimeError(f"could not start a transaction: {began.strip()}")
            try:
                raw = c._raw(f"TRACE {stmt};")
            finally:
                if writes:
                    c._raw("ROLLBACK;")
        finally:
            c.close()
    except ConnectionRefusedError:
        raise RuntimeError(f"RuSQL is not running on {RUSQL_HOST}:{RUSQL_PORT}. Start the server first.")
    text = raw.strip()
    if text.upper().startswith("ERR"):
        raise RuntimeError(" ".join(l.strip() for l in text.splitlines() if l.strip() and l.strip().upper() != "ERR")[:300])
    steps, total = [], None
    for row in _parse_table_output(text):
        if "Step" not in row:
            continue
        entry = {"what": row.get("What"), "rows": _num(row.get("Rows")), "ms": _num(row.get("ms")), "share_percent": _num(row.get("Share %"))}
        if row["Step"] == "Total":
            total = {"ms": entry["ms"], "result": entry["what"]}
        else:
            entry["step"] = _num(row["Step"])
            steps.append(entry)
    out = {"statement": stmt, "rolled_back": writes, "steps": steps, "total": total}
    timed = [s for s in steps if s["ms"] is not None]
    if timed:
        slow = max(timed, key=lambda s: s["ms"])
        out["slowest"] = f"Step {slow['step']} ({slow['what']}) takes {slow['share_percent']}% of the time"
    if writes:
        out["note"] = "The statement ran inside a transaction that was rolled back: nothing was saved."
    return out


@mcp.tool()
def trace_query(sql: str, database: str = "") -> str:
    """Trace a statement through the engine's stages and return the time of each as JSON: parse, prepare (cache, locks), table locks, every scan / join / filter step with
    its rows and share of the time, execute and return the answer, a total, and which step is the slowest. Use it to find out WHERE a slow statement spends its time (explain_query
    shows the plan, this shows the stages). A SELECT runs as it is; an INSERT / UPDATE / DELETE / REPLACE / MERGE runs inside a transaction that is rolled back, so nothing is saved.
    DDL is refused (it cannot be undone)."""
    try:
        return json.dumps(trace_query_for(sql, database), ensure_ascii=False)
    except (RuntimeError, OSError) as e:
        return _fail(e)


@mcp.tool()
def sample_data(table: str, n: int = 10, database: str = "") -> str:
    """Return up to N sample rows from a table as a JSON array.
    Useful for understanding data distribution before writing queries."""
    n = max(1, min(n, 100))
    raw = _run(f"SELECT * FROM {table} LIMIT {n}", database)
    rows = _parse_table_output(raw)
    return json.dumps(rows, ensure_ascii=False)


# ─── R70: slow_queries, kill_session, import_csv, export_csv, diff_schema, resources and prompts ────────────────────────────────────────────────────────────────────────────
# slow_queries reads the engine's slow query log; kill_session names the session first and ends it only after a second, confirmed call (as DROP does through
# confirm_dangerous_sql); import_csv / export_csv move a CSV file of THIS computer in and out of a table; diff_schema says how two databases differ.
_DB_NAME_RE = re.compile(r"^[A-Za-z0-9_$]+$")


def slow_queries_report(limit: int = 20, min_seconds: float = 0.0) -> dict:
    limit = max(1, min(int(limit), 200))
    # (the native protocol has no SHOW VARIABLES: it answers SELECT @@name, which also tells the value another session has set)
    shown = _rows("SELECT @@slow_query_log AS slow_query_log, @@long_query_time AS long_query_time")
    variables = {"slow_query_log": shown[0].get("slow_query_log", "") if shown else "", "long_query_time": shown[0].get("long_query_time", "") if shown else ""}
    log_on = str(variables["slow_query_log"]).upper() in ("ON", "1")
    counted = _rows("SHOW GLOBAL STATUS LIKE 'Slow_queries'")
    logged = _rows("SELECT START_TIME, USER_HOST, QUERY_TIME, ROWS_SENT, DB, SQL_TEXT, THREAD_ID FROM information_schema.slow_log")
    groups: dict[str, dict] = {}
    for r in logged:
        seconds = float(_num(r.get("QUERY_TIME"), 0) or 0)
        if seconds < min_seconds:
            continue
        sql = " ".join(str(r.get("SQL_TEXT", "")).split())
        g = groups.setdefault(sql, {"sql": sql, "database": r.get("DB", ""), "runs": 0, "total_seconds": 0.0, "slowest_seconds": 0.0, "rows_sent_last": None, "last_started": ""})
        g["runs"] += 1
        g["total_seconds"] = round(g["total_seconds"] + seconds, 6)
        g["slowest_seconds"] = max(g["slowest_seconds"], seconds)
        g["rows_sent_last"] = _num(r.get("ROWS_SENT"))
        g["last_started"] = max(g["last_started"], r.get("START_TIME", ""))
    statements = sorted(groups.values(), key=lambda g: (-g["total_seconds"], -g["slowest_seconds"]))[:limit]
    out: dict = {
        "slow_query_log": "ON" if log_on else "OFF",
        "long_query_time_seconds": float(_num(variables["long_query_time"], 0) or 0),
        "slow_queries_counted_since_start": _num(counted[0].get("Value")) if counted else None,
        "logged_statements": len(logged),
        "statements": statements,
    }
    if statements:
        out["next"] = "Pass one of these statements to suggest_indexes (verify=True tries the suggested indexes on a copy of the plan and shows the speedup) or to explain_query to see where the time goes."
    elif not log_on:
        out["next"] = ("The slow query log is OFF, so nothing was written down. If the user agrees, turn it on with execute_sql('SET GLOBAL slow_query_log = ON') "
                       "(and SET GLOBAL long_query_time = 0.5 to catch more), let the workload run, and call slow_queries again.")
    else:
        out["next"] = f"No statement ran for {out['long_query_time_seconds']} seconds or more since the log was turned on."
    return out


@mcp.tool()
def slow_queries(limit: int = 20, min_seconds: float = 0.0) -> str:
    """The slow queries: the statements that ran for at least long_query_time (the engine's slow query log, information_schema.slow_log, the last 1000 statements while
    slow_query_log is ON), grouped by statement text and sorted by the time they took in all, as JSON: how many runs, the total and slowest seconds, the rows sent, the database.
    Also says whether the log is ON, the threshold, and how many slow statements the server counted since it started. If the log is OFF the answer says how to turn it on (ask
    the user first). Use it for "which queries are slow lately?", then suggest_indexes on a statement."""
    try:
        return json.dumps(slow_queries_report(limit, min_seconds), ensure_ascii=False)
    except Exception as e:
        return _fail(e)


def kill_session_report(session_id: int, kill_connection: bool, confirmed: bool) -> str:
    rows = _rows(f"SELECT ID, USER, HOST, DB, COMMAND, TIME, STATE, INFO FROM information_schema.processlist WHERE ID = {int(session_id)}")
    if not rows:
        return f"Error: there is no session {int(session_id)} (list the sessions with list_locks or execute_sql('SHOW PROCESSLIST'))."
    s = rows[0]
    what = "the whole connection (the client is disconnected)" if kill_connection else "the statement it is running now (the connection stays)"
    description = (f"session {s.get('ID')} of {s.get('USER')}@{s.get('HOST')} on database {s.get('DB') or '-'}, {s.get('COMMAND')} for {s.get('TIME')} s"
                   + (f", running: {str(s.get('INFO'))[:200]}" if s.get("INFO") and s.get("INFO") != "NULL" else ""))
    if not confirmed:
        return (f"Refused: this ends {what} of {description}. Show the user the session and ask them to confirm, then call kill_session again with the same arguments and confirmed=True.")
    answer = _run(f"KILL {'CONNECTION' if kill_connection else 'QUERY'} {int(session_id)}")
    return json.dumps({"session": s, "killed": "connection" if kill_connection else "query", "engine_answer": " ".join(answer.split())[:200]}, ensure_ascii=False)


@mcp.tool()
def kill_session(session_id: int, kill_connection: bool = False, confirmed: bool = False) -> str:
    """End a session that is stuck or running too long: KILL QUERY stops the statement it is running (the default, the connection stays), kill_connection=True also closes the
    connection. Take the session number from list_locks (blockers) or SHOW PROCESSLIST. Ending a session loses what it was doing, so the first call (confirmed=False) only
    describes the session and refuses; show it to the user, and call again with confirmed=True only after they said yes."""
    try:
        return kill_session_report(session_id, kill_connection, confirmed)
    except Exception as e:
        return _fail(e)


_CSV_BATCH_ROWS = 200
_CSV_MAX_BYTES = 50 * 1024 * 1024
_CSV_MAX_EXPORT_ROWS = 1_000_000


def _sql_text(value) -> str:
    """A value as a SQL literal (None: NULL). Backslashes, quotes and line ends are written as escapes: the native protocol sends one statement on one line."""
    if value is None:
        return "NULL"
    text = str(value).replace("\\", "\\\\").replace("'", "''").replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t").replace("\x00", "\\0")
    return f"'{text}'"


def import_csv_report(table: str, file_path: str, database: str, has_header: bool, delimiter: str, empty_as_null: bool, commit: bool) -> str:
    import csv
    path = Path(file_path).expanduser()
    if not path.is_file():
        return f"Error: {file_path} is not a file on this computer."
    if path.stat().st_size > _CSV_MAX_BYTES:
        return f"Error: {path.name} is bigger than {_CSV_MAX_BYTES // (1024 * 1024)} MB: split it."
    if not re.fullmatch(r"[A-Za-z0-9_$.]+", table):
        return "Error: table must be a table name (letters, digits, _ and $)."
    if len(delimiter) != 1:
        return "Error: delimiter is one character."
    with path.open(encoding="utf-8-sig", newline="") as f:
        records = [r for r in csv.reader(f, delimiter=delimiter) if r]
    if not records:
        return "Error: the file has no rows."
    bare = table.split(".")[-1]
    columns = [r["COLUMN_NAME"] for r in _rows(f"SELECT COLUMN_NAME FROM information_schema.columns WHERE TABLE_NAME = '{bare}' AND TABLE_SCHEMA = " + (f"'{database}'" if database else "DATABASE()") + " ORDER BY ORDINAL_POSITION", database)]
    if not columns:
        return f"Error: the table {table} was not found."
    if has_header:
        by_lower = {c.lower(): c for c in columns}
        header, records = [h.strip() for h in records[0]], records[1:]
        unknown = [h for h in header if h.lower() not in by_lower]
        if unknown:
            return f"Error: the header names columns the table does not have: {', '.join(unknown)}. The table has: {', '.join(columns)}."
        target = [by_lower[h.lower()] for h in header]
    else:
        target = columns
    wide = [i + 1 for i, r in enumerate(records) if len(r) > len(target)]
    if wide:
        return f"Error: row {wide[0]} has more cells than the {len(target)} columns it is loaded into."
    statements = []
    for start in range(0, len(records), _CSV_BATCH_ROWS):
        batch = records[start:start + _CSV_BATCH_ROWS]
        values = ", ".join("(" + ", ".join(_sql_text(None if (cell == "" and empty_as_null) else cell) for cell in (r + [""] * (len(target) - len(r)))) + ")" for r in batch)
        statements.append(f"INSERT INTO {table} ({', '.join(target)}) VALUES {values}")
    raw = run_script(statements, database, commit, True)
    try:
        script = json.loads(raw)
    except ValueError:
        return raw
    out = {"file": path.name, "table": table, "columns": target, "rows_in_file": len(records), "mode": script["mode"], "committed": script["committed"], "failed_at": script["failed_at"]}
    failed = [step for step in script["results"] if not step.get("ok")]
    if failed:
        out["error"] = failed[0].get("error", "")
        out["note"] = f"Statement {script['failed_at']} (rows {(script['failed_at'] - 1) * _CSV_BATCH_ROWS + 1} and on) failed, so nothing was loaded."
    elif commit:
        out["note"] = f"{len(records)} rows were loaded and saved."
    else:
        out["note"] = f"PREVIEW: {len(records)} rows were inserted and rolled back, nothing was saved. Call again with commit=True to load them."
    return json.dumps(out, ensure_ascii=False)


@mcp.tool()
def import_csv(table: str, file_path: str, database: str = "", has_header: bool = True, delimiter: str = ",", empty_as_null: bool = True, commit: bool = False) -> str:
    """Load the rows of a CSV file (a UTF-8 file on THIS computer) into an existing table, in one transaction. With has_header=True the first row names the columns (they may
    be in any order); without it the cells go into the table's columns in order. An empty cell is NULL (empty_as_null=False keeps it as an empty text). commit=False (the
    default) is a PREVIEW: the rows are inserted for real inside a transaction that is rolled back, so errors (a duplicate key, a wrong type) show but nothing is saved; call
    again with commit=True once the user agreed. The first row that fails rolls everything back."""
    try:
        return import_csv_report(table, file_path, database, has_header, delimiter, empty_as_null, commit)
    except Exception as e:
        return _fail(e)


def export_csv_report(sql: str, file_path: str, database: str, overwrite: bool) -> str:
    import csv
    if not re.match(r"\s*(SELECT|WITH|SHOW|\()", sql, flags=re.IGNORECASE):
        return "Refused: export_csv writes the answer of a SELECT (or SHOW) statement."
    path = Path(file_path).expanduser()
    if path.suffix.lower() not in (".csv", ".tsv", ".txt"):
        return "Refused: the file must end in .csv, .tsv or .txt."
    if not path.parent.is_dir():
        return f"Error: the folder {path.parent} does not exist."
    if path.exists() and not overwrite:
        return f"Refused: {path} exists. Ask the user whether to replace it, then call again with overwrite=True."
    raw = _run(sql.strip().rstrip(";"), database).strip()
    if raw.upper().startswith(("ERR", "ERROR")):
        return "Error: " + " ".join(l.strip() for l in raw.splitlines() if l.strip() and l.strip().upper() != "ERR")[:300]
    rows = _parse_table_output(raw)
    if not rows or ("result" in rows[0] and len(rows) == 1) or "error" in rows[0]:
        return "Error: the statement answered no table: " + raw[:200]
    if len(rows) > _CSV_MAX_EXPORT_ROWS:
        return f"Error: {len(rows)} rows are more than {_CSV_MAX_EXPORT_ROWS}: add a LIMIT."
    headers = list(rows[0].keys())
    with path.open("w", encoding="utf-8-sig", newline="") as f:
        writer = csv.writer(f, delimiter="\t" if path.suffix.lower() == ".tsv" else ",")
        writer.writerow(headers)
        for r in rows:
            writer.writerow(["" if r[h] == "NULL" else r[h] for h in headers])
    return json.dumps({"file": str(path), "rows": len(rows), "columns": headers, "note": "Written as UTF-8 with a BOM (Excel opens it); NULL is an empty cell."}, ensure_ascii=False)


@mcp.tool()
def export_csv(sql: str, file_path: str, database: str = "", overwrite: bool = False) -> str:
    """Write the answer of a SELECT (or SHOW) statement to a CSV file on THIS computer (.csv, .tsv or .txt; UTF-8 with a BOM so that Excel opens it; NULL is an empty cell).
    An existing file is not replaced unless overwrite=True, which needs the user's yes. At most 1,000,000 rows."""
    try:
        return export_csv_report(sql, file_path, database, overwrite)
    except Exception as e:
        return _fail(e)


def _schema_snapshot(db: str) -> dict:
    """{table: {"columns": {name: [type, nullable, default, extra]}, "indexes": {name: [unique, [columns]]}, "foreign_keys": {constraint: [columns, parent, parent columns, rules]}}}"""
    snap: dict = {}
    for r in _rows(f"SELECT TABLE_NAME, COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_DEFAULT, EXTRA FROM information_schema.columns WHERE TABLE_SCHEMA = '{db}' ORDER BY TABLE_NAME, ORDINAL_POSITION"):
        snap.setdefault(r["TABLE_NAME"], {"columns": {}, "indexes": {}, "foreign_keys": {}})["columns"][r["COLUMN_NAME"]] = [r.get("COLUMN_TYPE", ""), r.get("IS_NULLABLE", ""), r.get("COLUMN_DEFAULT", ""), r.get("EXTRA", "")]
    for r in _rows(f"SELECT TABLE_NAME, INDEX_NAME, NON_UNIQUE, SEQ_IN_INDEX, COLUMN_NAME FROM information_schema.statistics WHERE TABLE_SCHEMA = '{db}' ORDER BY TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX"):
        if r["TABLE_NAME"] in snap:
            entry = snap[r["TABLE_NAME"]]["indexes"].setdefault(r["INDEX_NAME"], [r.get("NON_UNIQUE") == "0", []])
            entry[1].append(r["COLUMN_NAME"])
    for r in _rows(f"SELECT TABLE_NAME, CONSTRAINT_NAME, COLUMN_NAME, REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME FROM information_schema.key_column_usage WHERE TABLE_SCHEMA = '{db}' AND REFERENCED_TABLE_NAME IS NOT NULL ORDER BY TABLE_NAME, CONSTRAINT_NAME, ORDINAL_POSITION"):
        if r["TABLE_NAME"] in snap:
            fk = snap[r["TABLE_NAME"]]["foreign_keys"].setdefault(r["CONSTRAINT_NAME"], [[], r["REFERENCED_TABLE_NAME"], [], ""])
            fk[0].append(r["COLUMN_NAME"])
            fk[2].append(r["REFERENCED_COLUMN_NAME"])
    for r in _rows(f"SELECT TABLE_NAME, CONSTRAINT_NAME, UPDATE_RULE, DELETE_RULE FROM information_schema.referential_constraints WHERE CONSTRAINT_SCHEMA = '{db}'"):
        fks = snap.get(r["TABLE_NAME"], {}).get("foreign_keys", {})
        if r["CONSTRAINT_NAME"] in fks:
            fks[r["CONSTRAINT_NAME"]][3] = f"ON UPDATE {r.get('UPDATE_RULE', '')} ON DELETE {r.get('DELETE_RULE', '')}"
    return snap


def diff_schema_report(database_a: str, database_b: str) -> dict:
    for name in (database_a, database_b):
        if not _DB_NAME_RE.match(name):
            raise ValueError(f"{name!r} is not a database name.")
    a, b = _schema_snapshot(database_a), _schema_snapshot(database_b)
    changed: dict = {}
    for table in sorted(set(a) & set(b)):
        diffs: dict = {}
        ca, cb = a[table]["columns"], b[table]["columns"]
        added = [c for c in cb if c not in ca]
        removed = [c for c in ca if c not in cb]
        altered = {c: {database_a: ca[c], database_b: cb[c]} for c in ca if c in cb and ca[c] != cb[c]}
        if added: diffs["columns_only_in_" + database_b] = added
        if removed: diffs["columns_only_in_" + database_a] = removed
        if altered: diffs["columns_that_differ"] = altered
        for key, label in (("indexes", "indexes"), ("foreign_keys", "foreign_keys")):
            ka, kb = a[table][key], b[table][key]
            only_a = [k for k in ka if k not in kb]
            only_b = [k for k in kb if k not in ka]
            differ = {k: {database_a: ka[k], database_b: kb[k]} for k in ka if k in kb and ka[k] != kb[k]}
            if only_a: diffs[f"{label}_only_in_{database_a}"] = only_a
            if only_b: diffs[f"{label}_only_in_{database_b}"] = only_b
            if differ: diffs[f"{label}_that_differ"] = differ
        if diffs:
            changed[table] = diffs
    only_a_tables = sorted(set(a) - set(b))
    only_b_tables = sorted(set(b) - set(a))
    return {"identical": not (changed or only_a_tables or only_b_tables), "tables_only_in_" + database_a: only_a_tables, "tables_only_in_" + database_b: only_b_tables,
            "tables_that_differ": changed, "compared": {database_a: len(a), database_b: len(b)},
            "note": "Column entries are [type, nullable, default, extra]; indexes [unique, columns]; foreign keys [columns, parent table, parent columns, rules]. SHOW CREATE TABLE gives the DDL of a table."}


@mcp.tool()
def diff_schema(database_a: str, database_b: str) -> str:
    """Compare the structure of two databases: the tables only one of them has, and for the tables both have the columns (type, NULL, default), indexes and foreign keys that
    differ. Reads information_schema only; nothing is changed. The desktop client's "Compare Schema" window also writes the migration SQL."""
    try:
        return json.dumps(diff_schema_report(database_a, database_b), ensure_ascii=False)
    except Exception as e:
        return _fail(e)


# resources: what a client can read without calling a tool (the databases, the tables of one with their columns); prompts: ready-made requests that use the tools above
@mcp.resource("rusql://databases", mime_type="application/json")
def databases_resource() -> str:
    """The databases of the server."""
    return json.dumps(_rows("SHOW DATABASES"), ensure_ascii=False)


@mcp.resource("rusql://schema/{database}", mime_type="application/json")
def schema_resource(database: str) -> str:
    """The tables of a database with their columns (name, type, NULL), as JSON."""
    if not _DB_NAME_RE.match(database):
        return json.dumps({"error": f"{database!r} is not a database name."})
    tables: dict = {}
    for r in _rows(f"SELECT TABLE_NAME, COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_KEY FROM information_schema.columns WHERE TABLE_SCHEMA = '{database}' ORDER BY TABLE_NAME, ORDINAL_POSITION"):
        tables.setdefault(r["TABLE_NAME"], []).append({"name": r["COLUMN_NAME"], "type": r.get("COLUMN_TYPE", ""), "nullable": r.get("IS_NULLABLE") == "YES", "key": r.get("COLUMN_KEY", "")})
    return json.dumps({"database": database, "tables": tables}, ensure_ascii=False)


@mcp.prompt()
def optimize_query(sql: str, database: str = "") -> str:
    """Find out why a query is slow and which index would help."""
    return (f"The query below is slow on RuSQL (database: {database or 'the current one'}). 1. Call explain_query to see the plan and where the time goes. 2. Call suggest_indexes "
            f"with verify=True to try the suggested indexes and see the speedup. 3. Tell me what you found, and only after I say yes create the index with execute_sql.\n\n{sql}")


@mcp.prompt()
def find_slow_queries() -> str:
    """Look for the statements that are slow lately and fix the worst one."""
    return ("Call slow_queries. If the slow query log is off, tell me and ask whether to turn it on. Otherwise take the statement that took the most time, call suggest_indexes "
            "with verify=True, and explain the result. Do not create anything before I agree.")


@mcp.prompt()
def explain_schema(database: str) -> str:
    """Describe a database: its tables, how they relate, what each is for."""
    return (f"Read the resource rusql://schema/{database} (or call list_tables and get_table_schema) and get_fk_graph for {database}. Describe in a few sentences what the "
            f"database holds, how the tables relate through foreign keys, and anything odd (a table without a primary key, a column that looks unused).")


# ─── 저장된 연결(Connections) 관리 ─────────────────────────────
# code/data/connections.json 파일을 RuSQL UI(main.rs의 get_connections/save_connections)와
# 그대로 공유 — 이 파일이 두 프로세스 사이의 유일한 다리다(웹뷰 localStorage는 이 프로세스가
# 접근할 방법이 없음). UI가 홈 화면(로그인 전)에서 3초마다 이 파일을 다시 읽으므로, 여기서
# 추가/삭제하면 앱을 재시작하지 않아도 잠시 후 화면에 반영된다.
_CONNECTIONS_FILE = Path(__file__).resolve().parent.parent / "data" / "connections.json"


def _load_connections() -> list[dict]:
    try:
        return json.loads(_CONNECTIONS_FILE.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        return []


def _save_connections(connections: list[dict]) -> None:
    _CONNECTIONS_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = _CONNECTIONS_FILE.with_suffix(".json.tmp")
    tmp_path.write_text(json.dumps(connections, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp_path.replace(_CONNECTIONS_FILE)  # 원자적 교체 — UI가 절반만 쓰인 JSON을 읽는 일이 없게


@mcp.tool()
def list_connections() -> str:
    """List all connections saved in the RuSQL UI's home screen. Returns a JSON array
    with id/name/host/port/user/dataDir (passwords are redacted, never returned)."""
    redacted = [{k: v for k, v in c.items() if k != "password"} for c in _load_connections()]
    return json.dumps(redacted, ensure_ascii=False)


@mcp.tool()
def add_connection(name: str, host: str, port: int, user: str, password: str) -> str:
    """Add a new connection to the RuSQL UI's home screen. Ask the user for name, host,
    port, user, and password first if they haven't already given them - do not guess or
    default any of these, especially the password. Appears in the UI within a few seconds
    (no restart needed) if the UI is currently showing its home/connection-list screen."""
    connections = _load_connections()
    conn_id = str(int(time.time() * 1000))
    data_dir = str(_CONNECTIONS_FILE.parent / f"data_{conn_id}")
    connections.append({
        "id": conn_id, "name": name, "host": host, "port": port,
        "user": user, "password": password, "autoLogin": False, "dataDir": data_dir,
    })
    _save_connections(connections)
    return f"Added connection '{name}' (id: {conn_id})."


@mcp.tool()
def delete_connection(id_or_name: str) -> str:
    """Delete a saved connection by its id (preferred, from list_connections) or by exact
    name. If multiple connections share that name, nothing is deleted and the matching ids
    are returned so the caller can retry with a specific id."""
    connections = _load_connections()
    by_id = [c for c in connections if c["id"] == id_or_name]
    if by_id:
        _save_connections([c for c in connections if c["id"] != id_or_name])
        return f"Deleted connection '{by_id[0]['name']}' (id: {id_or_name})."

    by_name = [c for c in connections if c["name"] == id_or_name]
    if not by_name:
        return f"No connection found with id or name '{id_or_name}'."
    if len(by_name) > 1:
        ids = ", ".join(c["id"] for c in by_name)
        return f"{len(by_name)} connections are named '{id_or_name}' (ids: {ids}). Retry delete_connection with a specific id."
    _save_connections([c for c in connections if c["id"] != by_name[0]["id"]])
    return f"Deleted connection '{id_or_name}' (id: {by_name[0]['id']})."


def _find_connection(connections: list[dict], id_or_name: str):
    """id 우선 매칭, 없으면 이름이 유일할 때만 매칭 - delete_connection과 동일한 규칙.
    (연결 없음, 모호함) 둘 다 None을 반환하고 두 번째 값에 사람이 읽을 에러 메시지를 담는다."""
    by_id = [c for c in connections if c["id"] == id_or_name]
    if by_id:
        return by_id[0], None
    by_name = [c for c in connections if c["name"] == id_or_name]
    if not by_name:
        return None, f"No connection found with id or name '{id_or_name}'."
    if len(by_name) > 1:
        ids = ", ".join(c["id"] for c in by_name)
        return None, f"{len(by_name)} connections are named '{id_or_name}' (ids: {ids}). Retry with a specific id."
    return by_name[0], None


@mcp.tool()
def update_connection(id_or_name: str, name: str = "", host: str = "", port: int = 0,
                       user: str = "", password: str = "", auto_login: bool | None = None) -> str:
    """Update fields of an existing saved connection (matched by id, preferred, or by
    exact unique name). Only pass the fields you want to change - everything else is left
    as-is. Ask the user to confirm before changing host/port/user/password on a connection
    that isn't obviously a scratch/test one."""
    connections = _load_connections()
    conn, err = _find_connection(connections, id_or_name)
    if err:
        return err
    if name:
        conn["name"] = name
    if host:
        conn["host"] = host
    if port:
        conn["port"] = port
    if user:
        conn["user"] = user
    if password:
        conn["password"] = password
    if auto_login is not None:
        conn["autoLogin"] = auto_login
    _save_connections(connections)
    return f"Updated connection '{conn['name']}' (id: {conn['id']})."


# ─── UI 조작 (에디터/탭/쿼리 실행) ───────────────────────────────
# code/data/ui_commands.json을 통한 큐. 여기서 "pending" 항목을 넣으면 RuSQL 앱의
# 백그라운드 스레드(main.rs)가 그걸 집어 "ui-command" Tauri 이벤트로 프런트에 보내고,
# 프런트가 실제로 화면을 조작한 뒤 결과를 같은 항목에 채워 "done"으로 바꾼다 - 그걸
# 여기서 잠깐 폴링해 기다렸다가 돌려준다. Phase 17에서 제거됐던 UI 제어 도구들과
# 겉모습은 비슷하지만, 그때는 응답하는 쪽이 아예 없는 죽은 프로토콜이었고 이번엔
# main.rs 스레드 + App.tsx의 uiCmdHandlerRef가 실제로 응답한다.
_UI_COMMANDS_FILE = Path(__file__).resolve().parent.parent / "data" / "ui_commands.json"
_UI_COMMAND_TIMEOUT_SEC = 30.0
_UI_COMMAND_POLL_SEC = 0.2
_MAX_UI_COMMAND_TIMEOUT_SEC = 600.0  # execute_in_editor의 timeout_seconds 상한


def _load_ui_commands() -> list[dict]:
    try:
        return json.loads(_UI_COMMANDS_FILE.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        return []


def _save_ui_commands(cmds: list[dict]) -> None:
    _UI_COMMANDS_FILE.parent.mkdir(parents=True, exist_ok=True)
    # 이 파일은 Rust 배경 스레드(main.rs)와 이 프로세스 양쪽이 자주 쓴다. 고정된
    # 이름의 임시 파일을 공유하면 두 쓰기가 겹칠 때 한쪽의 replace가 상대가 이미
    # 없애버린 파일을 찾다 FileNotFoundError 나는 경합이 생기므로(동시 호출 2개로
    # 실제 재현됨), 쓰기마다 고유한 임시 파일명을 쓴다.
    tmp_path = _UI_COMMANDS_FILE.with_suffix(f".json.tmp.{os.getpid()}.{threading.get_ident()}.{time.time_ns()}")
    tmp_path.write_text(json.dumps(cmds, ensure_ascii=False, indent=2), encoding="utf-8")
    # Windows는 다른 프로세스/스레드가 대상 파일을 잠깐 열어둔 순간에 os.replace가
    # ERROR_SHARING_VIOLATION으로 실패할 수 있음 (POSIX rename과 달리) — 몇 번 짧게
    # 재시도해서 흡수한다.
    for attempt in range(10):
        try:
            tmp_path.replace(_UI_COMMANDS_FILE)
            return
        except PermissionError:
            if attempt == 9:
                raise
            time.sleep(0.03)


def _ui_lock_path() -> "Path":
    return _UI_COMMANDS_FILE.with_suffix(".json.lock")


class _UiCommandsLock:
    """code/data/ui_commands.json에 대한 파일 기반 상호 배제 락.

    Rust 배경 스레드(main.rs)와 이 프로세스(및 그 안의 동시 도구 호출들) 양쪽이 이
    파일에 "읽고 - 고치고 - 통째로 다시 쓰기"를 한다. 락 없이 이 read-modify-write를
    하면, 두 호출이 겹칠 때 나중에 쓰는 쪽이 상대가 방금 큐에 넣은 항목을 못 본 채
    자기 스냅샷으로 덮어써 그 항목이 통째로 사라질 수 있다(동시 MCP 도구 호출 2개로
    실제 재현: 하나는 처리됐지만 다른 하나는 큐에서 사라져 자기 타임아웃까지 응답
    없이 멈춰있었음). O_CREAT|O_EXCL로 만드는 락 파일 자체를 뮤텍스로 쓰며, Rust
    쪽도 (main.rs의 UiCommandsLock) 동일한 파일명 규칙으로 잠근다."""

    def __enter__(self):
        path = _ui_lock_path()
        deadline = time.time() + 5.0
        while True:
            try:
                fd = os.open(str(path), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.close(fd)
                return self
            except FileExistsError:
                if time.time() > deadline:
                    # 락을 쥔 프로세스가 죽어서 남은 파일일 수 있으니 정리 후 재시도
                    try:
                        path.unlink()
                    except OSError:
                        pass
                    deadline = time.time() + 5.0
                time.sleep(0.01)

    def __exit__(self, exc_type, exc, tb):
        try:
            _ui_lock_path().unlink()
        except OSError:
            pass
        return False


def _send_ui_command(action: str, params, timeout: float = _UI_COMMAND_TIMEOUT_SEC, instance: str = "") -> str:
    cmd_id = str(int(time.time() * 1_000_000))
    with _UiCommandsLock():
        cmds = [c for c in _load_ui_commands() if c.get("status") in ("pending", "in_progress")]
        cmds.append({
            "id": cmd_id, "action": action, "params": params, "status": "pending", "result": None,
            "target_instance": instance,
        })
        _save_ui_commands(cmds)

    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(_UI_COMMAND_POLL_SEC)
        entry = next((c for c in _load_ui_commands() if c["id"] == cmd_id), None)
        if entry and entry.get("status") == "done":
            with _UiCommandsLock():
                _save_ui_commands([c for c in _load_ui_commands() if c["id"] != cmd_id])
            return entry.get("result") or ""
    # 타임아웃 시 이 항목을 큐에서 지운다 — 안 지우면 앱이 나중에 다시 켜졌을 때
    # 이미 포기한 이 호출을 뒤늦게 처리해버리거나, 큐가 계속 불어날 수 있음.
    with _UiCommandsLock():
        _save_ui_commands([c for c in _load_ui_commands() if c["id"] != cmd_id])
    return "Error: timed out waiting for the RuSQL app to respond. Is it running and logged in?"


@mcp.tool()
def write_to_editor(query: str, tab: str = "", instance: str = "") -> str:
    """Write SQL text into the RuSQL query editor, replacing the current content of the
    given tab (or the currently active tab if `tab` is omitted). This only fills the
    editor - it does not run the query; use execute_in_editor for that. Requires the
    RuSQL app to be open and logged in. If more than one window is open, pass `instance`
    (from list_app_instances) to target a specific one."""
    params = {"content": query, "tab": tab} if tab else {"content": query}
    return _send_ui_command("write_to_editor", params, instance=instance)


@mcp.tool()
def new_editor_tab(name: str = "", query: str = "", instance: str = "") -> str:
    """Open a new query tab in the RuSQL editor, optionally pre-filled with SQL and a
    custom tab name. Requires the RuSQL app to be open and logged in. If more than one
    window is open, pass `instance` (from list_app_instances) to target a specific one."""
    params = {}
    if name:
        params["name"] = name
    if query:
        params["query"] = query
    return _send_ui_command("new_tab", params, instance=instance)


@mcp.tool()
def close_editor_tab(tab: str, instance: str = "") -> str:
    """Close the RuSQL editor tab with this exact name. Requires the RuSQL app to be
    open and logged in. If more than one window is open, pass `instance` (from
    list_app_instances) to target a specific one."""
    return _send_ui_command("close_tab", tab, instance=instance)


@mcp.tool()
def switch_editor_tab(tab: str, instance: str = "") -> str:
    """Switch focus to the RuSQL editor tab with this exact name. Requires the RuSQL
    app to be open and logged in. If more than one window is open, pass `instance` (from
    list_app_instances) to target a specific one."""
    return _send_ui_command("switch_to_tab", tab, instance=instance)


@mcp.tool()
def list_editor_tabs(instance: str = "") -> str:
    """List the names of all open tabs in the RuSQL editor, in order. Returns a JSON
    array. Requires the RuSQL app to be open and logged in. If more than one window is
    open, pass `instance` (from list_app_instances) to target a specific one."""
    return _send_ui_command("list_tabs", "", instance=instance)


@mcp.tool()
def get_editor_tab_content(tab: str = "", instance: str = "") -> str:
    """Get the current SQL text in the given RuSQL editor tab (or the active tab if
    `tab` is omitted). Requires the RuSQL app to be open and logged in. If more than one
    window is open, pass `instance` (from list_app_instances) to target a specific one."""
    return _send_ui_command("get_tab_content", tab, instance=instance)


@mcp.tool()
def execute_in_editor(query: str = "", tab: str = "", timeout_seconds: float = 30.0, instance: str = "") -> str:
    """Run a query in the RuSQL editor UI itself (as if the user clicked Run), in the
    given tab or the active tab if omitted. If `query` is given it replaces that tab's
    content first, otherwise whatever is already in the tab is run as-is. Returns the
    result as JSON. Unlike execute_sql (which runs invisibly against the engine), this
    drives the real UI so the user sees the query and its result appear on screen.
    Requires the RuSQL app to be open and logged in. If more than one window is open, pass
    `instance` (from list_app_instances) to target a specific one.

    If you expect the query to be slow (large scan, big JOIN, etc.), raise
    `timeout_seconds` (up to 600) - the default 30s only bounds how long this call waits
    for a result, not the query itself, but a timeout before the query finishes means
    the result never gets reported back even though the UI keeps running it.

    If `query` is a DROP/TRUNCATE or a WHERE-less UPDATE/DELETE, this is refused - ask the
    user to confirm, then call confirm_dangerous_sql(sql, via_editor=True, tab=...). Running
    whatever is already in the tab (leaving `query` empty) is never refused - that's SQL the
    user already typed themselves, not something being injected here."""
    if query:
        reason = _dangerous_sql_reason(query)
        if reason:
            return (f"Refused: {reason} Ask the user to explicitly confirm this is intended, "
                    f"then call confirm_dangerous_sql(sql, via_editor=True, tab=...) to run it.")
    params = {}
    if tab:
        params["tab"] = tab
    if query:
        params["query"] = query
    timeout = max(1.0, min(timeout_seconds, _MAX_UI_COMMAND_TIMEOUT_SEC))
    return _send_ui_command("execute_in_editor", params, timeout=timeout, instance=instance)


# ─── 다중 앱 인스턴스 ────────────────────────────────────────────
# RuSQL 창을 여러 개 띄워 서로 다른 DB에 동시 접속해 쓰는 경우를 위해, 각 인스턴스가
# code/data/app_instances.json에 자기 존재를 주기적으로(main.rs의 배경 스레드, ~2초
# 간격) 기록한다. 위의 모든 UI 제어 도구(write_to_editor 등)는 여기서 얻은 id를
# `instance` 파라미터로 넘기면 그 창만 골라서 조작할 수 있다 - 안 넘기면(기본값) 지금처럼
# 아무 인스턴스나(먼저 집어가는 쪽이) 처리한다.
_APP_INSTANCES_FILE = Path(__file__).resolve().parent.parent / "data" / "app_instances.json"
_INSTANCE_STALE_SEC = 15.0  # main.rs의 STALE_SECS와 동일 — 이보다 오래된 하트비트는 죽은 창


def _load_app_instances() -> list[dict]:
    try:
        raw = json.loads(_APP_INSTANCES_FILE.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        return []
    now = time.time()
    return [i for i in raw if now - i.get("lastHeartbeat", 0) < _INSTANCE_STALE_SEC]


@mcp.tool()
def list_app_instances() -> str:
    """List every currently-running RuSQL app window, each with its instance id, whether
    it's logged in, and which database it's connected to (if any). Returns a JSON array.
    Use an entry's `id` as the `instance` parameter on write_to_editor/new_editor_tab/
    close_editor_tab/switch_editor_tab/list_editor_tabs/get_editor_tab_content/
    execute_in_editor/login to target that specific window when more than one is open -
    most useful when the user has several windows open against different databases."""
    return json.dumps(_load_app_instances(), ensure_ascii=False)


@mcp.tool()
def launch_app() -> str:
    """Launch the RuSQL desktop app if no instance is currently running. Does nothing (and
    says so) if an instance is already open - use list_app_instances to check first if you
    need to know whether this actually did anything. The launched window still needs the
    user (or the login tool, once it's had a few seconds to start up) to log in before any
    editor/tab tools will work."""
    if _load_app_instances():
        return "An instance of the RuSQL app is already running."
    if not RUSQL_APP_PATH:
        return ("Error: RUSQL_APP_PATH is not set. Use the \"Auto-connect Claude Desktop\" "
                "button in the RuSQL app's AI MCP panel once to configure it.")
    try:
        subprocess.Popen([RUSQL_APP_PATH])
    except OSError as e:
        return f"Error: failed to launch '{RUSQL_APP_PATH}': {e}"
    return "Launched the RuSQL app. Give it a few seconds to start before calling login."


@mcp.tool()
def login(connection: str, instance: str = "") -> str:
    """Log a running (but not yet logged-in) RuSQL app window into a saved connection, by
    the connection's id or exact unique name (see list_connections). If more than one app
    window is open, pass `instance` (from list_app_instances) to pick which one - otherwise
    whichever window is idle on its home screen handles it. Does nothing useful if that
    window is already logged in (use list_app_instances to check, or just try it and read
    the error). The connection must already have a saved password - login can't prompt the
    user for one."""
    params = {"connection": connection}
    return _send_ui_command("login", params, timeout=15.0, instance=instance)


@mcp.tool()
def start_server(port: int = 0, mysql_port: int = 0, instance: str = "") -> str:
    """Start the RuSQL Server Manager's public listener (native + optional MySQL wire
    protocol) for the current session, so other clients (mysql CLI, another app, etc.) can
    connect to the same data this window is logged into. Omit port/mysql_port to reuse
    whatever is set in that window's Server Manager tab (mysql_port 0 disables the MySQL
    protocol). Requires that window to already be logged in - this does not start the
    underlying database engine itself (that already happens automatically at login)."""
    params = {}
    if port:
        params["port"] = port
    if mysql_port:
        params["mysqlPort"] = mysql_port
    return _send_ui_command("start_server", params, timeout=15.0, instance=instance)


@mcp.tool()
def stop_server(instance: str = "") -> str:
    """Stop the RuSQL Server Manager's public listener started by start_server. Does not
    log the window out or stop the underlying query engine - only the extra public
    listener for other clients."""
    return _send_ui_command("stop_server", "", timeout=15.0, instance=instance)


@mcp.tool()
def get_server_status(instance: str = "") -> str:
    """Get the RuSQL Server Manager's current status for the given window (or any window if
    `instance` is omitted): whether the public listener is running, its port, connected
    client count, recent activity log, and session list. Returns JSON."""
    return _send_ui_command("get_server_status", "", timeout=10.0, instance=instance)


if __name__ == "__main__":
    mcp.run()
