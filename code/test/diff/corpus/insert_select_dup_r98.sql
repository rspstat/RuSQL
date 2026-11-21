# R98 (2026-11-21): INSERT ... SELECT ... ON DUPLICATE KEY UPDATE (MySQL 8 takes it, with the SELECT or a WITH ... SELECT; RuSQL answered "Unexpected token(s) after end of statement"). A write is followed by a SELECT of what it wrote.
--DROP TABLE IF EXISTS up_dst
--DROP TABLE IF EXISTS up_src
--CREATE TABLE up_src (id INT PRIMARY KEY, name VARCHAR(20), v INT)
--INSERT INTO up_src VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30)
--CREATE TABLE up_dst (id INT PRIMARY KEY, name VARCHAR(20), v INT, n INT DEFAULT 0)
--INSERT INTO up_dst (id, name, v) VALUES (1, 'old', 1), (9, 'nine', 9)
INSERT INTO up_dst (id, name, v) SELECT id, name, v FROM up_src ON DUPLICATE KEY UPDATE v = VALUES(v) + 100
SELECT id, name, v, n FROM up_dst ORDER BY id
INSERT INTO up_dst (id, name, v) SELECT id, name, v FROM up_src ON DUPLICATE KEY UPDATE n = n + 1
SELECT id, name, v, n FROM up_dst ORDER BY id
INSERT INTO up_dst (id, name, v) SELECT id, name, v + 1 FROM up_src WHERE id <= 2 ON DUPLICATE KEY UPDATE name = CONCAT(up_dst.name, '!')
SELECT id, name, v, n FROM up_dst ORDER BY id
INSERT INTO up_dst (id, name, v) WITH s AS (SELECT id, name, v FROM up_src) SELECT id, name, v FROM s ON DUPLICATE KEY UPDATE v = up_dst.v + 1000
SELECT id, name, v, n FROM up_dst ORDER BY id
INSERT INTO up_dst (id, name, v) SELECT id, name, v FROM up_src ON DUPLICATE KEY UPDATE v = up_dst.v
INSERT INTO up_dst (id, name, v) SELECT id + 10, name, v FROM up_src ON DUPLICATE KEY UPDATE v = 0
SELECT id, name, v, n FROM up_dst ORDER BY id
INSERT INTO up_dst (id, name, v) SELECT id, name, v FROM up_src ORDER BY id LIMIT 2 ON DUPLICATE KEY UPDATE n = 7
SELECT id, n FROM up_dst WHERE id <= 3 ORDER BY id
INSERT INTO up_dst (id, name, v) SELECT s.id, s.name, s.v FROM up_src s JOIN up_src t ON t.id = s.id ON DUPLICATE KEY UPDATE v = 1
SELECT id, v FROM up_dst WHERE id <= 3 ORDER BY id
INSERT INTO up_dst (id, name, v) SELECT 1, 'x', 5 ON DUPLICATE KEY UPDATE v = VALUES(v)
SELECT id, name, v FROM up_dst WHERE id = 1
INSERT INTO up_dst (id, name, v) SELECT id, name, v FROM up_src WHERE id > 100 ON DUPLICATE KEY UPDATE v = 0
INSERT INTO up_dst (id, name, v) SELECT id, name, v FROM up_src ON DUPLICATE KEY UPDATE nosuch = 1
INSERT INTO up_dst (id, name, v) SELECT id, name, v FROM up_src ON DUPLICATE KEY
