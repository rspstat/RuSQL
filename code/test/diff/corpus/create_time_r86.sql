# R86 (2026-11-15): which statements make a table again as far as information_schema.tables.CREATE_TIME says. Measured on MySQL 8.4.9: ALTER TABLE (any action but a rename alone), CREATE INDEX,
# DROP INDEX and OPTIMIZE TABLE give the table a new CREATE_TIME; RENAME (both forms), TRUNCATE, ANALYZE, a write and a failed ALTER keep it. (R85 said TRUNCATE changes it: it does not.)
# A line is a statement, '--' a setup statement run on both. Run: python vs_mysql.py <engine_server.exe> corpus/create_time_r86.sql --proto --errno
# (the two servers keep different times: the last line asks only whether a table is younger than 3 seconds, after a 3 second pause)
--SET SESSION information_schema_stats_expiry = 0
--CREATE TABLE c01 (a INT PRIMARY KEY, b INT)
--INSERT INTO c01 VALUES (1, 1)
--CREATE TABLE c02 (a INT PRIMARY KEY, b INT)
--INSERT INTO c02 VALUES (1, 1)
--CREATE TABLE c03 (a INT PRIMARY KEY, b INT)
--INSERT INTO c03 VALUES (1, 1)
--CREATE TABLE c04 (a INT PRIMARY KEY, b INT)
--INSERT INTO c04 VALUES (1, 1)
--CREATE TABLE c05 (a INT PRIMARY KEY, b INT)
--INSERT INTO c05 VALUES (1, 1)
--CREATE TABLE c07 (a INT PRIMARY KEY, b INT)
--INSERT INTO c07 VALUES (1, 1)
--CREATE TABLE c08 (a INT PRIMARY KEY, b INT)
--INSERT INTO c08 VALUES (1, 1)
--CREATE TABLE c09 (a INT PRIMARY KEY, b INT)
--INSERT INTO c09 VALUES (1, 1)
--CREATE TABLE c10 (a INT PRIMARY KEY, b INT)
--INSERT INTO c10 VALUES (1, 1)
--CREATE TABLE c11 (a INT PRIMARY KEY, b INT)
--INSERT INTO c11 VALUES (1, 1)
--CREATE TABLE c12 (a INT PRIMARY KEY, b INT)
--INSERT INTO c12 VALUES (1, 1)
--CREATE TABLE c13 (a INT PRIMARY KEY, b INT)
--INSERT INTO c13 VALUES (1, 1)
--CREATE TABLE c14 (a INT PRIMARY KEY, b INT)
--INSERT INTO c14 VALUES (1, 1)
--CREATE TABLE c15 (a INT PRIMARY KEY, b INT)
--INSERT INTO c15 VALUES (1, 1)
--CREATE TABLE c16 (a INT PRIMARY KEY, b INT)
--INSERT INTO c16 VALUES (1, 1)
--CREATE INDEX ib ON c11 (b)
SELECT SLEEP(3)
ALTER TABLE c01 ADD COLUMN c INT
ALTER TABLE c02 DROP COLUMN b
ALTER TABLE c03 ADD INDEX ib (b)
ALTER TABLE c04 ALTER COLUMN b SET DEFAULT 5
ALTER TABLE c05 COMMENT = 'x'
ALTER TABLE c07 RENAME TO c07r
RENAME TABLE c08 TO c08r
ALTER TABLE c09 RENAME TO c09r, ADD COLUMN c INT
CREATE INDEX ib ON c10 (b)
DROP INDEX ib ON c11
OPTIMIZE TABLE c12
ANALYZE TABLE c13
TRUNCATE TABLE c14
INSERT INTO c15 VALUES (2, 2)
ALTER TABLE c16 DROP COLUMN nosuch
CREATE INDEX ib ON c16 (nosuch)
DROP INDEX nosuch ON c16
SELECT table_name, TIMESTAMPDIFF(SECOND, create_time, NOW()) < 3 AS younger FROM information_schema.tables WHERE table_schema = 'v' AND table_name LIKE 'c%' ORDER BY table_name
