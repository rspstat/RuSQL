"""
RuSQL True MCP Server
stdio transport — Claude Desktop spawns this process and communicates via JSON-RPC.

Claude Desktop config:
  %APPDATA%\\Claude\\claude_desktop_config.json
  → see claude_desktop_config_example.json
"""
import json
import socket
import sys
from mcp.server.fastmcp import FastMCP

RUSTDB_HOST = "127.0.0.1"
RUSTDB_PORT = 7878
RUSTDB_USER = "root"
RUSTDB_PASS = "root"

mcp = FastMCP("RuSQL v2.2.0")


class _Conn:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((RUSTDB_HOST, RUSTDB_PORT))
        self.sock.settimeout(30)
        self._recv()  # welcome banner
        self._raw(f"AUTH {RUSTDB_USER} {RUSTDB_PASS}")

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


def _run_ui(action: str, data: str) -> str:
    """UI 제어 명령을 JSON으로 인코딩해 TCP 서버에 전송 (개행 포함 SQL 지원)."""
    payload = json.dumps({"action": action, "data": data}, ensure_ascii=False)
    command = f"UI:{payload}"
    print(f"[mcp] _run_ui: {command[:80]!r}", file=sys.stderr, flush=True)
    try:
        c = _Conn()
        try:
            return c._raw(command) or "OK"
        finally:
            c.close()
    except ConnectionRefusedError:
        return f"Error: RuSQL is not running on {RUSTDB_HOST}:{RUSTDB_PORT}. Start the server first."
    except Exception as e:
        return f"Error: {e}"


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
        msg = f"Error: RuSQL is not running on {RUSTDB_HOST}:{RUSTDB_PORT}. Start the server first."
        print(f"[mcp] {msg}", file=sys.stderr, flush=True)
        return msg
    except Exception as e:
        msg = f"Error: {e}"
        print(f"[mcp] {msg}", file=sys.stderr, flush=True)
        return msg


def _parse_table_output(text: str) -> list[dict]:
    """탭 구분 RuSQL 출력을 JSON 배열로 변환."""
    lines = [l for l in text.splitlines() if l.strip()]
    if not lines:
        return []
    # 헤더 행 탐지: 첫 번째 비어있지 않은 줄
    header_line = lines[0]
    if "\t" not in header_line:
        return [{"result": text}]
    headers = [h.strip() for h in header_line.split("\t")]
    rows = []
    for line in lines[1:]:
        if line.startswith("(") and line.endswith(")"):  # "(N rows returned.)" 무시
            continue
        parts = [p.strip() for p in line.split("\t")]
        if len(parts) == len(headers):
            rows.append(dict(zip(headers, parts)))
    return rows


@mcp.tool()
def execute_sql(sql: str, database: str = "") -> str:
    """Execute any SQL query on RuSQL. Returns a JSON array of row objects for SELECT,
    or a plain status message for DDL/DML. Optionally specify a database to USE before executing.

    IMPORTANT — RuSQL is a custom engine. The following MySQL features are NOT supported:
    - AUTO_INCREMENT (use INTEGER PRIMARY KEY without it; the engine auto-assigns IDs)
    - ENUM(...) types (use VARCHAR instead)
    - TINYINT, SMALLINT (use INT)
    - BOOLEAN (use INT with 0/1)
    - CHECK constraints
    - FOREIGN KEY constraints
    - CURDATE(), NOW(), DATEDIFF(), DATE_ADD(), DATE_SUB()
    - IF(cond, a, b) expression
    - NOT EXISTS / EXISTS subqueries
    - SUM(boolean_expression) — e.g. SUM(col IS NULL)
    - Multi-table UPDATE / DELETE
    Always write SQL that avoids the above. Use only: INT, BIGINT, FLOAT, DOUBLE, VARCHAR(n), TEXT, DATE, DATETIME."""
    raw = _run(sql, database)
    # DDL 실행 후 UI 사이드바 갱신
    DDL_KEYWORDS = ("CREATE ", "DROP ", "ALTER ", "RENAME ", "USE ")
    if any(sql.strip().upper().startswith(k) for k in DDL_KEYWORDS):
        _run_ui("refresh_sidebar", "")
    # SELECT 계열 결과는 JSON 배열로 변환
    stripped = raw.strip()
    if "\t" in stripped and not stripped.startswith("ERR") and not stripped.startswith("OK"):
        rows = _parse_table_output(stripped)
        if rows and not (len(rows) == 1 and "result" in rows[0]):
            return json.dumps(rows, ensure_ascii=False)
    return raw


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


@mcp.tool()
def sample_data(table: str, n: int = 10, database: str = "") -> str:
    """Return up to N sample rows from a table as a JSON array.
    Useful for understanding data distribution before writing queries."""
    n = max(1, min(n, 100))
    raw = _run(f"SELECT * FROM {table} LIMIT {n}", database)
    rows = _parse_table_output(raw)
    return json.dumps(rows, ensure_ascii=False)


@mcp.tool()
def write_to_editor(query: str) -> str:
    """Write a SQL query into the RuSQL UI editor (active tab) without executing it.
    The user can review and run it manually. Supports multi-line SQL."""
    return _run_ui("write_to_editor", query)


@mcp.tool()
def open_new_tab(query: str, tab_name: str = "query.sql") -> str:
    """Open a new tab in the RuSQL UI editor with the given SQL query pre-filled.
    Use tab_name to give the tab a meaningful name (e.g. 'report.sql')."""
    data = json.dumps({"name": tab_name, "query": query}, ensure_ascii=False)
    return _run_ui("new_tab", data)


@mcp.tool()
def execute_in_editor(query: str) -> str:
    """Write a SQL query to the RuSQL UI editor and execute it immediately.
    Results appear in the UI result panel. Supports multi-line SQL."""
    return _run_ui("execute_in_editor", query)


@mcp.tool()
def close_tab(tab_name: str) -> str:
    """Close a tab in the RuSQL UI editor by its name (e.g. 'query1.sql').
    The last remaining tab cannot be closed."""
    return _run_ui("close_tab", tab_name)


@mcp.tool()
def get_tab_content(tab_name: str = "query.sql") -> str:
    """Get the current SQL content of a specific tab in the RuSQL UI editor.
    Use this to read what the user has written before executing it."""
    return _run_ui("get_tab_content", tab_name)


@mcp.tool()
def list_tabs() -> str:
    """List all currently open tabs in the RuSQL UI editor.
    Returns one tab name per line. Use this before switch_to_tab or get_tab_content."""
    return _run_ui("list_tabs", "")


@mcp.tool()
def switch_to_tab(tab_name: str) -> str:
    """Switch focus to a specific tab in the RuSQL UI editor by name (e.g. 'query1.sql')."""
    return _run_ui("switch_to_tab", tab_name)


@mcp.tool()
def get_query_result() -> str:
    """Get the result of the last executed query from the RuSQL UI editor.
    Returns tab-separated rows with a header line. Use this to read and analyze results."""
    raw = _run_ui("get_query_result", "")
    stripped = raw.strip()
    if "\t" in stripped and not stripped.startswith("ERR"):
        rows = _parse_table_output(stripped)
        if rows and not (len(rows) == 1 and "result" in rows[0]):
            return json.dumps(rows, ensure_ascii=False)
    return raw


@mcp.tool()
def get_current_database() -> str:
    """Get the name of the currently selected database in the RuSQL UI session."""
    return _run_ui("get_current_database", "")


if __name__ == "__main__":
    mcp.run()
