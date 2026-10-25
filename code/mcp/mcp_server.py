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
    Useful for diagnosing slow queries, checking index usage, and estimating row counts."""
    explain_sql = sql.strip()
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
# Reads the query (a heuristic reading with regular expressions, there is no SQL parser in this process), the columns and indexes of its tables (information_schema),
# the shape of the data (ANALYZE TABLE: how many distinct values each column has) and, for a SELECT, the plan the engine runs (EXPLAIN ANALYZE), and says which
# index would let the query stop scanning. It never creates anything: the answer holds the CREATE INDEX statements for the user to run.
_SQL_WORDS = {"WHERE", "ON", "JOIN", "LEFT", "RIGHT", "INNER", "OUTER", "CROSS", "FULL", "NATURAL", "GROUP", "ORDER", "LIMIT", "HAVING", "UNION", "SET", "USING",
              "VALUES", "SELECT", "FROM", "AS", "AND", "OR", "NOT", "NULL", "TRUE", "FALSE", "DISTINCT", "OFFSET", "FOR", "WINDOW", "LATERAL", "STRAIGHT_JOIN"}
_CLAUSE_END = r"(?=\bWHERE\b|\bGROUP\s+BY\b|\bORDER\s+BY\b|\bHAVING\b|\bLIMIT\b|\bUNION\b|\bFOR\s+UPDATE\b|\bWINDOW\b|$)"
_IDENT = r"`?[A-Za-z_][\w$]*`?(?:\.`?[A-Za-z_][\w$]*`?){0,2}"
_SMALL_TABLE_ROWS = 100       # a scan of fewer rows is cheap: no index suggested
_NOT_NARROWING = 0.2          # an index that leaves more than this share of the table to read does not help


def _strip_sql(sql: str) -> tuple[str, list[str]]:
    """The statement without comments, strings and extra blanks; and the LIKE patterns, which are strings the answer depends on."""
    like_patterns = re.findall(r"\bLIKE\s+'((?:[^']|'')*)'", sql, flags=re.IGNORECASE)
    s = re.sub(r"/\*.*?\*/", " ", sql, flags=re.S)
    s = re.sub(r"--[^\n]*", " ", s)
    s = re.sub(r"'(?:[^']|'')*'", "''", s)
    s = re.sub(r'"(?:[^"]|"")*"', '""', s)
    return re.sub(r"\s+", " ", s).strip().rstrip(";").strip(), like_patterns


def _bare(name: str) -> str:
    return name.replace("`", "").split(".")[-1]


def _query_tables(stripped: str) -> list[tuple[str, str]]:
    """(table, alias) of every table the statement names (FROM, JOIN, comma lists, UPDATE, DELETE FROM); the alias is the table's own name when it has none."""
    found: list[tuple[str, str]] = []

    def add(table: str, alias: str | None):
        alias = alias if alias and alias.upper() not in _SQL_WORDS else None
        entry = (_bare(table), _bare(alias) if alias else _bare(table))
        if entry not in found:
            found.append(entry)

    for m in re.finditer(rf"\b(?:JOIN|UPDATE|DELETE\s+FROM)\s+({_IDENT})(?:\s+(?:AS\s+)?([A-Za-z_]\w*))?", stripped, flags=re.IGNORECASE):
        add(m.group(1), m.group(2))
    for m in re.finditer(rf"\bFROM\s+(.+?){_CLAUSE_END}", stripped, flags=re.IGNORECASE):
        listing = re.split(r"\b(?:INNER|LEFT|RIGHT|FULL|CROSS|NATURAL)?\s*(?:OUTER\s+)?JOIN\b", m.group(1), flags=re.IGNORECASE)
        for k, part in enumerate(listing):
            part = re.sub(r"\bON\b.*$", "", part, flags=re.IGNORECASE).strip()
            for piece in part.split(","):
                piece = piece.strip().strip("()")
                if piece.upper().startswith("SELECT"):
                    continue
                pm = re.match(rf"({_IDENT})(?:\s+(?:AS\s+)?([A-Za-z_]\w*))?$", piece)
                if pm:
                    add(pm.group(1), pm.group(2))
    return found


def _clause(stripped: str, keyword: str) -> str:
    m = re.search(rf"\b{keyword}\b\s+(.+?){_CLAUSE_END}", stripped, flags=re.IGNORECASE)
    return m.group(1).strip() if m else ""


def _join_conditions(stripped: str) -> list[str]:
    return [m.group(1).strip() for m in re.finditer(r"\bON\b\s+(.+?)(?=\b(?:INNER|LEFT|RIGHT|FULL|CROSS|NATURAL)?\s*(?:OUTER\s+)?JOIN\b|\bWHERE\b|\bGROUP\s+BY\b|\bORDER\s+BY\b|\bHAVING\b|\bLIMIT\b|$)", stripped, flags=re.IGNORECASE)]


def _conjuncts(clause: str) -> list[str]:
    """The AND-ed tests of a WHERE / ON (an AND inside BETWEEN or inside parentheses is not a split)."""
    text = re.sub(r"(\bBETWEEN\s+\S+)\s+AND\s+", r"\1 __AND__ ", clause, flags=re.IGNORECASE)
    parts, depth, cur = [], 0, ""
    for token in re.split(r"(\s+AND\s+|\(|\))", text, flags=re.IGNORECASE):
        if token == "(":
            depth += 1
        elif token == ")":
            depth -= 1
        if depth == 0 and re.fullmatch(r"\s+AND\s+", token, flags=re.IGNORECASE):
            parts.append(cur.strip())
            cur = ""
        else:
            cur += token
    parts.append(cur.strip())
    return [p.replace("__AND__", "AND") for p in parts if p]


def _read_predicate(test: str, like_patterns: list[str]) -> dict:
    """One test of a WHERE: {"kind": eq | in | range | like_prefix | like_any | func | or | other, "columns": [names as written]}."""
    t = test.strip()
    while t.startswith("(") and t.endswith(")") and t.count("(") == t.count(")"):
        t = t[1:-1].strip()
    columns = [c for c in re.findall(_IDENT, t) if c.upper() not in _SQL_WORDS]
    if re.search(r"\bOR\b", t, flags=re.IGNORECASE):
        return {"kind": "or", "columns": columns}
    m = re.match(rf"^({_IDENT})\s*(<=>|<>|!=|>=|<=|=|>|<)\s*(.+)$", t)
    if m:
        left, op, right = m.group(1), m.group(2), m.group(3).strip()
        right_is_column = re.fullmatch(_IDENT, right) is not None and right.upper() not in _SQL_WORDS
        if right_is_column and op == "=":
            return {"kind": "join", "columns": [left, right]}
        if op in ("<>", "!="):
            return {"kind": "other", "columns": [left]}
        return {"kind": "eq" if op in ("=", "<=>") else "range", "columns": [left]}
    m = re.match(rf"^({_IDENT})\s+(NOT\s+)?(IN|BETWEEN|LIKE|IS)\b(.*)$", t, flags=re.IGNORECASE)
    if m:
        left, negated, word, rest = m.group(1), bool(m.group(2)), m.group(3).upper(), m.group(4).strip()
        if negated:
            return {"kind": "other", "columns": [left]}
        if word == "IN":
            return {"kind": "in", "columns": [left]}
        if word == "BETWEEN":
            return {"kind": "range", "columns": [left]}
        if word == "IS":
            return {"kind": "eq" if re.match(r"^NULL\b", rest, flags=re.IGNORECASE) else "other", "columns": [left]}
        pattern = like_patterns.pop(0) if like_patterns else ""
        return {"kind": "like_prefix" if pattern and pattern[0] not in "%_" else "like_any", "columns": [left]}
    return {"kind": "func" if columns else "other", "columns": columns}


def _order_items(stripped: str, keyword: str) -> list[tuple[str, str]]:
    items = []
    for piece in _clause(stripped, keyword).split(","):
        m = re.match(rf"^\s*({_IDENT})(?:\s+(ASC|DESC))?\s*$", piece, flags=re.IGNORECASE)
        if m:
            items.append((m.group(1), (m.group(2) or "ASC").upper()))
        else:
            break  # an expression or a position ends what an index can serve
    return items


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


def _plan_summary(sql: str, database: str) -> dict:
    raw = _run(f"EXPLAIN ANALYZE {sql}", database)
    if raw.strip().upper().startswith(("ERR", "ERROR")):
        return {"error": " ".join(l for l in raw.splitlines() if l.strip() and l.strip() not in ("ERR", "---END---"))[:300]}
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


def _covered(columns: list[str], indexes: dict[str, list[str]], rows: int | None, distinct: dict[str, int]) -> str | None:
    """The index that already serves a search on `columns` (its leading columns are those columns), or one on the first column that leaves only a few rows."""
    for name, index in indexes.items():
        k = len(columns)
        if index[:k] == columns or set(index[:k]) == set(columns):
            return name
    if rows:
        for name, index in indexes.items():
            if index and index[0] in columns and distinct.get(index[0]) and rows / distinct[index[0]] <= 50:
                return name
    return None


def suggest_indexes_for(sql: str, database: str = "") -> dict:
    stripped, like_patterns = _strip_sql(sql)
    kind = (stripped.split(" ", 1)[0] if stripped else "").upper()
    if kind not in ("SELECT", "UPDATE", "DELETE", "WITH"):
        return {"error": "suggest_indexes reads a SELECT, UPDATE or DELETE statement."}
    tables = _query_tables(stripped)
    if not tables:
        return {"error": "No table found in the statement."}
    facts = {t: _table_facts(t, database) for t in dict.fromkeys(t for t, _ in tables)}
    alias_to_table = {a: t for t, a in tables}
    for t, _ in tables:
        alias_to_table.setdefault(t, t)

    def resolve(name: str) -> tuple[str, str] | None:
        parts = name.replace("`", "").split(".")
        column = parts[-1]
        if len(parts) >= 2:
            table = alias_to_table.get(parts[-2])
            return (table, column) if table and column in facts[table]["columns"] else None
        owners = [t for t in facts if column in facts[t]["columns"]]
        return (owners[0], column) if len(owners) == 1 else None

    # what each table is searched, joined, grouped and sorted by
    eq: dict[str, list[str]] = {t: [] for t in facts}
    rng: dict[str, list[str]] = {t: [] for t in facts}
    joins: dict[str, list[str]] = {t: [] for t in facts}
    notes: list[str] = []
    tests = _conjuncts(_clause(stripped, "WHERE")) + [c for on in _join_conditions(stripped) for c in _conjuncts(on)]
    for test in tests:
        p = _read_predicate(test, like_patterns)
        resolved = [r for r in (resolve(c) for c in p["columns"]) if r]
        if p["kind"] in ("eq", "in"):
            for t, c in resolved:
                if c not in eq[t]:
                    eq[t].append(c)
        elif p["kind"] in ("range", "like_prefix"):
            for t, c in resolved:
                if c not in rng[t]:
                    rng[t].append(c)
        elif p["kind"] == "join":
            for t, c in resolved:
                if c not in joins[t]:
                    joins[t].append(c)
        elif p["kind"] == "func" and resolved:
            notes.append(f"`{test}` wraps a column in an expression or function, so no index can serve it as written (compare the bare column instead).")
        elif p["kind"] == "like_any" and resolved:
            notes.append(f"`{test}` starts with a wildcard, so an index cannot be used for it.")
        elif p["kind"] == "or" and resolved:
            notes.append(f"`{test}` combines columns with OR: one index cannot serve it, a separate index per OR branch may.")
    sorted_by = [(resolve(c), d) for c, d in _order_items(stripped, "ORDER BY")]
    sorted_by = [(r, d) for r, d in sorted_by if r]
    grouped_by = [resolve(c) for c, _ in _order_items(stripped, "GROUP BY")]
    grouped_by = [r for r in grouped_by if r]

    suggestions: list[dict] = []
    already: list[dict] = []

    def consider(table: str, narrowing: list[str], order_columns: list[str], why: str):
        """An index on the columns the statement searches by (`narrowing`) followed by the ones it sorts by (`order_columns`)."""
        f = facts[table]
        rows = f["rows"]
        columns = narrowing + order_columns
        if not columns:
            return
        if rows is not None and rows < _SMALL_TABLE_ROWS:
            notes.append(f"{table} has only {rows} rows: a scan is cheap, no index suggested.")
            return
        have = _covered(columns, f["indexes"], rows, f["distinct"])
        if have:
            already.append({"table": table, "columns": columns, "index": have})
            return
        # how many rows a search reads is only known for equality columns: how much a range keeps is not, so a range column adds no estimate and is never a reason to refuse
        ranged = [c for c in narrowing if c in rng[table] and c not in eq[table]]
        counted = [c for c in narrowing if c not in ranged]
        estimate = None
        if rows and counted and all(c in f["distinct"] for c in counted):
            combined = 1
            for c in counted:
                combined *= max(1, f["distinct"][c])
            estimate = max(1, rows // combined) if combined else rows
            if estimate > rows * _NOT_NARROWING and not ranged:
                notes.append(f"An index on {table}({', '.join(counted)}) would still leave about {estimate} of {rows} rows to read: not suggested.")
                return
        name = "idx_" + table + "_" + "_".join(columns)
        entry = {"table": table, "columns": columns, "create_index": f"CREATE INDEX {name} ON {table} ({', '.join(columns)});", "why": why}
        if estimate is not None:
            entry["rows_read_with_index_estimate"] = estimate
            entry["rows_in_table"] = rows
        suggestions.append(entry)

    single_table = len(facts) == 1
    has_limit = re.search(r"\bLIMIT\b", stripped, flags=re.IGNORECASE) is not None
    for t, f in facts.items():
        narrowing = sorted(eq[t], key=lambda c: -f["distinct"].get(c, 0)) + rng[t][:1]
        order_columns: list[str] = []
        # the rows come out of the index in the order of the ORDER BY when the index is searched by equality only and the whole sort goes one way
        if single_table and not rng[t] and sorted_by and all(r[0] == t for r, _ in sorted_by) and len({d for _, d in sorted_by}) == 1:
            order_columns = [r[1] for r, _ in sorted_by if r[1] not in narrowing]
        parts = []
        if narrowing:
            parts.append("searched by " + ", ".join(f"{c} ({'=' if c in eq[t] else 'range'})" for c in narrowing))
        if order_columns:
            parts.append("sorted by " + ", ".join(order_columns) + " (the rows then come out of the index in order)")
        if narrowing or (order_columns and has_limit):
            consider(t, narrowing, order_columns, " and ".join(parts))
        elif order_columns:
            notes.append(f"An index on {t}({', '.join(order_columns)}) would spare the sort, but without a LIMIT the statement reads every row anyway.")
        if joins[t]:
            consider(t, joins[t][:1], [], f"joined on {joins[t][0]}: the engine looks up matching rows through it")
    if grouped_by and single_table:
        t = next(iter(facts))
        group_columns = [c for r, c in grouped_by if r == t]
        if group_columns and not eq[t] and not rng[t]:
            consider(t, group_columns, [], "grouped by " + ", ".join(group_columns))
    result: dict = {"statement": kind, "tables": {t: {"rows": f["rows"], "indexes": f["indexes"]} for t, f in facts.items()}, "suggestions": suggestions}
    if already:
        result["already_indexed"] = already
    if notes:
        result["notes"] = list(dict.fromkeys(notes))
    if kind in ("SELECT", "WITH"):
        result["plan"] = _plan_summary(sql.strip().rstrip(";"), database)
        slow = max(result["plan"].get("steps", []), key=lambda s: s["share_percent"], default=None)
        if slow and slow["share_percent"] >= 50:
            result["slowest_step"] = f"Step {slow['step']} ({slow['what']}) takes {slow['share_percent']}% of the time"
    else:
        result.setdefault("notes", []).append("The plan was not run: EXPLAIN ANALYZE would execute an UPDATE or DELETE.")
    result["caveat"] = "A heuristic reading of the statement; the row estimates assume the columns are independent. Nothing was created: run a CREATE INDEX yourself and EXPLAIN again to see the plan change."
    return result


@mcp.tool()
def suggest_indexes(sql: str, database: str = "") -> str:
    """Suggest indexes for a SELECT, UPDATE or DELETE: reads the statement, the columns and indexes of its tables, how many distinct values each column has
    (ANALYZE TABLE) and, for a SELECT, the plan the engine runs (EXPLAIN ANALYZE), and returns a JSON object with the CREATE INDEX statements that would let the
    statement stop scanning (equality columns first, then one range column, then ORDER BY), why, how many rows it would read with them, the indexes the tables already
    have, and the reasons an index would not help (a function around a column, a leading wildcard, an OR, a small table). It does not create anything."""
    return json.dumps(suggest_indexes_for(sql, database), ensure_ascii=False)


@mcp.tool()
def sample_data(table: str, n: int = 10, database: str = "") -> str:
    """Return up to N sample rows from a table as a JSON array.
    Useful for understanding data distribution before writing queries."""
    n = max(1, min(n, 100))
    raw = _run(f"SELECT * FROM {table} LIMIT {n}", database)
    rows = _parse_table_output(raw)
    return json.dumps(rows, ensure_ascii=False)


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
