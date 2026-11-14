# RuSQL 빌드 · 실행 · 시험 명령 모음

개발 PC 기준 환경: Windows 11, Visual Studio 2022(MSVC, C++20), CMake 4.3.3(3.20 이상이면 됩니다), Node.js 24 / npm 11, Rust 1.96(cargo, Tauri용), Python 3.14.

<br/>

1. C++ 빌드
```
cmake -S code -B code/build
cmake --build code/build --config Release
```
`--config Debug`도 같은 방식입니다. 시험과 측정 수치는 Release 빌드 기준입니다. Debug 산출물은 오래된 것일 수 있으므로 시험하기 전에 다시 빌드합니다. 한 대상만 빌드하려면 `--target engine_server`처럼 이름을 붙입니다.

| 대상 | 설명 | 실행 파일 (Release) |
|---|---|---|
| engine_core | DB 엔진 정적 라이브러리 | (라이브러리) |
| engine_cli | 터미널 REPL | `code/build/backend/cli/Release/engine_cli.exe` |
| engine_client | TCP 클라이언트 CLI | `code/build/backend/client/Release/engine_client.exe` |
| engine_server | TCP 서버 (자체 프로토콜 7878 + MySQL 프로토콜 3306) | `code/build/backend/server/Release/engine_server.exe` |
| engine_tests | Catch2 단위 시험 | `code/build/backend/tests/Release/engine_tests.exe` |

<br/>

2. 실행
```
# 터미널 REPL
code/build/backend/cli/Release/engine_cli.exe

# 서버 (자체 프로토콜 7878 + MySQL 프로토콜 3306을 함께 수신)
code/build/backend/server/Release/engine_server.exe
code/build/backend/server/Release/engine_server.exe --mysql-port 13306
code/build/backend/server/Release/engine_server.exe --no-mysql
code/build/backend/server/Release/engine_server.exe --buffer-pool-size 256

# 자체 클라이언트 (서버를 띄운 뒤)
code/build/backend/client/Release/engine_client.exe -u root -p root -h 127.0.0.1 -P 7878

# 기존 MySQL 도구로 접속 (mysql CLI, DBeaver, JDBC 등)
mysql -h 127.0.0.1 -P 3306 -u root --skip-auto-rehash
```
이 PC의 MySQL이 이미 3306을 쓰고 있으면 `--mysql-port`로 다른 포트를 지정합니다. 그 밖의 서버 옵션은 `--mysql-bind`(수신 주소), `--buffer-pool-size`(기본 64 페이지)입니다.

<br/>

3. 시험
```
# C++ 단위 시험 (Catch2, 현재 993 케이스)
code/build/backend/tests/Release/engine_tests.exe
code/build/backend/tests/Release/engine_tests.exe "[write_integrity]"
code/build/backend/tests/Release/engine_tests.exe --list-tests

# CTest로 같은 시험 실행 (CTest 설정이 backend 폴더에 있습니다)
ctest --test-dir code/build/backend -C Release

# 기능 확인용 SQL 스크립트
code/build/backend/cli/Release/engine_cli.exe < code/test/test_full.sql
code/build/backend/cli/Release/engine_cli.exe < code/test/test_full-ver2.sql
```
그 밖의 시험은 저장소 루트에서 실행합니다.

| 명령 | 내용 |
|---|---|
| `py code/test/mcp/test_monitoring_tools.py` | MCP 도구 단위 시험 (`test_suggest_indexes.py`, `test_r70_tools.py`도 같은 방식) |
| `py code/test/mcp/live_check.py` | 실제 `engine_server`를 띄워 MCP 도구를 확인 |
| `node code/test/frontend/test_libs.mjs` | UI의 DOM 없는 라이브러리 시험 |
| `py -3 code/test/frontend/cdp/test_screens.py` | 앱을 실제로 띄워 화면을 하나씩 점검 |
| `py code/test/crash/fuzz_crash.py [횟수] [시드]` | `kill -9` 뒤 복구가 맞는지 보는 무작위 시험 (같은 폴더에 외래 키 · 동시 접속용도 있습니다) |
| `py code/test/diff/diff_builds.py <옛 engine_server.exe> <새 engine_server.exe>` | 같은 질의를 두 빌드에서 돌려 결과를 비교 |

실제 MySQL과 결과를 대조하는 도구(`code/test/diff/*.py`)는 MySQL 8.4를 `127.0.0.1:3391`(사용자 root, 비밀번호 없음)에 따로 띄워야 하며, 자세한 사용법은 `code/test/diff/README.txt`에 있습니다. 성능 측정은 `code/test/perf/README.txt`를 봅니다.

<br/>

4. UI 실행
```
cd code/frontend
npm install
npm run tauri dev
```
`npm run build`는 타입 검사(`tsc`)와 Vite 빌드만 하고, `npm run tauri build`는 설치 파일을 만듭니다. 앱은 엔진을 내장하지 않고 `engine_server`에 TCP로 접속하므로, 서버를 먼저 띄우거나 앱의 서버 관리 화면에서 기동합니다.

<br/>

5. MCP 서버 (Claude Desktop 연동)
```
pip install -r code/mcp/requirements.txt
python -u code/mcp/mcp_server.py
```
`mcp_server.py`는 stdio로 통신하므로 보통 Claude Desktop이 직접 실행합니다. 연결하려면 `code/mcp/claude_desktop_config_example.json`의 `rusql` 항목을 Claude Desktop 설정 파일(`claude_desktop_config.json`)의 `mcpServers`에 넣고 경로를 이 PC에 맞게 고칩니다. 앱의 AI MCP 패널에서 자동으로 등록할 수도 있습니다. 도구 목록과 사용 방법은 `docs/mds/AI.md`를 봅니다.

<br/>

6. Git

`main`이 현재 C++ 버전(실제 개발 브랜치)이고, `legacy`는 과거 Rust 버전을 보존만 해둔 아카이브 브랜치입니다(더 이상 개발 안 함). 커밋 메시지는 `commit` 한 단어로 통일합니다.

```
git checkout main

git add -A
git commit -m "commit"
git push origin main

git checkout -b 브랜치이름
git add -A
git commit -m "commit"
git push -u origin 브랜치이름
```
