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
