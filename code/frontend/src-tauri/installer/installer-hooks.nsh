; RuSQL installer: texts of the Welcome and Finish pages (English; the installer has no other language).
LangString rusqlWelcomeTitle 1033 "Welcome to the RuSQL Setup Wizard"
LangString rusqlWelcomeText 1033 "This wizard will guide you through the installation of RuSQL, an open-source relational database management system with MySQL protocol compatibility, a desktop management tool and an MCP server.$\r$\n$\r$\nIt is recommended that you close all other applications before starting Setup.$\r$\n$\r$\nClick Next to continue."
LangString rusqlFinishTitle 1033 "Completing the RuSQL Setup Wizard"
LangString rusqlFinishText 1033 "RuSQL has been installed on your computer.$\r$\n$\r$\nA default connection named 'RuSQL Local' is created the first time the application starts.$\r$\n$\r$\nClick Finish to close this wizard."

; Closing RuSQL leaves the engine it started running, and a running engine_server.exe cannot be overwritten or deleted
; ("Error opening file for writing"). Stop the engines that live in this install folder before files are copied or removed.
; (The installer is a 32-bit program, so this PowerShell is the 32-bit one: Get-Process cannot read the path of a 64-bit process there, Win32_Process can.)
!macro RusqlStopEngine
  Push $0
  nsExec::Exec `powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-CimInstance Win32_Process | Where-Object { $$_.Name -eq 'engine_server.exe' -and $$_.ExecutablePath -like '$INSTDIR\engine\*' } | ForEach-Object { Stop-Process -Id $$_.ProcessId -Force }"`
  Pop $0
  Sleep 500
  Pop $0
!macroend
!macro NSIS_HOOK_PREINSTALL
  !insertmacro RusqlStopEngine
!macroend
!macro NSIS_HOOK_PREUNINSTALL
  !insertmacro RusqlStopEngine
!macroend

!define MUI_WELCOMEPAGE_TITLE "$(rusqlWelcomeTitle)"
!define MUI_WELCOMEPAGE_TEXT "$(rusqlWelcomeText)"
!define MUI_FINISHPAGE_TITLE "$(rusqlFinishTitle)"
!define MUI_FINISHPAGE_TEXT "$(rusqlFinishText)"
