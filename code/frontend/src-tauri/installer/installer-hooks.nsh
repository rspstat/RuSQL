; RuSQL installer: texts of the Welcome and Finish pages (Korean 1042 / English 1033).
; The file must be saved as UTF-8 with a BOM (NSIS reads a file without one as ANSI).
LangString rusqlWelcomeTitle 1042 "RuSQL 설치를 시작합니다"
LangString rusqlWelcomeTitle 1033 "Welcome to RuSQL Setup"
LangString rusqlWelcomeText 1042 "RuSQL은 직접 구현한 관계형 데이터베이스 엔진에 MySQL 프로토콜 호환, 데스크톱 관리 도구, AI/MCP 연동을 더한 RDBMS입니다.$\r$\n$\r$\n엔진, 관리 도구, MCP 서버가 한꺼번에 설치됩니다.$\r$\n$\r$\n계속하려면 '다음'을 눌러 주세요."
LangString rusqlWelcomeText 1033 "RuSQL is an RDBMS with its own engine, MySQL protocol compatibility, a desktop management tool and AI/MCP integration.$\r$\n$\r$\nThe engine, the management tool and the MCP server are installed together.$\r$\n$\r$\nClick Next to continue."
LangString rusqlFinishTitle 1042 "RuSQL 설치가 끝났습니다"
LangString rusqlFinishTitle 1033 "RuSQL is installed"
LangString rusqlFinishText 1042 "RuSQL을 실행하면 기본 연결 'RuSQL Local'이 만들어져 있어 바로 SQL을 실행해 볼 수 있습니다.$\r$\n$\r$\n'마침'을 눌러 설치를 끝내세요."
LangString rusqlFinishText 1033 "When RuSQL starts, the default connection 'RuSQL Local' is already there, so you can run SQL right away.$\r$\n$\r$\nClick Finish to close Setup."

!define MUI_WELCOMEPAGE_TITLE "$(rusqlWelcomeTitle)"
!define MUI_WELCOMEPAGE_TEXT "$(rusqlWelcomeText)"
!define MUI_FINISHPAGE_TITLE "$(rusqlFinishTitle)"
!define MUI_FINISHPAGE_TEXT "$(rusqlFinishText)"
