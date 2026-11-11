# The error NUMBER of statements that failed for the right reason with another number, or because another check came first (R80). Run with --proto --errno (the number is what a client reads).
# ONLY_FULL_GROUP_BY (1055), an aggregate next to a plain column (1140): the engine told the right words and the number 1064
SELECT g, s FROM t GROUP BY g
SELECT id, g, COUNT(*) FROM t GROUP BY id WITH ROLLUP
SELECT g, SUM(i), SUM(SUM(i)) OVER () FROM t ORDER BY g
SELECT g, SUM(i) OVER (ORDER BY g) FROM t GROUP BY g
# a window function where none is read (3593), a window that is not defined (3579)
SELECT g, SUM(i) FROM t GROUP BY g HAVING RANK() OVER (ORDER BY g) = 1
SELECT id FROM t WHERE RANK() OVER (ORDER BY id) = 1
SELECT g FROM t GROUP BY RANK() OVER (ORDER BY g)
SELECT id, SUM(i) OVER (nosuchwindow ORDER BY id) FROM t ORDER BY id
SELECT id, SUM(i) OVER w FROM t WINDOW w AS (ORDER BY id) ORDER BY id
# RANGE frames that MySQL refuses: two sort keys or none (3587), a datetime key with a number (3588), a number key with an INTERVAL (3589)
--CREATE TABLE rf (id INT PRIMARY KEY, v INT, d DECIMAL(6,2), dt DATE, ts DATETIME, s VARCHAR(5))
--INSERT INTO rf VALUES (1,10,1.5,'2024-01-01','2024-01-01 00:00:00','a'),(2,15,2.5,'2024-01-03','2024-01-01 00:30:00','b'),(3,30,3.5,'2024-01-08','2024-01-01 02:00:00','c'),(4,NULL,NULL,NULL,NULL,'d')
SELECT id, SUM(v) OVER (ORDER BY v, id RANGE BETWEEN 5 PRECEDING AND CURRENT ROW) FROM rf
SELECT id, SUM(v) OVER (RANGE BETWEEN 5 PRECEDING AND CURRENT ROW) FROM rf
SELECT id, SUM(v) OVER (ORDER BY s RANGE BETWEEN 5 PRECEDING AND CURRENT ROW) FROM rf
SELECT id, SUM(v) OVER (ORDER BY v RANGE BETWEEN INTERVAL 1 DAY PRECEDING AND CURRENT ROW) FROM rf
SELECT id, SUM(v) OVER (ORDER BY dt RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) FROM rf
# the MOD operator (a MOD b is a % b) and a division by zero in the WHERE of an UPDATE (1365)
SELECT id, i MOD 3, i MOD 0 FROM t ORDER BY id
SELECT id FROM t WHERE i MOD 2 = 0 ORDER BY id
SELECT id FROM t WHERE i MOD (g + 1) = 0 ORDER BY id
UPDATE t SET g = g WHERE i MOD d = 0
UPDATE t SET g = g WHERE i % d = 0
# two tables that both have an id: the ambiguous column of the select list (1052) is told before the target table of the UPDATE (1093)
UPDATE t SET g = g WHERE id IN (SELECT id FROM u JOIN t ON u.id = t.id)
# a DATETIME(3) that rounds up past 9999-12-31 (1441)
--CREATE TABLE fs2 (id INT PRIMARY KEY, a DATETIME(3))
INSERT INTO fs2 VALUES (4, '9999-12-31 23:59:59.9996')
INSERT INTO fs2 VALUES (5, '9999-12-31 23:59:59.9994')
SELECT id, CAST(a AS CHAR) FROM fs2 ORDER BY id
# unsigned arithmetic that goes below zero is the error of the expression (1690); the column's own range is 1264
--CREATE TABLE un2 (id INT PRIMARY KEY, i INT UNSIGNED, n INT)
--INSERT INTO un2 VALUES (1, 4294967295, 0), (2, 0, 0), (3, 5, 0)
UPDATE un2 SET i = i - 5 WHERE id = 2
UPDATE un2 SET n = i - 5 WHERE id = 2
UPDATE un2 SET i = i - 5 WHERE id = 3
UPDATE un2 SET i = i + 1 WHERE id = 1
UPDATE un2 SET i = i * 2 WHERE id = 1
SELECT id, i, n FROM un2 ORDER BY id
# SELECT ... INTO a variable that is too short: 1265 (SET and DEFAULT say 1406)
--CREATE TABLE rt2 (id INT PRIMARY KEY, s VARCHAR(10))
--INSERT INTO rt2 VALUES (1, 'abc'), (2, 'a')
--CREATE PROCEDURE p_into2(IN k INT) BEGIN DECLARE t VARCHAR(2); SELECT s INTO t FROM rt2 WHERE id = k; SELECT t; END
--CREATE PROCEDURE p_set2() BEGIN DECLARE t VARCHAR(2); SET t = 'abc'; SELECT t; END
--CREATE PROCEDURE p_def() BEGIN DECLARE t VARCHAR(2) DEFAULT 'abc'; SELECT t; END
CALL p_into2(1)
CALL p_into2(2)
CALL p_set2()
CALL p_def()
# ALTER TABLE ... NOT NULL over a NULL: a change that keeps the type is made in place (1138), one that changes it says the NULL does not fit (1265, at its row)
--CREATE TABLE m1 (id INT PRIMARY KEY, f DOUBLE, i INT, s VARCHAR(5))
--INSERT INTO m1 VALUES (1, 2.5, 1, 'a'), (2, 1.5, 2, 'b'), (3, NULL, NULL, NULL)
ALTER TABLE m1 MODIFY f INT NOT NULL
ALTER TABLE m1 MODIFY f DOUBLE NOT NULL
ALTER TABLE m1 MODIFY i INT NOT NULL
ALTER TABLE m1 MODIFY i BIGINT NOT NULL
ALTER TABLE m1 MODIFY s VARCHAR(5) NOT NULL
ALTER TABLE m1 MODIFY s VARCHAR(9) NOT NULL
ALTER TABLE m1 MODIFY s TEXT NOT NULL
# ADD COLUMN ... NOT NULL fills the rows the table has with the zero of the type; a DATE / DATETIME has no zero that strict mode takes (1292), unless the table is empty
--CREATE TABLE ac (id INT PRIMARY KEY)
--INSERT INTO ac VALUES (1), (2)
ALTER TABLE ac ADD COLUMN c_int INT NOT NULL
ALTER TABLE ac ADD COLUMN c_tiny TINYINT UNSIGNED NOT NULL
ALTER TABLE ac ADD COLUMN c_dec DECIMAL(6,2) NOT NULL
ALTER TABLE ac ADD COLUMN c_dbl DOUBLE NOT NULL
ALTER TABLE ac ADD COLUMN c_vc VARCHAR(5) NOT NULL
ALTER TABLE ac ADD COLUMN c_txt TEXT NOT NULL
ALTER TABLE ac ADD COLUMN c_bool BOOLEAN NOT NULL
ALTER TABLE ac ADD COLUMN c_enum ENUM('x','y') NOT NULL
ALTER TABLE ac ADD COLUMN c_set SET('p','q') NOT NULL
ALTER TABLE ac ADD COLUMN c_date DATE NOT NULL
ALTER TABLE ac ADD COLUMN c_dt DATETIME NOT NULL
ALTER TABLE ac ADD COLUMN c_dt3 DATETIME(3) NOT NULL
ALTER TABLE ac ADD COLUMN c_ts TIMESTAMP NOT NULL
ALTER TABLE ac ADD COLUMN c_time TIME NOT NULL
ALTER TABLE ac ADD COLUMN c_json JSON NOT NULL
ALTER TABLE ac ADD COLUMN c_def INT NOT NULL DEFAULT 7
ALTER TABLE ac ADD COLUMN c_null INT
SELECT * FROM ac ORDER BY id
--CREATE TABLE ac_empty (id INT PRIMARY KEY)
ALTER TABLE ac_empty ADD COLUMN c_date DATE NOT NULL
ALTER TABLE ac_empty ADD COLUMN c_dt DATETIME NOT NULL
# a row that is a child and a parent at once: the references to its old key (1451) are looked for before the parent of its new value (1452)
--CREATE TABLE emp2 (id INT PRIMARY KEY, mgr INT, FOREIGN KEY (mgr) REFERENCES emp2(id))
--INSERT INTO emp2 VALUES (1,NULL),(7,7)
UPDATE emp2 SET id = 8, mgr = 8 WHERE id = 7
UPDATE emp2 SET mgr = 9 WHERE id = 7
UPDATE emp2 SET id = 9 WHERE id = 1
SELECT id, mgr FROM emp2 ORDER BY id
