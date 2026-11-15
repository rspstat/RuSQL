# R86 (2026-11-15): the system schema `mysql`. A MySQL-protocol session lists it (SHOW DATABASES, information_schema.schemata); it cannot be made or dropped (3552). The native protocol (the app,
# the CLI, MCP) does not list it: the C++ test has that half. A line is a statement. Run: python vs_mysql.py <engine_server.exe> corpus/system_schema_r86.sql --proto --errno
SELECT schema_name FROM information_schema.schemata WHERE schema_name IN ('mysql', 'v', 'nosuch') ORDER BY schema_name
CREATE DATABASE mysql
CREATE DATABASE IF NOT EXISTS mysql
DROP DATABASE mysql
DROP DATABASE IF EXISTS mysql
SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'mysql' AND table_name = 'nosuch'
