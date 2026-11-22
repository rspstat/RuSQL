; RuSQL installer: texts of the Welcome and Finish pages (English; the installer has no other language).
LangString rusqlWelcomeTitle 1033 "Welcome to the RuSQL Setup Wizard"
LangString rusqlWelcomeText 1033 "This wizard will guide you through the installation of RuSQL, an open-source relational database management system with MySQL protocol compatibility, a desktop management tool and an MCP server.$\r$\n$\r$\nIt is recommended that you close all other applications before starting Setup.$\r$\n$\r$\nClick Next to continue."
LangString rusqlFinishTitle 1033 "Completing the RuSQL Setup Wizard"
LangString rusqlFinishText 1033 "RuSQL has been installed on your computer.$\r$\n$\r$\nA default connection named 'RuSQL Local' is created the first time the application starts.$\r$\n$\r$\nClick Finish to close this wizard."

!define MUI_WELCOMEPAGE_TITLE "$(rusqlWelcomeTitle)"
!define MUI_WELCOMEPAGE_TEXT "$(rusqlWelcomeText)"
!define MUI_FINISHPAGE_TITLE "$(rusqlFinishTitle)"
!define MUI_FINISHPAGE_TEXT "$(rusqlFinishText)"
