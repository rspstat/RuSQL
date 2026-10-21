# types and strict-mode checks on INSERT
--CREATE TABLE ty (id INT PRIMARY KEY AUTO_INCREMENT, ti TINYINT, si SMALLINT, n INT, bg BIGINT, dc DECIMAL(5,2), fl DOUBLE, vc VARCHAR(5), tx TEXT, dt DATE, dtm DATETIME, b BOOLEAN)
INSERT INTO ty (ti) VALUES (100)
INSERT INTO ty (ti) VALUES (200)
INSERT INTO ty (ti) VALUES (-129)
INSERT INTO ty (si) VALUES (40000)
INSERT INTO ty (n) VALUES (2147483648)
INSERT INTO ty (n) VALUES ('12abc')
INSERT INTO ty (n) VALUES ('abc')
INSERT INTO ty (n) VALUES (3.7)
INSERT INTO ty (n) VALUES ('7')
INSERT INTO ty (dc) VALUES (123.456)
INSERT INTO ty (dc) VALUES (1234.5)
INSERT INTO ty (dc) VALUES (-999.999)
INSERT INTO ty (vc) VALUES ('abcde')
INSERT INTO ty (vc) VALUES ('abcdef')
INSERT INTO ty (vc) VALUES (12345)
INSERT INTO ty (dt) VALUES ('2024-02-30')
INSERT INTO ty (dt) VALUES ('2024-02-29')
INSERT INTO ty (dt) VALUES ('2024-2-9')
INSERT INTO ty (dt) VALUES ('abc')
INSERT INTO ty (dt) VALUES ('2024-01-15 10:20:30')
INSERT INTO ty (dtm) VALUES ('2024-01-15')
INSERT INTO ty (dtm) VALUES ('2024-01-15 25:00:00')
INSERT INTO ty (b) VALUES (TRUE)
INSERT INTO ty (b) VALUES (2)
INSERT INTO ty (b) VALUES ('yes')
INSERT INTO ty (fl) VALUES (1.5e3)
INSERT INTO ty (fl) VALUES ('1.5e3')
INSERT INTO ty (tx) VALUES (NULL)
SELECT id, ti, si, n, bg, dc, fl, vc, tx, dt, dtm, b FROM ty ORDER BY id
# NOT NULL, defaults, auto increment
--CREATE TABLE nn (id INT PRIMARY KEY AUTO_INCREMENT, a INT NOT NULL, b INT NOT NULL DEFAULT 7, c VARCHAR(5) DEFAULT 'x', d INT DEFAULT NULL)
INSERT INTO nn (a) VALUES (1)
INSERT INTO nn (b) VALUES (2)
INSERT INTO nn (a, b) VALUES (NULL, 3)
INSERT INTO nn (a, b) VALUES (4, NULL)
INSERT INTO nn (a, c) VALUES (5, DEFAULT)
INSERT INTO nn (id, a) VALUES (10, 6)
INSERT INTO nn (a) VALUES (7)
INSERT INTO nn (id, a) VALUES (5, 8)
INSERT INTO nn (a) VALUES (9), (10), (11)
INSERT INTO nn VALUES (NULL, 12, 13, 'y', 14)
INSERT INTO nn VALUES (0, 15, 16, 'z', 17)
SELECT * FROM nn ORDER BY id
UPDATE nn SET a = NULL WHERE id = 1
UPDATE nn SET b = DEFAULT WHERE id = 1
UPDATE nn SET c = 'toolong' WHERE id = 1
SELECT * FROM nn WHERE id <= 2 ORDER BY id
DELETE FROM nn WHERE id = 10
INSERT INTO nn (a) VALUES (99)
SELECT MAX(id) FROM nn
TRUNCATE TABLE nn
INSERT INTO nn (a) VALUES (1)
SELECT id, a FROM nn
# unique and primary key
--CREATE TABLE uq (id INT PRIMARY KEY, u INT UNIQUE, n VARCHAR(5), UNIQUE KEY uk (n, u))
INSERT INTO uq VALUES (1, 1, 'a'), (2, 2, 'a')
INSERT INTO uq VALUES (3, 1, 'b')
INSERT INTO uq VALUES (3, NULL, 'b'), (4, NULL, 'b')
INSERT INTO uq VALUES (5, 5, 'a'), (6, 5, 'c')
INSERT INTO uq VALUES (7, 7, 'a'), (7, 8, 'a')
UPDATE uq SET u = 2 WHERE id = 1
UPDATE uq SET u = u + 1 WHERE id IN (1, 2)
UPDATE uq SET id = 2 WHERE id = 1
UPDATE uq SET n = 'a', u = 2 WHERE id = 3
SELECT * FROM uq ORDER BY id
# foreign keys
--CREATE TABLE par (id INT PRIMARY KEY, nm VARCHAR(5))
--CREATE TABLE chi (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES par(id) ON DELETE CASCADE ON UPDATE CASCADE)
--CREATE TABLE chr (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES par(id))
--INSERT INTO par VALUES (1, 'a'), (2, 'b'), (3, 'c')
INSERT INTO chi VALUES (1, 1), (2, 1), (3, 2)
INSERT INTO chi VALUES (4, 9)
INSERT INTO chi VALUES (5, NULL)
INSERT INTO chr VALUES (1, 3)
DELETE FROM par WHERE id = 3
UPDATE par SET id = 30 WHERE id = 3
DELETE FROM par WHERE id = 1
SELECT * FROM chi ORDER BY id
UPDATE par SET id = 20 WHERE id = 2
SELECT * FROM chi ORDER BY id
SELECT * FROM par ORDER BY id
DROP TABLE par
# check constraints and generated
--CREATE TABLE ck (id INT PRIMARY KEY, age INT CHECK (age >= 0 AND age < 150), s VARCHAR(3), CHECK (s <> 'bad'))
INSERT INTO ck VALUES (1, 30, 'ok')
INSERT INTO ck VALUES (2, -1, 'ok')
INSERT INTO ck VALUES (3, 200, 'ok')
INSERT INTO ck VALUES (4, 10, 'bad')
INSERT INTO ck VALUES (5, NULL, 'ok')
UPDATE ck SET age = 500 WHERE id = 1
SELECT * FROM ck ORDER BY id
# update / delete variants
--CREATE TABLE up (id INT PRIMARY KEY, a INT, b INT, g INT)
--INSERT INTO up VALUES (1, 10, 1, 1), (2, 20, 2, 1), (3, 30, 3, 2), (4, 40, 4, 2), (5, 50, 5, 3)
UPDATE up SET a = a + b, b = a WHERE g = 1
SELECT * FROM up ORDER BY id
UPDATE up SET a = 0 ORDER BY id DESC LIMIT 2
SELECT * FROM up ORDER BY id
UPDATE up SET b = b * 2 WHERE id IN (SELECT id FROM up WHERE a > 5)
DELETE FROM up WHERE g = 3 LIMIT 5
DELETE FROM up ORDER BY id DESC LIMIT 1
SELECT * FROM up ORDER BY id
UPDATE up u1 JOIN up u2 ON u1.id = u2.id - 1 SET u1.b = u2.a
SELECT * FROM up ORDER BY id
UPDATE up SET a = NULL WHERE id = 1
UPDATE up SET a = a + 1
SELECT * FROM up ORDER BY id
INSERT INTO up SELECT id + 10, a, b, g FROM up
SELECT COUNT(*), SUM(a) FROM up
INSERT INTO up (id, a) SELECT id + 100, a FROM up WHERE a IS NOT NULL
SELECT id, a, b, g FROM up ORDER BY id
# transactions
--CREATE TABLE tx (id INT PRIMARY KEY, v INT)
--INSERT INTO tx VALUES (1, 10)
BEGIN
INSERT INTO tx VALUES (2, 20)
SELECT COUNT(*) FROM tx
ROLLBACK
SELECT COUNT(*) FROM tx
START TRANSACTION
UPDATE tx SET v = v + 1
COMMIT
SELECT * FROM tx
BEGIN
INSERT INTO tx VALUES (1, 99)
ROLLBACK
SELECT * FROM tx
BEGIN
SAVEPOINT s1
INSERT INTO tx VALUES (3, 30)
ROLLBACK TO s1
COMMIT
SELECT * FROM tx
# UNSIGNED integers: 0 up to 255 / 65535 / 4294967295 / 18446744073709551615, nothing below 0 (not even -0.4), exact comparison above the signed BIGINT maximum
--CREATE TABLE un (id INT PRIMARY KEY, t TINYINT UNSIGNED, s SMALLINT UNSIGNED, i INT UNSIGNED, b BIGINT UNSIGNED, n INT)
INSERT INTO un VALUES (1, 255, 65535, 4294967295, 18446744073709551615, -5)
INSERT INTO un VALUES (2, 0, 0, 0, 0, 5)
INSERT INTO un (id, b) VALUES (3, 9223372036854775808)
INSERT INTO un (id, t) VALUES (4, 256)
INSERT INTO un (id, t) VALUES (4, -1)
INSERT INTO un (id, s) VALUES (4, 65536)
INSERT INTO un (id, s) VALUES (4, -1)
INSERT INTO un (id, i) VALUES (4, 4294967296)
INSERT INTO un (id, i) VALUES (4, -1)
INSERT INTO un (id, b) VALUES (4, 18446744073709551616)
INSERT INTO un (id, b) VALUES (4, -1)
INSERT INTO un (id, i) VALUES (4, 1 - 2)
INSERT INTO un (id, i) VALUES (4, 4294967295 + 1)
INSERT INTO un (id, i) VALUES (4, '12')
INSERT INTO un (id, i) VALUES (5, 2.4)
INSERT INTO un (id, i) VALUES (6, 2.6)
INSERT INTO un (id, i) VALUES (7, -0.4)
SELECT id, t, s, i, b, n FROM un ORDER BY id
UPDATE un SET i = i - 5 WHERE id = 2
UPDATE un SET i = i + 1 WHERE id = 1
UPDATE un SET t = t * 2 WHERE id = 1
UPDATE un SET i = i - 5 WHERE id = 1
SELECT id, i FROM un ORDER BY id
SELECT id FROM un WHERE b > 5 ORDER BY id
SELECT id FROM un WHERE b > 9223372036854775807 ORDER BY id
SELECT id, b FROM un ORDER BY b DESC, id
SELECT MAX(b), MIN(b), COUNT(b) FROM un
SELECT id FROM un WHERE b = 18446744073709551615
SELECT id FROM un WHERE i BETWEEN 0 AND 100 ORDER BY id
ALTER TABLE un ADD COLUMN x SMALLINT UNSIGNED
INSERT INTO un (id, x) VALUES (20, -1)
INSERT INTO un (id, x) VALUES (20, 65535)
ALTER TABLE un MODIFY COLUMN n INT UNSIGNED
UPDATE un SET n = 7 WHERE id = 1
ALTER TABLE un MODIFY COLUMN n INT UNSIGNED
INSERT INTO un (id, n) VALUES (21, -1)
SELECT id, n, x FROM un ORDER BY id

# a text read as a number without being wholly one ('abc', '12abc', '0x10', '--5') is the number it starts with in a SELECT and error 1292 where a value is written
# (INSERT, UPDATE, REPLACE); blanks are 0, a number may have blanks around it, a sign, a fraction, an exponent (R43)
--CREATE TABLE tn_src (id INT PRIMARY KEY, s VARCHAR(20), n INT)
--INSERT INTO tn_src VALUES (1,'abc',1),(2,'12',2),(3,'12abc',3),(4,'',4),(5,' 7',5),(6,'7 ',6),(7,'1e3',7),(8,'.5',8),(9,'5.',9),(10,'0x10',10),(11,'+5',11),(12,'--5',12),(13,'   ',13)
--CREATE TABLE tn_dst (id INT PRIMARY KEY, v DOUBLE)
SELECT id, s + 0 FROM tn_src ORDER BY id
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 1
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 2
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 3
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 4
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 5
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 6
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 7
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 8
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 9
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 10
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 11
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 12
INSERT INTO tn_dst SELECT id, s + 1 FROM tn_src WHERE id = 13
INSERT INTO tn_dst VALUES (20, 'abc' + 1)
INSERT INTO tn_dst VALUES (21, '3' + 1)
INSERT IGNORE INTO tn_dst VALUES (22, 'abc' + 1)
INSERT IGNORE INTO tn_dst SELECT 50, 'abc' + 1
INSERT IGNORE INTO tn_dst SELECT 51, 1 / 0
INSERT INTO tn_dst VALUES (31, 'abc' * 1)
INSERT INTO tn_dst VALUES (32, 5 - 'abc')
INSERT INTO tn_dst VALUES (33, '' + 1)
UPDATE tn_src SET n = s + 1 WHERE id = 1
UPDATE tn_src SET n = s * 2 WHERE id = 3
UPDATE tn_src SET n = s + 1 WHERE id = 2
SELECT id, v FROM tn_dst ORDER BY id
SELECT id, n FROM tn_src WHERE id <= 3 ORDER BY id
SELECT s + 1 FROM tn_src WHERE id = 1
REPLACE INTO tn_src VALUES (14, 'r', 'abc' + 1)
--CREATE TABLE tn_dt (d DATE)
INSERT INTO tn_dt VALUES ('2024-01-31' + INTERVAL 1 DAY)
SELECT * FROM tn_dt
# a date or a time read as a number is not refused (MySQL turns it into 20240305; the number here is the one it starts with)
--CREATE TABLE tn_tm (id INT PRIMARY KEY, d DATE, t TIME, dt DATETIME)
--INSERT INTO tn_tm VALUES (1, '2024-03-05', '12:30:00', '2024-03-05 12:30:00')
INSERT INTO tn_dst SELECT 60, d + 0 FROM tn_tm
INSERT INTO tn_dst SELECT 61, t + 0 FROM tn_tm
INSERT INTO tn_dst SELECT 62, dt + 0 FROM tn_tm

# := where a value is assigned (SET, UPDATE ... SET, INSERT ... SET, ON DUPLICATE KEY UPDATE)
SET @tn_a := 5
SELECT @tn_a
UPDATE tn_src SET n := 9 WHERE id = 2
SELECT n FROM tn_src WHERE id = 2
UPDATE tn_src JOIN tn_dst ON tn_dst.id = tn_src.id SET tn_src.n := 77
SELECT id, n FROM tn_src WHERE n = 77 ORDER BY id
INSERT INTO tn_dst SET id = 40, v := 2
SELECT v FROM tn_dst WHERE id = 40
INSERT INTO tn_dst VALUES (40, 1) ON DUPLICATE KEY UPDATE v := 5
SELECT v FROM tn_dst WHERE id = 40

# the rows a statement affects, as MySQL counts them (R44): an UPDATE counts the rows it changed, ON DUPLICATE KEY UPDATE 1 for a row inserted and 2 for a row it changed (0 for one it
# set to what it holds), REPLACE 2 for a row it replaced
--CREATE TABLE ar (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(10), u INT UNIQUE, n INT DEFAULT 0)
INSERT INTO ar (v, u) VALUES ('a', 1), ('b', 2), ('c', 3)
SELECT ROW_COUNT() AS ar_1
INSERT IGNORE INTO ar (v, u) VALUES ('x', 1), ('y', 10)
SELECT ROW_COUNT() AS ar_2
INSERT INTO ar (v, u) VALUES ('a2', 1) ON DUPLICATE KEY UPDATE v = 'changed'
SELECT ROW_COUNT() AS ar_3
INSERT INTO ar (v, u) VALUES ('a3', 1) ON DUPLICATE KEY UPDATE v = 'changed'
SELECT ROW_COUNT() AS ar_4
INSERT INTO ar (v, u) VALUES ('p', 2), ('q', 20), ('r', 3) ON DUPLICATE KEY UPDATE v = CONCAT(v, '!')
SELECT ROW_COUNT() AS ar_5
INSERT INTO ar (v, u) VALUES ('p', 2), ('q', 20) ON DUPLICATE KEY UPDATE v = v
SELECT ROW_COUNT() AS ar_6
REPLACE INTO ar (v, u) VALUES ('r1', 1), ('r2', 30)
SELECT ROW_COUNT() AS ar_7
UPDATE ar SET n = 5 WHERE u <= 3
SELECT ROW_COUNT() AS ar_8
UPDATE ar SET n = 5 WHERE u <= 3
SELECT ROW_COUNT() AS ar_9
DELETE FROM ar WHERE u > 20
SELECT ROW_COUNT() AS ar_10
SELECT ROW_COUNT() AS ar_11
INSERT INTO ar (v, u) SELECT v, u + 100 FROM ar
SELECT ROW_COUNT() AS ar_12
INSERT IGNORE INTO ar (v, u) SELECT v, u FROM ar WHERE u < 10
SELECT ROW_COUNT() AS ar_13
