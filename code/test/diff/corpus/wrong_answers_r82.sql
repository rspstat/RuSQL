# R82: the answers that were wrong without an error (RECOMMAND.md "R81" rows, now fixed): hexadecimal / bit literals, COMMENT, SELECT * over a LATERAL derived table, SHA2 of 224 / 384 / 512 bits,
# MEDIUMINT / TINYTEXT / TEXT / BLOB sizes and CHAR(n). Run: python vs_mysql.py <engine_server.exe> corpus/wrong_answers_r82.sql --proto --errno
# A line is a statement, '--' a setup statement run on both.
--CREATE TABLE bl (id INT PRIMARY KEY, b BLOB, s VARCHAR(10))
--CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL COMMENT 'it''s a name') COMMENT='parents'
--CREATE TABLE chi (id INT PRIMARY KEY, pid INT)
--INSERT INTO par VALUES (1,'a'),(2,'b'),(3,'c')
--INSERT INTO chi VALUES (1,1),(2,1),(3,2)
--CREATE TABLE ty (id INT PRIMARY KEY AUTO_INCREMENT, m MEDIUMINT, mu MEDIUMINT UNSIGNED, tt TINYTEXT, tx TEXT, mt MEDIUMTEXT, tb TINYBLOB, c CHAR(5), v VARCHAR(5), d CHAR, n NCHAR(3))
# --- hexadecimal and bit literals: bytes as a string, an unsigned integer where a number is asked
SELECT 0x41, 0x4142, X'4142', x'41', HEX(0x4142), HEX(X'41'), LENGTH(0x4142), CONCAT('x', 0x41)
SELECT 0x41 + 0, 0x4142 + 0, 0x10 * 2, 0x01 + 1, b'101' + 0, 0b101 + 0, B'1000001', 0x123
SELECT _utf8mb4'abc', _binary'abc', _latin1 'abc', N'abc', UPPER(_utf8mb4'abc'), 'a' = _binary'a'
SELECT '0x41' + 0, HEX('0x41')
SELECT 0x01 = 1, 0x41 = 65, 0x41 = 'A', 0x41 > 64
SELECT 65 = 0x41, 'A' = 0x41, 0x4142 = 'AB', 0x41 = 0x41
SELECT CAST(0x41 AS UNSIGNED), CAST(0x4142 AS SIGNED), CAST(0x41 AS CHAR), 0x41 + 0.5, 0x41 * 1.0, CONCAT(0x41, 1)
SELECT 0xFF + 0
SELECT 0x00, LENGTH(0x00), HEX(0x00), b'', LENGTH(b''), X'', LENGTH(X'')
SELECT ASCII(0x41), ORD(0x41), CHAR_LENGTH(0x4142)
--CREATE TABLE hn (id INT PRIMARY KEY, a INT, s VARCHAR(10))
--INSERT INTO hn VALUES (1, 1, 'A'), (2, 65, 'B'), (3, 16706, 'AB')
SELECT id FROM hn WHERE a = 0x01
SELECT id FROM hn WHERE a = 0x41
SELECT id FROM hn WHERE a = 0x4142
SELECT id FROM hn WHERE 0x41 = a
SELECT id FROM hn WHERE a >= 0x41 ORDER BY id
SELECT id FROM hn WHERE s = 0x41
INSERT INTO bl VALUES (1, 0x4142, 0x41)
INSERT INTO bl VALUES (2, _binary'xy', N'z')
INSERT INTO bl VALUES (3, X'0141', 0x01)
SELECT id, HEX(b), HEX(s), LENGTH(b) FROM bl ORDER BY id
SELECT id FROM bl WHERE b = 0x4142
UPDATE bl SET s = 0x42 WHERE id = 1
SELECT s FROM bl WHERE id = 1
SELECT X'4'
SELECT X'4G'
# --- COMMENT: kept and shown
SELECT table_comment FROM information_schema.tables WHERE table_schema = 'v' AND table_name = 'par'
SELECT column_comment FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'par' ORDER BY ordinal_position
SHOW CREATE TABLE par
ALTER TABLE par COMMENT = 'people'
ALTER TABLE par ADD COLUMN age INT COMMENT 'years'
SELECT table_comment FROM information_schema.tables WHERE table_schema = 'v' AND table_name = 'par'
SELECT column_name, column_comment FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'par' ORDER BY ordinal_position
ALTER TABLE par MODIFY name VARCHAR(30) NOT NULL
SELECT column_comment FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'par' AND column_name = 'name'
ALTER TABLE par CHANGE age years INT COMMENT 'new name'
SELECT column_comment FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'par' AND column_name = 'years'
CREATE TABLE par2 LIKE par
SELECT table_comment FROM information_schema.tables WHERE table_schema = 'v' AND table_name = 'par2'
SELECT column_name, column_comment FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'par2' ORDER BY ordinal_position
CREATE TABLE IF NOT EXISTS par (id INT) COMMENT='replaced?'
SELECT table_comment FROM information_schema.tables WHERE table_schema = 'v' AND table_name = 'par'
# --- LATERAL: `*` lists the columns of the derived table
SELECT * FROM par, LATERAL (SELECT COUNT(*) c FROM chi WHERE chi.pid = par.id) l ORDER BY par.id
SELECT * FROM par JOIN LATERAL (SELECT COUNT(*) c FROM chi WHERE chi.pid = par.id) l ON TRUE ORDER BY par.id
SELECT * FROM par CROSS JOIN LATERAL (SELECT COUNT(*) c FROM chi WHERE chi.pid = par.id) l ORDER BY par.id
SELECT l.* FROM par, LATERAL (SELECT COUNT(*) c FROM chi WHERE chi.pid = par.id) l ORDER BY par.id
SELECT par.id, l.* FROM par, LATERAL (SELECT COUNT(*) c FROM chi WHERE chi.pid = par.id) l ORDER BY par.id
SELECT * FROM par LEFT JOIN LATERAL (SELECT id AS cid FROM chi WHERE chi.pid = par.id ORDER BY id LIMIT 1) l ON TRUE ORDER BY par.id
SELECT * FROM par, LATERAL (SELECT 5 AS x) a, LATERAL (SELECT par.id * 10 AS y) b WHERE par.id = 2
SELECT * FROM par2, LATERAL (SELECT COUNT(*) c FROM chi WHERE chi.pid = par2.id) l
SELECT par2.id, l.c FROM par2, LATERAL (SELECT COUNT(*) c FROM chi WHERE chi.pid = par2.id) l
# --- SHA2: every length MySQL has, NULL for the rest
SELECT SHA2('a', 224), SHA2('a', 256), SHA2('a', 384), SHA2('a', 512), SHA2('a', 0)
SELECT SHA2('', 224), SHA2('', 384), SHA2('', 512)
SELECT SHA2('abc', 224), SHA2('abc', 384), SHA2('abc', 512)
SELECT SHA2('abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu', 512)
SELECT SHA2('abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu', 384)
SELECT SHA2('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 224)
SELECT SHA2(REPEAT('a', 1000), 512), SHA2(REPEAT('a', 1000), 384), SHA2(REPEAT('a', 1000), 224), SHA2(REPEAT('a', 1000), 256)
SELECT SHA2('abc', 123), SHA2('abc', 1), SHA2(NULL, 256), SHA2('abc', NULL)
# --- MEDIUMINT
INSERT INTO ty (m, mu) VALUES (8388607, 16777215)
INSERT INTO ty (m, mu) VALUES (-8388608, 0)
INSERT INTO ty (m) VALUES (8388608)
INSERT INTO ty (m) VALUES (-8388609)
INSERT INTO ty (mu) VALUES (16777216)
INSERT INTO ty (mu) VALUES (-1)
UPDATE ty SET m = 9000000 WHERE id = 1
SELECT m, mu FROM ty ORDER BY id
SELECT data_type, column_type, numeric_precision, numeric_scale FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'ty' AND column_name IN ('m', 'mu') ORDER BY column_name
# --- TEXT and BLOB sizes
INSERT INTO ty (tt) VALUES (REPEAT('x', 255))
INSERT INTO ty (tt) VALUES (REPEAT('x', 256))
INSERT INTO ty (tx) VALUES (REPEAT('x', 65535))
INSERT INTO ty (tx) VALUES (REPEAT('x', 65536))
INSERT INTO ty (mt) VALUES (REPEAT('x', 65536))
INSERT INTO ty (tb) VALUES (REPEAT('x', 255))
INSERT INTO ty (tb) VALUES (REPEAT('x', 256))
UPDATE ty SET tt = REPEAT('y', 300) WHERE id = 3
SELECT data_type, character_maximum_length FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'ty' AND column_name IN ('tt', 'tx', 'mt', 'tb') ORDER BY column_name
# --- CHAR(n)
INSERT INTO ty (c, v, d, n) VALUES ('ab  ', 'ab  ', 'x', 'xy ')
SELECT CHAR_LENGTH(c), CHAR_LENGTH(v), CHAR_LENGTH(n), CONCAT(c, '|'), CONCAT(v, '|') FROM ty WHERE c IS NOT NULL
INSERT INTO ty (c) VALUES ('abcdef')
INSERT INTO ty (c) VALUES ('abcde   ')
INSERT INTO ty (d) VALUES ('xy')
SELECT c FROM ty WHERE c = 'ab'
UPDATE ty SET c = 'zz   ' WHERE c = 'ab'
SELECT CHAR_LENGTH(c) FROM ty WHERE c = 'zz'
SHOW COLUMNS FROM ty
--ALTER TABLE ty MODIFY v CHAR(5)
SELECT CHAR_LENGTH(v) FROM ty WHERE v IS NOT NULL
SELECT column_name, data_type, column_type FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'ty' AND column_name IN ('c', 'v', 'd', 'n') ORDER BY column_name
