# R90 (2026-11-16): index hints (USE | FORCE | IGNORE INDEX | KEY [FOR JOIN | ORDER BY | GROUP BY] (names)) after a table in FROM or UPDATE are read and do not change the answer.
# (not here: DELETE FROM t USE INDEX, which MySQL calls a syntax error and RuSQL reads; the error 1176 of a hint that names an index the table does not have, and NATURAL LEFT | RIGHT JOIN -- neither is done.)
# Measured on MySQL 8.4.9. A line is a statement, '--' a setup statement run on both. Run: python vs_mysql.py <engine_server.exe> corpus/hints_r90.sql --proto --errno
--DROP TABLE IF EXISTS hn1
--DROP TABLE IF EXISTS hn2
--CREATE TABLE hn1 (id INT PRIMARY KEY, a INT, b VARCHAR(10), KEY ia (a), KEY ib (b))
--CREATE TABLE hn2 (id INT PRIMARY KEY, a INT, c VARCHAR(10), KEY ja (a))
--INSERT INTO hn1 VALUES (1, 10, 'x'), (2, 20, 'y'), (3, 30, 'z'), (4, 10, 'w')
--INSERT INTO hn2 VALUES (1, 10, 'p'), (2, 20, 'q'), (5, 50, 'r')
SELECT id FROM hn1 USE INDEX (ia) WHERE a = 10 ORDER BY id
SELECT id FROM hn1 FORCE INDEX (ia) WHERE a = 10 ORDER BY id
SELECT id FROM hn1 IGNORE INDEX (ia) WHERE a = 10 ORDER BY id
SELECT id FROM hn1 USE KEY (ia) WHERE a = 10 ORDER BY id
SELECT id FROM hn1 FORCE KEY (ia, ib) WHERE a = 10 ORDER BY id
SELECT id FROM hn1 USE INDEX () WHERE a = 10 ORDER BY id
SELECT id FROM hn1 USE INDEX FOR ORDER BY (ia) WHERE a = 10 ORDER BY id
SELECT id FROM hn1 FORCE INDEX FOR JOIN (ia) WHERE a = 10 ORDER BY id
SELECT id FROM hn1 IGNORE INDEX FOR GROUP BY (ia) WHERE a = 10 ORDER BY id
SELECT id FROM hn1 USE INDEX (ia) USE INDEX (ib) WHERE a = 10 ORDER BY id
SELECT id FROM hn1 USE INDEX (PRIMARY) WHERE id = 2
SELECT x.id FROM hn1 AS x FORCE INDEX (ia) WHERE x.a = 10 ORDER BY x.id
SELECT x.id FROM hn1 x FORCE INDEX (ia) WHERE x.a = 10 ORDER BY x.id
SELECT hn1.id, hn2.c FROM hn1 FORCE INDEX (ia) JOIN hn2 USE INDEX (ja) ON hn1.a = hn2.a ORDER BY hn1.id
SELECT hn1.id, hn2.c FROM hn1 USE INDEX (ia) LEFT JOIN hn2 IGNORE INDEX (ja) ON hn1.a = hn2.a ORDER BY hn1.id
SELECT p.id FROM hn1 p FORCE INDEX (ia) JOIN hn2 q FORCE INDEX (ja) ON p.a = q.a ORDER BY p.id
UPDATE hn1 USE INDEX (ia) SET b = 'u' WHERE a = 20
SELECT id, b FROM hn1 ORDER BY id
SELECT id, b FROM hn1 ORDER BY id
SELECT COUNT(*) FROM hn1 FORCE INDEX (ia)
SELECT * FROM hn1 NATURAL JOIN hn2 ORDER BY id
