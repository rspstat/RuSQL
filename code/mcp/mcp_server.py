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
        headers = [c.strip() for c in content_lines[0].strip("|").split("|")]
        rows = []
        for line in content_lines[1:]:
            cells = [c.strip() for c in line.strip("|").split("|")]
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


@mcp.tool()
def execute_sql(sql: str, database: str = "") -> str:
    """Execute any SQL query on RuSQL. Returns a JSON array of row objects for SELECT,
    or a plain status message for DDL/DML. Optionally specify a database to USE before executing.

    RuSQL is a MySQL-compatible custom engine with broad feature support, including
    AUTO_INCREMENT, ENUM, TINYINT/SMALLINT, BOOLEAN, CHECK constraints, FOREIGN KEY
    constraints, date functions (CURDATE/NOW/DATEDIFF/DATE_ADD/DATE_SUB/...), IF(cond, a, b),
    EXISTS/NOT EXISTS subqueries, and multi-table UPDATE/DELETE (`UPDATE t1, t2 SET ...`,
    `DELETE t1, t2 FROM t1 JOIN t2 ON ...`). See docs/mds/FUNCTIONS.md in the repo for the
    full feature list."""
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


# ─── UI 조작 (에디터/탭/쿼리 실행) ───────────────────────────────
# code/data/ui_commands.json을 통한 큐. 여기서 "pending" 항목을 넣으면 RuSQL 앱의
# 백그라운드 스레드(main.rs)가 그걸 집어 "ui-command" Tauri 이벤트로 프런트에 보내고,
# 프런트가 실제로 화면을 조작한 뒤 결과를 같은 항목에 채워 "done"으로 바꾼다 - 그걸
# 여기서 잠깐 폴링해 기다렸다가 돌려준다. Phase 17에서 제거됐던 UI 제어 도구들과
# 겉모습은 비슷하지만, 그때는 응답하는 쪽이 아예 없는 죽은 프로토콜이었고 이번엔
# main.rs 스레드 + App.tsx의 uiCmdHandlerRef가 실제로 응답한다.
_UI_COMMANDS_FILE = Path(__file__).resolve().parent.parent / "data" / "ui_commands.json"
_UI_COMMAND_TIMEOUT_SEC = 20.0
_UI_COMMAND_POLL_SEC = 0.2


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


def _send_ui_command(action: str, params) -> str:
    cmd_id = str(int(time.time() * 1_000_000))
    with _UiCommandsLock():
        cmds = [c for c in _load_ui_commands() if c.get("status") in ("pending", "in_progress")]
        cmds.append({"id": cmd_id, "action": action, "params": params, "status": "pending", "result": None})
        _save_ui_commands(cmds)

    deadline = time.time() + _UI_COMMAND_TIMEOUT_SEC
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
def write_to_editor(query: str, tab: str = "") -> str:
    """Write SQL text into the RuSQL query editor, replacing the current content of the
    given tab (or the currently active tab if `tab` is omitted). This only fills the
    editor - it does not run the query; use execute_in_editor for that. Requires the
    RuSQL app to be open and logged in."""
    params = {"content": query, "tab": tab} if tab else {"content": query}
    return _send_ui_command("write_to_editor", params)


@mcp.tool()
def new_editor_tab(name: str = "", query: str = "") -> str:
    """Open a new query tab in the RuSQL editor, optionally pre-filled with SQL and a
    custom tab name. Requires the RuSQL app to be open and logged in."""
    params = {}
    if name:
        params["name"] = name
    if query:
        params["query"] = query
    return _send_ui_command("new_tab", params)


@mcp.tool()
def close_editor_tab(tab: str) -> str:
    """Close the RuSQL editor tab with this exact name. Requires the RuSQL app to be
    open and logged in."""
    return _send_ui_command("close_tab", tab)


@mcp.tool()
def switch_editor_tab(tab: str) -> str:
    """Switch focus to the RuSQL editor tab with this exact name. Requires the RuSQL
    app to be open and logged in."""
    return _send_ui_command("switch_to_tab", tab)


@mcp.tool()
def list_editor_tabs() -> str:
    """List the names of all open tabs in the RuSQL editor, in order. Returns a JSON
    array. Requires the RuSQL app to be open and logged in."""
    return _send_ui_command("list_tabs", "")


@mcp.tool()
def get_editor_tab_content(tab: str = "") -> str:
    """Get the current SQL text in the given RuSQL editor tab (or the active tab if
    `tab` is omitted). Requires the RuSQL app to be open and logged in."""
    return _send_ui_command("get_tab_content", tab)


@mcp.tool()
def execute_in_editor(query: str = "", tab: str = "") -> str:
    """Run a query in the RuSQL editor UI itself (as if the user clicked Run), in the
    given tab or the active tab if omitted. If `query` is given it replaces that tab's
    content first, otherwise whatever is already in the tab is run as-is. Returns the
    result as JSON. Unlike execute_sql (which runs invisibly against the engine), this
    drives the real UI so the user sees the query and its result appear on screen.
    Requires the RuSQL app to be open and logged in."""
    params = {}
    if tab:
        params["tab"] = tab
    if query:
        params["query"] = query
    return _send_ui_command("execute_in_editor", params)


if __name__ == "__main__":
    mcp.run()
