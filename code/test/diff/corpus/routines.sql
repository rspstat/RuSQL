# Stored functions and procedures (R42): the values given to a parameter or a variable fit its declared type (DECIMAL(8,2): 1.005 is 1.01; INT: 7 / 2 is 4), a function's
# result fits its RETURNS type, a procedure called with the wrong number of arguments is an error, a division by zero in an INSERT / UPDATE / function is an error, not a
# NULL written (strict mode); prepared statements with ? marks; LAST_INSERT_ID() after a trigger. A line is a statement, '--' a setup statement run on both.
# (@x := 7 and the other assignment operators are not supported: left out)
--CREATE TABLE rt (id INT PRIMARY KEY, v DECIMAL(8,2), n INT, s VARCHAR(10), d DATE)
--INSERT INTO rt VALUES (1, 10.50, 7, 'abc', '2024-03-05'), (2, 3.25, 10, 'defg', '2024-12-31')
--CREATE FUNCTION f_half(x INT) RETURNS INT DETERMINISTIC RETURN x / 2
--CREATE FUNCTION f_div(a INT, b INT) RETURNS DECIMAL(10,3) DETERMINISTIC RETURN a / b
--CREATE FUNCTION f_up(x INT UNSIGNED) RETURNS BIGINT UNSIGNED DETERMINISTIC RETURN x + 1
--CREATE FUNCTION f_day(x DATE) RETURNS VARCHAR(12) DETERMINISTIC RETURN CONCAT('d:', x)
--CREATE FUNCTION f_bare(x DECIMAL) RETURNS DECIMAL DETERMINISTIC RETURN x
--CREATE FUNCTION f_txt(x VARCHAR(3)) RETURNS VARCHAR(3) DETERMINISTIC RETURN x
--CREATE FUNCTION f_dbl(x DOUBLE) RETURNS DOUBLE DETERMINISTIC RETURN x * 3
--CREATE FUNCTION f_tiny(x TINYINT) RETURNS TINYINT DETERMINISTIC RETURN x + 100
SELECT f_half(7), f_half(8), f_half(NULL), f_half('9'), f_half(7.5)
SELECT f_div(1, 3), f_div(10, 4), f_div(1, 0), f_div(NULL, 2)
SELECT f_up(5), f_up(0), f_up(4294967295)
SELECT f_up(-1)
SELECT f_day('2024-3-5'), f_day(NULL)
SELECT f_day('2024-02-30')
SELECT f_bare(3.7), f_bare(12.5), f_bare(NULL)
SELECT f_txt('abc'), f_txt('abcd')
SELECT f_dbl(1.1), f_dbl(0.1), f_dbl(2)
SELECT f_tiny(5), f_tiny(27), f_tiny(28)
SELECT f_tiny(200)
SELECT id, f_half(n), f_div(n, id + 1) FROM rt ORDER BY id
SELECT id FROM rt WHERE f_half(n) = 4 ORDER BY id
SELECT SUM(f_half(n)), AVG(f_div(n, 3)) FROM rt
--CREATE PROCEDURE p_set(OUT o1 INT, OUT o2 DECIMAL(6,2), OUT o3 VARCHAR(4), OUT o4 DATE) BEGIN SET o1 = 7 / 2; SET o2 = 10 / 3; SET o3 = 'abcdefgh'; SET o4 = '2024-1-2'; END
CALL p_set(@a, @b, @c, @d)
SELECT @a, @b, @c, @d
--CREATE PROCEDURE p_into(IN k INT) BEGIN DECLARE m INT DEFAULT 0; DECLARE w DECIMAL(5,1) DEFAULT 0; DECLARE t VARCHAR(2); SELECT AVG(v) INTO w FROM rt; SELECT COUNT(*) / 2 INTO m FROM rt; SELECT s INTO t FROM rt WHERE id = k; SELECT m, w, t; END
CALL p_into(1)
CALL p_into(2)
--CREATE PROCEDURE p_loop(IN n INT, OUT total DECIMAL(10,2)) BEGIN DECLARE i INT DEFAULT 1; DECLARE acc DECIMAL(10,2) DEFAULT 0; WHILE i <= n DO SET acc = acc + i / 3; SET i = i + 1; END WHILE; SET total = acc; END
CALL p_loop(4, @t4)
SELECT @t4
CALL p_loop('3', @t3)
SELECT @t3
--CREATE PROCEDURE p_inout(INOUT x INT, INOUT y VARCHAR(3)) BEGIN SET x = x * 2.5; SET y = CONCAT(y, 'zzzz'); END
SET @x = 3, @y = 'a'
CALL p_inout(@x, @y)
SELECT @x, @y
--CREATE PROCEDURE p_ins(IN a INT, IN b DECIMAL(5,2)) BEGIN DECLARE c DECIMAL(5,2); SET c = a * b; INSERT INTO rt VALUES (a, c, a, 'p', '2024-01-01'); END
CALL p_ins(10, 1.255)
CALL p_ins(11, 999.99)
SELECT id, v, n FROM rt WHERE id >= 10 ORDER BY id

--CREATE TABLE pr (id INT PRIMARY KEY AUTO_INCREMENT, v DECIMAL(8,2), n INT, s VARCHAR(10))
--CREATE PROCEDURE p_add(IN p_v DECIMAL(8,2), IN p_n INT, IN p_s VARCHAR(10)) BEGIN INSERT INTO pr (v, n, s) VALUES (p_v, p_n, p_s); END
--CREATE PROCEDURE p_calc(IN a DECIMAL(5,2), IN b INT, OUT r DECIMAL(10,3)) BEGIN DECLARE t DECIMAL(5,2) DEFAULT 0; DECLARE i INT DEFAULT 0; DECLARE txt VARCHAR(5) DEFAULT 'x'; SET t = a * 3; SET i = b / 3; SET txt = CONCAT('abc', 'defgh'); SET r = t + i; INSERT INTO pr (v, n, s) VALUES (t, i, txt); END
CALL p_add(1.005, '7.6', 'abc')
CALL p_add(NULL, NULL, NULL)
CALL p_add(100000, 1, 'x')
CALL p_add(1, 99999999999, 'x')
CALL p_add(1, 1, 'way too long text')
SELECT id, v, n, s FROM pr ORDER BY id
CALL p_calc(1.005, 10, @r)
SELECT @r
SELECT id, v, n, s FROM pr ORDER BY id
CALL p_calc(12.5, 4, @r2)
SELECT @r2
CALL p_calc(1)
CALL p_calc(1, 2, @a, @b)

--CREATE TABLE dz (id INT PRIMARY KEY, a INT, b INT)
--INSERT INTO dz VALUES (0, 8, 2)
INSERT INTO dz VALUES (1, 1/0, 5)
INSERT INTO dz VALUES (2, 4, 0 + 1 % 0)
INSERT INTO dz SELECT 3, 1/0, 1
INSERT INTO dz (id, a) SELECT 4, a DIV 0 FROM dz
UPDATE dz SET a = a / (b - 2)
UPDATE dz SET a = 1 / 0 WHERE id = 99
SELECT id, a, b FROM dz ORDER BY id
REPLACE INTO dz VALUES (0, 1 / 0, 1)
SELECT id, a, b FROM dz ORDER BY id
SELECT 1/0, 5 DIV 0, 5 % 0, MOD(5, 0)
SELECT a / (b - 2) FROM dz
SELECT SUM(a / 0) FROM dz
INSERT INTO dz VALUES (5, 0, 1) ON DUPLICATE KEY UPDATE a = 1 / 0
INSERT IGNORE INTO dz VALUES (6, 1 / 0, 1)
SELECT id, a, b FROM dz ORDER BY id

--CREATE TABLE pp (id INT PRIMARY KEY AUTO_INCREMENT, s VARCHAR(40), n INT, d DECIMAL(6,2), t DATE)
PREPARE p1 FROM 'INSERT INTO pp (s, n, d, t) VALUES (?, ?, ?, ?)'
SET @s = 'it''s ok?', @n = 5, @d = 1.5, @t = '2024-01-02'
EXECUTE p1 USING @s, @n, @d, @t
SET @s = 'a?b', @n = NULL, @d = '2.5', @t = NULL
EXECUTE p1 USING @s, @n, @d, @t
SET @s = '', @n = -3, @d = 0, @t = '2024-02-29'
EXECUTE p1 USING @s, @n, @d, @t
SELECT id, s, n, d, t FROM pp ORDER BY id
SELECT LAST_INSERT_ID()
PREPARE p2 FROM 'SELECT id FROM pp WHERE s = ? OR n > ? ORDER BY id'
SET @a = 'a?b', @b = 0
EXECUTE p2 USING @a, @b
SET @a = 'x', @b = NULL
EXECUTE p2 USING @a, @b
PREPARE p3 FROM 'SELECT ''what?'' AS q, ? AS v, CONCAT(?, ''?'')'
SET @v = 'one', @w = 'two'
EXECUTE p3 USING @v, @w
PREPARE p4 FROM 'UPDATE pp SET n = n + ? WHERE id = ?'
SET @inc = 10, @id = 1
EXECUTE p4 USING @inc, @id
SELECT n FROM pp WHERE id = 1
SELECT ROW_COUNT()
PREPARE p5 FROM 'SELECT id FROM pp WHERE id IN (?, ?) ORDER BY id'
SET @i1 = 1, @i2 = 3
EXECUTE p5 USING @i1, @i2
PREPARE p6 FROM 'SELECT * FROM pp LIMIT ?'
SET @lim = 2
EXECUTE p6 USING @lim

--CREATE TABLE inv (id INT PRIMARY KEY AUTO_INCREMENT, sku VARCHAR(10) UNIQUE, qty INT NOT NULL DEFAULT 0, price DECIMAL(8,2), note VARCHAR(20))
--INSERT INTO inv (sku, qty, price) VALUES ('A', 10, 5.50), ('B', 0, 12.00), ('C', 25, 0.99), ('D', 3, NULL)
--CREATE FUNCTION f_total(p_qty INT, p_price DECIMAL(8,2)) RETURNS DECIMAL(10,2) DETERMINISTIC RETURN p_qty * IFNULL(p_price, 0)
--CREATE FUNCTION f_label(p_qty INT) RETURNS VARCHAR(10) DETERMINISTIC RETURN CASE WHEN p_qty = 0 THEN 'out' WHEN p_qty < 5 THEN 'low' ELSE 'ok' END
SELECT sku, f_total(qty, price), f_label(qty) FROM inv ORDER BY id
SELECT f_total(2, 1.005), f_total(NULL, 1), f_label(NULL), f_label(5), UPPER(f_label(0))
SELECT sku FROM inv WHERE f_label(qty) = 'low'
SELECT f_label(qty), COUNT(*), SUM(f_total(qty, price)) FROM inv GROUP BY f_label(qty) ORDER BY 1
SET @x = 5, @y = 'abc', @z = NULL
SET @n = (SELECT COUNT(*) FROM inv)
SELECT @n, @n * 2
SELECT sku, qty FROM inv WHERE qty > @x ORDER BY id
SET @s = 0
UPDATE inv SET qty = qty + 1 WHERE id <= 2
SELECT SUM(qty) INTO @s FROM inv
SELECT @s
SELECT sku INTO @first FROM inv ORDER BY id LIMIT 1
SELECT @first
PREPARE st FROM 'SELECT sku, qty FROM inv WHERE qty > ? ORDER BY id'
SET @q = 4
EXECUTE st USING @q
SET @q = 20
EXECUTE st USING @q
DEALLOCATE PREPARE st
PREPARE ins FROM 'INSERT INTO inv (sku, qty, price) VALUES (?, ?, ?)'
SET @k = 'E', @qq = 9, @pp = 1.25
EXECUTE ins USING @k, @qq, @pp
SELECT sku, qty, price FROM inv WHERE sku = 'E'
SELECT LAST_INSERT_ID()
DELETE FROM inv WHERE sku = 'E'
SELECT COUNT(*) FROM inv

# triggers: a row written by a trigger leaves LAST_INSERT_ID() as the statement set it
--CREATE TABLE acct (id INT PRIMARY KEY, owner VARCHAR(20), bal DECIMAL(10,2) NOT NULL DEFAULT 0, kind ENUM('chk','sav') DEFAULT 'chk', opened DATE, CHECK (bal >= -100))
--CREATE TABLE log (id INT PRIMARY KEY AUTO_INCREMENT, aid INT, op VARCHAR(10), amt DECIMAL(10,2), at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)
--INSERT INTO acct VALUES (1,'ann',100.00,'chk','2020-01-01'),(2,'bob',50.50,'sav','2021-02-02'),(3,'cy',0,'chk','2022-03-03')
--CREATE TRIGGER t_ai AFTER INSERT ON acct FOR EACH ROW INSERT INTO log (aid, op, amt) VALUES (NEW.id, 'open', NEW.bal)
--CREATE TRIGGER t_ad AFTER DELETE ON acct FOR EACH ROW INSERT INTO log (aid, op, amt) VALUES (OLD.id, 'close', OLD.bal)
INSERT INTO acct VALUES (4,'dee',10.005,'sav','2023-04-04')
SELECT * FROM acct ORDER BY id
SELECT aid, op, amt FROM log ORDER BY id
UPDATE acct SET bal = bal + 0.004 WHERE id = 1
SELECT id, bal FROM acct ORDER BY id
UPDATE acct SET bal = bal - 250 WHERE id = 2
UPDATE acct SET bal = bal - 150 WHERE id = 3
SELECT id, bal FROM acct ORDER BY id
UPDATE acct SET bal = CASE id WHEN 1 THEN bal * 2 WHEN 2 THEN -bal ELSE bal END
SELECT id, bal FROM acct ORDER BY id
UPDATE acct SET bal = bal + 1, owner = UPPER(owner) WHERE kind = 'chk' ORDER BY id DESC LIMIT 1
SELECT id, owner, bal FROM acct ORDER BY id
DELETE FROM acct WHERE bal < 0 OR id = 4
SELECT id FROM acct ORDER BY id
SELECT aid, op, amt FROM log ORDER BY id
UPDATE acct a JOIN log l ON l.aid = a.id AND l.op = 'open' SET a.bal = a.bal + l.amt
SELECT id, bal FROM acct ORDER BY id
DELETE a FROM acct a JOIN log l ON l.aid = a.id WHERE l.op = 'close'
SELECT id FROM acct ORDER BY id
INSERT INTO acct (id, owner) VALUES (10, 'x'), (11, 'y')
INSERT INTO acct (id, owner, bal) VALUES (12, 'z', -500)
SELECT id, owner, bal, kind FROM acct ORDER BY id
INSERT INTO acct (id, owner, kind) VALUES (13, 'k', 'bad')
UPDATE acct SET kind = 'sav' WHERE id IN (10, 13)
SELECT kind, COUNT(*) FROM acct GROUP BY kind ORDER BY kind
START TRANSACTION
INSERT INTO acct (id, owner) VALUES (20, 'tx')
UPDATE acct SET bal = 999 WHERE id = 10
SELECT id, bal FROM acct WHERE id IN (10, 20) ORDER BY id
ROLLBACK
SELECT id, bal FROM acct WHERE id IN (10, 20) ORDER BY id
START TRANSACTION
INSERT INTO acct (id, owner) VALUES (21, 'a')
SAVEPOINT s1
INSERT INTO acct (id, owner) VALUES (22, 'b')
ROLLBACK TO SAVEPOINT s1
INSERT INTO acct (id, owner) VALUES (23, 'c')
COMMIT
SELECT id FROM acct WHERE id >= 20 ORDER BY id
INSERT INTO acct (id, owner) VALUES (21, 'dup')
INSERT INTO acct (id, owner) VALUES (30, 'a'), (31, 'b'), (21, 'dup')
SELECT id FROM acct WHERE id >= 30 ORDER BY id
REPLACE INTO acct (id, owner, bal) VALUES (21, 'repl', 5)
SELECT id, owner, bal FROM acct WHERE id = 21
INSERT INTO acct (id, owner, bal) VALUES (21, 'ups', 7) ON DUPLICATE KEY UPDATE bal = bal + VALUES(bal), owner = CONCAT(owner, '+')
SELECT id, owner, bal FROM acct WHERE id = 21
TRUNCATE TABLE log
INSERT INTO acct (id, owner) VALUES (40, 'after truncate')
SELECT aid, op FROM log
SELECT ROW_COUNT(), LAST_INSERT_ID()

# a branch that is not taken is not worked out: no division by zero in it
--CREATE TABLE dz2 (id INT PRIMARY KEY, a INT, b INT)
--INSERT INTO dz2 VALUES (1, 8, 2), (2, 9, 0), (3, NULL, 0)
INSERT INTO dz2 VALUES (4, IF(1 = 1, 5, 1 / 0), 1)
INSERT INTO dz2 VALUES (5, CASE WHEN 1 = 2 THEN 1 / 0 ELSE 6 END, 1)
INSERT INTO dz2 VALUES (6, COALESCE(7, 1 / 0), 1)
INSERT INTO dz2 VALUES (7, 5 / NULLIF(0, 0), 1)
UPDATE dz2 SET a = CASE WHEN b = 0 THEN 0 ELSE a / b END WHERE id < 4
UPDATE dz2 SET b = a / NULLIF(b, 0) WHERE id < 4
SELECT id, a, b FROM dz2 ORDER BY id
INSERT IGNORE INTO dz2 VALUES (8, 1 / 0, 1)
SELECT id, a, b FROM dz2 WHERE id = 8
UPDATE dz2 SET a = a / b WHERE id = 1
UPDATE dz2 SET a = a / b
SELECT id, a, b FROM dz2 ORDER BY id

# LAST_INSERT_ID() after an INSERT in the body of a procedure; a CALL answers with the result table of its SELECT even when a statement that has none follows it
--CREATE TABLE pt (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(20), qty INT)
--INSERT INTO pt (name, qty) VALUES ('a', 5), ('b', 0), ('c', 12), ('d', 3)
--CREATE PROCEDURE pt_add() BEGIN INSERT INTO pt (name, qty) VALUES ('e', 1); SELECT LAST_INSERT_ID(); END
CALL pt_add()
SELECT LAST_INSERT_ID()
--CREATE PROCEDURE pt_say(IN s VARCHAR(10)) BEGIN SELECT CONCAT(s, '!'), LENGTH(s), UPPER(s); SET @out = CONCAT('got ', s); END
CALL pt_say('hi')
SELECT @out
CALL pt_say(NULL)
SELECT @out
--CREATE PROCEDURE pt_count() BEGIN SELECT COUNT(*) FROM pt; INSERT INTO pt (name, qty) VALUES ('f', 2); END
CALL pt_count()
SELECT COUNT(*) FROM pt
--CREATE PROCEDURE pt_sel() BEGIN INSERT INTO pt (name, qty) SELECT 'g', 9; SELECT LAST_INSERT_ID(); END
CALL pt_sel()

# NEW of a BEFORE INSERT trigger holds the values as the columns will hold them (1.005 into a DECIMAL(8,2) is 1.01, so the total is 3.03)
--CREATE TABLE li (id INT PRIMARY KEY AUTO_INCREMENT, qty INT, price DECIMAL(8,2), total DECIMAL(8,2), note VARCHAR(5))
--CREATE TRIGGER li_bi BEFORE INSERT ON li FOR EACH ROW SET NEW.total = NEW.qty * NEW.price
INSERT INTO li (qty, price) VALUES (3, 1.005), (2, 10.555)
SELECT id, qty, price, total FROM li ORDER BY id
INSERT INTO li (qty, price) VALUES (100000, 9999.99)
SELECT COUNT(*) FROM li
--CREATE TRIGGER li_bi2 BEFORE INSERT ON li FOR EACH ROW SET NEW.note = CONCAT('abc', NEW.qty)
INSERT INTO li (qty, price) VALUES (7, 1)
SELECT id, note FROM li ORDER BY id

# EXECUTE with a different number of variables than ? marks is an error
PREPARE pe FROM 'SELECT ? + 1'
SET @e1 = 1, @e2 = 2
EXECUTE pe USING @e1, @e2
EXECUTE pe
EXECUTE pe USING @e1
