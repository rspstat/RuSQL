# R87 (2026-11-15): ALTER TABLE ... DROP COLUMN and the constraints that read the column. MySQL takes the column out of a UNIQUE key (an index left with no column goes), out of a composite
# PRIMARY KEY, and refuses (1062) when the shorter key would not be unique over the rows the table holds; a column that an FK or a table-level CHECK needs cannot be dropped.
# A line is a statement, '--' a setup statement run on both. Run: python vs_mysql.py <engine_server.exe> corpus/drop_column_constraints_r87.sql --proto --errno
--CREATE TABLE d1 (id INT PRIMARY KEY, a INT, b INT, c INT, UNIQUE KEY uab (a, b), UNIQUE KEY ub (b))
--INSERT INTO d1 VALUES (1, 1, 1, 1), (2, 2, 2, 2)
ALTER TABLE d1 DROP COLUMN b
SELECT index_name, column_name, seq_in_index, non_unique FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'd1' ORDER BY index_name, seq_in_index
INSERT INTO d1 VALUES (3, 1, 9)
INSERT INTO d1 VALUES (3, 3, 9)
SELECT id, a, c FROM d1 ORDER BY id
--CREATE TABLE d2 (id INT PRIMARY KEY, a INT, b INT UNIQUE, c INT)
--INSERT INTO d2 VALUES (1, 1, 1, 1), (2, 2, 2, 2)
ALTER TABLE d2 DROP COLUMN b
SELECT index_name, column_name, seq_in_index, non_unique FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'd2' ORDER BY index_name, seq_in_index
INSERT INTO d2 VALUES (3, 3, 3)
--CREATE TABLE d3 (id INT PRIMARY KEY, a INT, b INT, c INT, UNIQUE KEY uabc (a, b, c))
--INSERT INTO d3 VALUES (1, 1, 1, 1), (2, 1, 2, 2)
ALTER TABLE d3 DROP COLUMN b
SELECT index_name, column_name, seq_in_index, non_unique FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'd3' ORDER BY index_name, seq_in_index
INSERT INTO d3 VALUES (3, 1, 1)
INSERT INTO d3 VALUES (3, 1, 3)
--CREATE TABLE d4 (id INT PRIMARY KEY, a INT, b INT, UNIQUE KEY uab (a, b))
--INSERT INTO d4 VALUES (1, 1, 1), (2, 1, 2)
ALTER TABLE d4 DROP COLUMN b
SELECT id, a, b FROM d4 ORDER BY id
SELECT index_name, column_name, seq_in_index, non_unique FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'd4' ORDER BY index_name, seq_in_index
--CREATE TABLE d5 (a INT, b INT, c INT, PRIMARY KEY (a, b))
--INSERT INTO d5 VALUES (1, 1, 1), (2, 1, 2)
ALTER TABLE d5 DROP COLUMN b
SELECT index_name, column_name, seq_in_index, non_unique FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'd5' ORDER BY index_name, seq_in_index
INSERT INTO d5 VALUES (1, 5)
--CREATE TABLE d6 (a INT, b INT, c INT, PRIMARY KEY (a, b))
--INSERT INTO d6 VALUES (1, 1, 1), (1, 2, 2)
ALTER TABLE d6 DROP COLUMN b
SELECT a, b, c FROM d6 ORDER BY b
--CREATE TABLE d7 (a INT PRIMARY KEY, b INT)
ALTER TABLE d7 DROP COLUMN a
INSERT INTO d7 VALUES (1)
INSERT INTO d7 VALUES (1)
SELECT COUNT(*) FROM d7
--CREATE TABLE d8 (a INT, b INT, CONSTRAINT ck8 CHECK (b > 0))
ALTER TABLE d8 DROP COLUMN b
--CREATE TABLE d9 (a INT, b INT CHECK (b > 0))
ALTER TABLE d9 DROP COLUMN b
INSERT INTO d9 VALUES (1)
--CREATE TABLE d10p (id INT PRIMARY KEY)
--CREATE TABLE d10 (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES d10p (id))
ALTER TABLE d10 DROP COLUMN p
ALTER TABLE d10p DROP COLUMN id
--CREATE TABLE d11 (id INT PRIMARY KEY, a INT, b INT, c INT, UNIQUE KEY uab (a, b), KEY ib (b), KEY iab (a, b))
--INSERT INTO d11 VALUES (1, 1, 1, 1), (2, 2, 2, 2)
ALTER TABLE d11 DROP COLUMN a, DROP COLUMN b
SELECT index_name, column_name, seq_in_index, non_unique FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'd11' ORDER BY index_name, seq_in_index
INSERT INTO d11 VALUES (1, 1)
--CREATE TABLE d12p (id INT PRIMARY KEY, x INT)
--CREATE TABLE d12c (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES d12p (id))
ALTER TABLE d12p DROP COLUMN id
ALTER TABLE d12p DROP COLUMN x
--CREATE TABLE d13 (a INT, b INT, UNIQUE KEY ua (a), UNIQUE KEY uab (a, b))
ALTER TABLE d13 DROP COLUMN b
SELECT index_name, column_name, seq_in_index, non_unique FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'd13' ORDER BY index_name, seq_in_index
INSERT INTO d13 VALUES (1)
INSERT INTO d13 VALUES (1)
--CREATE TABLE d14 (a INT, b INT, c INT, UNIQUE KEY uab (a, b))
--INSERT INTO d14 VALUES (1, NULL, 1), (1, NULL, 2)
ALTER TABLE d14 DROP COLUMN b
SELECT a, b, c FROM d14 ORDER BY c
--CREATE TABLE d15 (a INT, b INT, c INT, UNIQUE KEY uabc (a, b, c))
--INSERT INTO d15 VALUES (1, 1, 1), (1, 2, 2)
ALTER TABLE d15 DROP COLUMN c, DROP COLUMN b
SELECT a, b, c FROM d15 ORDER BY c
ALTER TABLE d15 DROP COLUMN c
SELECT index_name, column_name, seq_in_index, non_unique FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'd15' ORDER BY index_name, seq_in_index
ALTER TABLE d15 DROP COLUMN a, DROP COLUMN b
SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'd15'
--CREATE TABLE d16 (a INT, b INT, c INT, UNIQUE KEY uabc (a, b, c))
--INSERT INTO d16 VALUES (1, NULL, 1), (1, NULL, 2)
ALTER TABLE d16 DROP COLUMN c
SELECT index_name, column_name, seq_in_index, non_unique FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'd16' ORDER BY index_name, seq_in_index
INSERT INTO d16 VALUES (1, NULL)
SELECT COUNT(*) FROM d16
--CREATE TABLE d17 (a INT, b INT, UNIQUE KEY uab (a, b))
--INSERT INTO d17 VALUES (1, 1), (1, 2)
DELETE FROM d17 WHERE b = 2
ALTER TABLE d17 DROP COLUMN b
SELECT a FROM d17
--CREATE TABLE d18 (a INT, b INT, c INT, UNIQUE KEY ub (b), UNIQUE KEY uab (a, b))
ALTER TABLE d18 DROP COLUMN a, DROP COLUMN b
SELECT index_name, column_name, seq_in_index, non_unique FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'd18' ORDER BY index_name, seq_in_index
INSERT INTO d18 VALUES (1)
INSERT INTO d18 VALUES (1)
SELECT COUNT(*) FROM d18
--CREATE TABLE d19 (a INT, b INT, c INT, PRIMARY KEY (a, b))
--INSERT INTO d19 VALUES (1, 1, 10), (2, 1, 20), (3, 1, 30)
ALTER TABLE d19 DROP COLUMN b
UPDATE d19 SET a = 5 WHERE a = 1
UPDATE d19 SET a = 2 WHERE a = 3
REPLACE INTO d19 VALUES (2, 99)
SELECT a, c FROM d19 WHERE a = 2
SELECT a, c FROM d19 ORDER BY a
DELETE FROM d19 WHERE a = 5
SELECT COUNT(*) FROM d19
INSERT INTO d19 VALUES (5, 1) ON DUPLICATE KEY UPDATE c = 7
INSERT INTO d19 VALUES (5, 2) ON DUPLICATE KEY UPDATE c = 7
SELECT a, c FROM d19 ORDER BY a
--CREATE TABLE d20 (a INT, b INT, c INT, UNIQUE KEY uab (a, b))
--INSERT INTO d20 VALUES (1, 1, 10), (2, 1, 20), (3, 1, 30)
ALTER TABLE d20 DROP COLUMN b
UPDATE d20 SET a = 5 WHERE a = 1
UPDATE d20 SET a = 2 WHERE a = 3
REPLACE INTO d20 VALUES (2, 99)
SELECT a, c FROM d20 ORDER BY a
INSERT INTO d20 VALUES (5, 1) ON DUPLICATE KEY UPDATE c = 7
SELECT a, c FROM d20 ORDER BY a
