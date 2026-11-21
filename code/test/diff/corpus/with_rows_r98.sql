# R98 (2026-11-21): the rows of INSERT / REPLACE / CREATE TABLE come from `WITH ... SELECT` (MySQL 8: INSERT ... WITH ... SELECT, REPLACE ... WITH ... SELECT, CREATE TABLE ... [AS] WITH ... SELECT,
# CREATE VIEW ... AS WITH ... SELECT). RuSQL refused them (`Expected VALUES, SET or SELECT`, `Expected '('`). A write is followed by a SELECT of what it wrote.
--DROP VIEW IF EXISTS wr_view
--DROP TABLE IF EXISTS wr_dst
--DROP TABLE IF EXISTS wr_plain
--DROP TABLE IF EXISTS wr_as
--DROP TABLE IF EXISTS wr_bare
--DROP TABLE IF EXISTS wr_cols
--DROP TABLE IF EXISTS wr_src
--CREATE TABLE wr_src (id INT PRIMARY KEY, name VARCHAR(20), v INT)
--INSERT INTO wr_src VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30)
--CREATE TABLE wr_dst (id INT PRIMARY KEY, name VARCHAR(20), v INT)
INSERT INTO wr_dst WITH big AS (SELECT id, name, v FROM wr_src WHERE v >= 20) SELECT id, name, v FROM big
SELECT id, name, v FROM wr_dst ORDER BY id
INSERT INTO wr_dst (id, v) WITH one AS (SELECT 9 AS i, 90 AS w) SELECT i, w FROM one
SELECT id, name, v FROM wr_dst ORDER BY id
INSERT INTO wr_dst (id, v) WITH RECURSIVE n (k) AS (SELECT 100 UNION ALL SELECT k + 1 FROM n WHERE k < 104) SELECT k, k * 2 FROM n
SELECT COUNT(*), SUM(v) FROM wr_dst
INSERT INTO wr_dst (id, v) WITH a AS (SELECT 200 AS i), b AS (SELECT i + 1 AS j FROM a) SELECT i, j FROM a, b
SELECT id, v FROM wr_dst WHERE id >= 200 ORDER BY id
INSERT IGNORE INTO wr_dst WITH s AS (SELECT id, name, v FROM wr_src) SELECT id, name, v FROM s
SELECT id, name, v FROM wr_dst ORDER BY id
REPLACE INTO wr_dst WITH s AS (SELECT id, 'z' AS name, v + 1 AS v FROM wr_src WHERE id = 2) SELECT id, name, v + 1 FROM s
SELECT id, name, v FROM wr_dst WHERE id = 2
CREATE TABLE wr_plain AS SELECT id, name FROM wr_src
CREATE TABLE wr_as AS WITH x AS (SELECT 1 AS one) SELECT id, name FROM wr_src
CREATE TABLE wr_bare WITH x AS (SELECT 1 AS one) SELECT id, name FROM wr_src
CREATE TABLE wr_cols (a INT, b VARCHAR(20)) AS WITH x AS (SELECT id, name FROM wr_src WHERE id < 3) SELECT id, name FROM x
SELECT id, name FROM wr_as ORDER BY id
SELECT a, b FROM wr_cols ORDER BY a
# (Not here: DESCRIBE of these tables -- MySQL keeps NOT NULL of a plain source column, RuSQL shows YES; the same with and without WITH, an older difference of CREATE TABLE ... AS SELECT.)
CREATE VIEW wr_view AS WITH c AS (SELECT id, v FROM wr_src) SELECT id, v FROM c WHERE v > 10
SELECT id, v FROM wr_view ORDER BY id
INSERT INTO wr_src VALUES (4, 'd', 40)
SELECT id, v FROM wr_view ORDER BY id
INSERT INTO wr_dst WITH s AS (SELECT 1) DELETE FROM wr_src
INSERT INTO wr_dst WITH s AS (SELECT 1 AS i)
