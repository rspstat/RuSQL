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
# C++ 단위 시험 (Catch2, 현재 1,065 케이스)
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
| `py -3 code/test/frontend/cdp/stress_ecosystem.py <연결> <DB> <표> [--release]` | View Topology(Ecosystem) 창의 규모 시험(R94): 가짜 데이터 폴더를 임시 폴더에 만들어 연결 수십 개 · 표 수천 개로 그려 보고 그리는 시간 · 프레임 · 마우스 반응을 출력하며 구조를 확인(`connections.json`은 원래대로 되돌림) |
| `py code/test/crash/fuzz_crash.py [횟수] [시드]` | `kill -9` 뒤 복구가 맞는지 보는 무작위 시험 (같은 폴더에 외래 키 · 동시 접속용도 있습니다) |
| `py code/test/crash/fuzz_kill.py [횟수] [시드]` | 프로세스가 아니라 SQL의 `KILL QUERY` · `KILL <id>`을 무작위 세션에 쏘면서 쓰기를 시킴(R94; 세션 목록은 `SHOW PROCESSLIST`로 읽고 `FUZZ_KILL_INFOSCHEMA=1`이면 `information_schema.processlist`로 읽음 — R94에는 이것이 경합을 일으켰고 R95에서 닫혔다): 멈춘 문장은 아무것도 안 바꾸고, 끊긴 세션의 트랜잭션은 되돌려지고, 잠금이 안 남고, 끝나면 죽은 세션이 목록에 없고 인덱스가 표와 맞는지 확인. 홀수 시드는 `kill -9`까지 섞음. `RUSQL_SERVER_EXE`로 다른 빌드(Debug 등)를 시험 |
| `py code/test/diff/diff_builds.py <옛 engine_server.exe> <새 engine_server.exe>` | 같은 질의를 두 빌드에서 돌려 결과를 비교 |

실제 MySQL과 결과를 대조하는 도구(`code/test/diff/*.py`)는 MySQL 8.4를 `127.0.0.1:3391`(사용자 root, 비밀번호 없음)에 따로 띄워야 하며, 자세한 사용법은 `code/test/diff/README.txt`에 있습니다. 성능 측정은 `code/test/perf/README.txt`를 봅니다.

<br/>

4. UI 실행
```
cd code/frontend
npm install
npm run tauri dev
```
`npm run build`는 타입 검사(`tsc`)와 Vite 빌드만 하고, `npm run tauri build`는 설치 파일(`src-tauri/target/release/bundle/nsis/RuSQL_<버전>_x64-setup.exe`)을 만듭니다. 설치 파일에는 `engine_server.exe`(Release)와 그것이 쓰는 VC++ 런타임 DLL 셋(`msvcp140` · `vcruntime140` · `vcruntime140_1`; 재배포 패키지가 없는 PC에서도 뜨도록 엔진 옆에 둠), `mcp_server.py`가 들어가므로 먼저 `cmake --build code/build --config Release --target engine_server`로 엔진을 빌드해 둡니다(없으면 `npm run stage-engine`이 설치 파일을 만들기 전에 멈춥니다). 앱은 로그인할 때 엔진을 직접 띄웁니다: 설치본은 설치 폴더의 `engine/`을, 소스 트리 안(`tauri dev`, 트리 안의 릴리즈 exe)에서는 `code/build/`의 엔진을 씁니다. 설치본의 데이터와 연결 목록은 `%APPDATA%\RuSQL\data`에 있고(소스 트리에서는 `code/data`), MCP 서버에도 같은 폴더를 `RUSQL_DATA_DIR`로 알려 줍니다. 새 PC 점검: `py -3 code/test/frontend/cdp/test_installed_layout.py <설치 폴더>`(설치 파일을 풀어 낸 폴더도 되고, 소스 트리 안의 exe는 `--tree`).

<br/>

5. MCP 서버 (Claude Desktop 연동)
```
pip install -r code/mcp/requirements.txt
python -u code/mcp/mcp_server.py
```
`mcp_server.py`는 stdio로 통신하므로 보통 Claude Desktop이 직접 실행합니다. 연결하려면 `code/mcp/claude_desktop_config_example.json`의 `rusql` 항목을 Claude Desktop 설정 파일(`claude_desktop_config.json`)의 `mcpServers`에 넣고 경로를 이 PC에 맞게 고칩니다. 앱의 AI MCP 패널에서 자동으로 등록할 수도 있습니다: **Add to Claude Desktop (extension)** 은 확장 파일(`RuSQL.mcpb`)을 만들어 탐색기에서 보여 주고(그 파일을 Claude Desktop에 넣어 설치 창을 띄움; 설정 파일을 거치지 않음), **Auto-connect Claude Desktop (config file)** 은 위 설정 파일에 씁니다. 둘 중 하나만 씁니다. 도구 목록과 사용 방법은 `docs/mds/AI.md`를 봅니다.

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

<br/>

7. macOS (Apple Silicon · Intel, macOS 13.3 이상)

Xcode 명령줄 도구, CMake, Node.js, Rust가 있으면 같은 소스가 맥에서 빌드됩니다. Makefile 같은 단일 구성 생성기를 쓰므로 `--config`와 `Release/` 폴더가 없고 실행 파일에 `.exe`가 없습니다.
```
cmake -S code -B code/build -DCMAKE_BUILD_TYPE=Release
cmake --build code/build --parallel
ctest --test-dir code/build/backend --output-on-failure

code/build/backend/server/engine_server              # 서버
code/build/backend/cli/engine_cli                    # 터미널 REPL
code/build/backend/client/engine_client -u root -p root -P 7878
code/build/backend/tests/engine_tests                # Catch2 시험

cd code/frontend
npm ci
npm run tauri build                                  # -> src-tauri/target/release/bundle/dmg/RuSQL_<버전>_<aarch64|x64>.dmg
```
`npm run tauri build`는 `tauri.macos.conf.json`(대상 `app` · `dmg`, 자원은 `engine/engine_server`, 최소 macOS 13.3)을 함께 읽고, `stage-engine`이 `code/build/backend/server/engine_server`를 복사해 ad-hoc 서명하고, `stage-python`이 python-build-standalone(3.12.10, SHA-256을 고정해 확인)을 내려받아 `mcp`를 넣고 모든 Mach-O 파일에 ad-hoc 서명합니다(처음 한 번 인터넷이 필요). 설치본은 `RuSQL.app/Contents/Resources/` 안의 `engine/` · `python/` · `mcp/`를 쓰고, 데이터와 연결 목록은 `~/Library/Application Support/RuSQL/data`에 둡니다. Apple 개발자 서명이 아니므로 처음 실행할 때 시스템 설정 → 개인정보 보호 및 보안에서 **그래도 열기**를 누르거나 `xattr -dr com.apple.quarantine /Applications/RuSQL.app`를 한 번 실행합니다. 응용 프로그램 폴더에서 실행해야 합니다(`.dmg`나 다운로드 폴더에서 바로 열면 앱이 임시 위치에서 돌아 MCP 단추가 거절합니다).

GitHub Actions(`.github/workflows/macos.yml`; `mac-port` 브랜치를 푸시하거나 Actions에서 직접 실행)가 macOS(Apple Silicon · Intel)와 Linux에서 엔진을 빌드해 시험 전체를 돌리고, `.dmg`를 만들어 설치한 앱에 `code/test/mac/smoke_bundle.py`를 돌립니다(엔진 · MySQL 핸드셰이크 · 번들 Python · MCP 도구 37개). 같은 점검을 직접 하려면 설치된 앱의 Python으로 `RuSQL.app/Contents/Resources/python/bin/python3 -B code/test/mac/smoke_bundle.py /Applications/RuSQL.app/Contents/Resources`.
