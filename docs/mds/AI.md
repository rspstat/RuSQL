# AI 연동 — Claude Desktop + MCP

RuSQL의 AI 기능은 Claude Desktop을 MCP(Model Context Protocol)로 연결해 제공한다. 사용자가 자연어로 말하면 Claude가 SQL을 만들고, RuSQL의 AI MCP 도구가 그것을 실행하거나 관리 도구의 화면을 조작한다. AI 추론은 Claude Desktop이 맡으므로 RuSQL에는 API 키가 필요 없다.

## 구조

```
Claude Desktop ⇄ stdio (JSON-RPC) ⇄ rusql-mcp (Python · FastMCP) ⇄ TCP 7878 ⇄ engine_server
                                      └ ui_commands.json + Tauri 이벤트 ⇄ 관리 도구(rusql-ui)
```

## AI MCP 도구

도구는 다섯 묶음이다: SQL 실행·CSV(`execute_sql` · `run_script` · `import_csv` · `export_csv`), 조회·진단(스키마, 실행 계획, 인덱스 제안, 락, 느린 쿼리, 세션 종료, 스키마 비교 등), 연결 관리, 편집기 제어(탭 만들기·전환·쿼리 실행), 서버·앱 제어. 그 밖에 resources 2개와 prompts 3개가 있다. 데이터베이스 · 연결 목록 · 앱을 바꾸지 않는 도구에는 MCP `readOnlyHint`를 붙였다.

## 안전장치

- DROP · TRUNCATE · WHERE 없는 UPDATE/DELETE는 `confirm_dangerous_sql`로 확인하기 전에는 거절한다.
- `run_script`는 영향받는 행을 미리 보여 주고 `commit=True`일 때만 저장한다.
- `kill_session`은 먼저 묻고 끊고, `suggest_indexes`는 UPDATE · DELETE를 실행하지 않고 추정으로만 시험한다.

## 연결 방식 (2026-11-23)

Claude Desktop이 RuSQL MCP 서버를 쓰는 길은 둘이다 — 앱 MCP Agent 패널의 **Add to Claude Desktop (extension)**(`RuSQL.mcpb` 확장을 만들어 탐색기로 보여 주고 사용자가 Claude Desktop에 넣음; 새로 설치한 Microsoft Store판이 설정 파일을 읽지 않는 문제를 피함)와 **Auto-connect Claude Desktop (config file)**(`claude_desktop_config.json`에 씀). 둘 중 하나만 쓴다 — 확장 단추는 설정 파일의 RuSQL 항목을 지우고, 설정 파일 단추는 확장이 있으면 경고한다. 데이터베이스 · 연결 목록 · 앱을 바꾸지 않는 도구에는 MCP `readOnlyHint`를 붙였다(Claude Desktop이 "읽기 전용"으로 묶으면 한 번에 허용할 수 있다; Claude 화면의 실제 동작은 확인하지 못함).

## macOS에서 (2026-11-23)

같은 두 단추가 맥에서도 돈다. 확장 단추는 `~/Library/Application Support/RuSQL/RuSQL.mcpb`를 만들어 Finder(`open -R`)로 보여 주고(manifest의 플랫폼은 `darwin`, Python은 `RuSQL.app/Contents/Resources/python/bin/python3`), 설정 파일 단추는 `~/Library/Application Support/Claude/claude_desktop_config.json`에 쓴다(Store판 경로는 Windows에만 있다); Claude Desktop은 ⌘Q로 완전히 종료한다. 앱이 `.dmg`나 다운로드 폴더에서 바로 열려 임시 위치(`/AppTranslocation/`)에서 돌면 그 경로가 곧 사라지므로 두 단추가 거절한다 — 응용 프로그램 폴더로 옮겨 다시 실행한다. `launch_app`은 앱 꾸러미 안의 실행 파일 경로를 받으면 `open -n`으로 연다. 번들 Python + MCP 서버 + 엔진은 설치한 `.dmg` 앱에서 Actions가 확인한다(`code/test/mac/smoke_bundle.py`: 도구 목록 · 읽기 전용 표시 · SQL 실행). Mac의 Claude Desktop 자체(확장 설치 창)는 확인하지 못했다.

## 반영 위치

`README.md`(AI Integration), `FUNCTIONS.md`, `DIFF.md`의 AI 연동 행, 앱의 Diagram 페이지(AI 패널), 산출물 D01~D08.
