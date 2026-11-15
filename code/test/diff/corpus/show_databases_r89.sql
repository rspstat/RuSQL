# R89 (2026-11-16): SHOW DATABASES | SCHEMAS LIKE 'pattern' | WHERE condition. The rows are the schemas that match, by name; the column is `Database (pattern)` for LIKE and `Database` for WHERE.
# Measured on MySQL 8.4.9. A line is a statement, '--' a setup statement run on both. Run: python vs_mysql.py <engine_server.exe> corpus/show_databases_r89.sql --proto --errno
--DROP DATABASE IF EXISTS zz_a
--DROP DATABASE IF EXISTS zz_b
--DROP DATABASE IF EXISTS yy_c
--CREATE DATABASE zz_a
--CREATE DATABASE zz_b
--CREATE DATABASE yy_c
SHOW DATABASES LIKE 'zz%'
SHOW DATABASES LIKE 'zz_a'
SHOW DATABASES LIKE 'ZZ_A'
SHOW DATABASES LIKE 'zz\_a'
SHOW DATABASES LIKE 'zz\_'
SHOW DATABASES LIKE 'nosuch'
SHOW DATABASES LIKE "zz%"
SHOW SCHEMAS LIKE 'yy%'
SHOW DATABASES LIKE 'zz''_a'
SHOW DATABASES LIKE 'my%'
SHOW DATABASES WHERE `Database` = 'zz_b'
SHOW DATABASES WHERE `Database` LIKE 'zz%' AND `Database` <> 'zz_a'
SHOW SCHEMAS WHERE `Database` > 'yy' AND `Database` < 'zzz'
SHOW DATABASES WHERE `Database` = 'mysql'
SHOW DATABASES WHERE nosuch = 1
SHOW DATABASES LIKE
SHOW DATABASES LIKE 'zz%' WHERE 1
SHOW DATABASES LIKE zz
SHOW DATABASES LIKE 5
SHOW DATABASES FROM zz_a
--DROP DATABASE zz_a
--DROP DATABASE zz_b
--DROP DATABASE yy_c
