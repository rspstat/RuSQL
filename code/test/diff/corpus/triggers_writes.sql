# Writes inside triggers and procedures (R71): BEFORE / AFTER INSERT, UPDATE, DELETE triggers that change NEW, write an audit table or another table that has a trigger of its own,
# REPLACE and ON DUPLICATE KEY UPDATE firing triggers, several rows in one statement, procedures that write in loops and with OUT parameters. A line is a statement, '--' a setup statement run on both.
--CREATE TABLE ta (id INT AUTO_INCREMENT PRIMARY KEY, v INT, note VARCHAR(20), upd INT DEFAULT 0, UNIQUE KEY (v))
--CREATE TABLE audit (n INT AUTO_INCREMENT PRIMARY KEY, what VARCHAR(40))
--CREATE TABLE tb (id INT PRIMARY KEY, c INT)
--CREATE TRIGGER ta_bi BEFORE INSERT ON ta FOR EACH ROW SET NEW.note = CONCAT('bi:', COALESCE(NEW.note, '-')), NEW.v = NEW.v * 10
--CREATE TRIGGER ta_ai AFTER INSERT ON ta FOR EACH ROW INSERT INTO audit (what) VALUES (CONCAT('ai ', NEW.id, ' ', NEW.v))
--CREATE TRIGGER ta_bu BEFORE UPDATE ON ta FOR EACH ROW SET NEW.upd = OLD.upd + 1, NEW.note = CONCAT(OLD.note, '>', NEW.v)
--CREATE TRIGGER ta_au AFTER UPDATE ON ta FOR EACH ROW INSERT INTO audit (what) VALUES (CONCAT('au ', OLD.v, '->', NEW.v))
--CREATE TRIGGER ta_bd BEFORE DELETE ON ta FOR EACH ROW INSERT INTO audit (what) VALUES (CONCAT('bd ', OLD.id))
--CREATE TRIGGER ta_ad AFTER DELETE ON ta FOR EACH ROW INSERT INTO tb VALUES (OLD.id, OLD.v)
--CREATE TRIGGER tb_ai AFTER INSERT ON tb FOR EACH ROW INSERT INTO audit (what) VALUES (CONCAT('tb ', NEW.id))
INSERT INTO ta (v, note) VALUES (1, 'x')
INSERT INTO ta (v) VALUES (2)
INSERT INTO ta (v, note) VALUES (3, 'a'), (4, 'b'), (5, NULL)
SELECT * FROM ta ORDER BY id
SELECT * FROM audit ORDER BY n
UPDATE ta SET v = v + 1 WHERE id = 1
UPDATE ta SET note = 'z' WHERE id IN (2, 3)
UPDATE ta SET v = v WHERE id = 4
UPDATE ta SET v = 999 WHERE id = 999
SELECT * FROM ta ORDER BY id
SELECT * FROM audit ORDER BY n
DELETE FROM ta WHERE id = 5
DELETE FROM ta WHERE id > 3
SELECT * FROM ta ORDER BY id
SELECT * FROM tb ORDER BY id
SELECT * FROM audit ORDER BY n
# a trigger that changes NEW inside an IF, a CASE and a compound body
--CREATE TABLE tc (id INT PRIMARY KEY, kind VARCHAR(10), qty INT, ts INT)
--CREATE TRIGGER tc_bi BEFORE INSERT ON tc FOR EACH ROW BEGIN IF NEW.qty < 0 THEN SET NEW.qty = 0; END IF; SET NEW.kind = CASE WHEN NEW.qty = 0 THEN 'empty' WHEN NEW.qty < 10 THEN 'few' ELSE 'many' END; SET NEW.ts = NEW.id * 2; END
--CREATE TRIGGER tc_bu BEFORE UPDATE ON tc FOR EACH ROW BEGIN IF NEW.qty < 0 THEN SET NEW.qty = OLD.qty; END IF; IF NEW.qty <> OLD.qty THEN SET NEW.kind = 'moved'; END IF; SET NEW.ts = OLD.ts + 1; END
INSERT INTO tc (id, kind, qty) VALUES (1, 'x', -5), (2, 'x', 5), (3, 'x', 50)
SELECT * FROM tc ORDER BY id
UPDATE tc SET qty = -1 WHERE id = 1
UPDATE tc SET qty = qty + 1 WHERE id IN (2, 3)
UPDATE tc SET kind = 'k' WHERE id = 1
SELECT * FROM tc ORDER BY id
UPDATE tc SET id = id + 10 WHERE id = 3
SELECT * FROM tc ORDER BY id
# REPLACE and ON DUPLICATE KEY UPDATE fire the triggers of the statements they are made of
--TRUNCATE TABLE audit
REPLACE INTO ta (id, v, note) VALUES (1, 77, 'r')
SELECT * FROM ta ORDER BY id
SELECT * FROM audit ORDER BY n
INSERT INTO ta (id, v, note) VALUES (1, 1, 'd') ON DUPLICATE KEY UPDATE note = 'dup'
SELECT * FROM ta ORDER BY id
SELECT * FROM audit ORDER BY n
# a trigger that fails rolls the whole statement back
--CREATE TABLE td (id INT PRIMARY KEY, n INT NOT NULL)
--CREATE TABLE te (id INT PRIMARY KEY)
--CREATE TRIGGER td_ai AFTER INSERT ON td FOR EACH ROW INSERT INTO te VALUES (NEW.id)
INSERT INTO td VALUES (1, 1), (2, 2)
INSERT INTO te VALUES (5)
INSERT INTO td VALUES (3, 3), (5, 5), (6, 6)
SELECT * FROM td ORDER BY id
SELECT * FROM te ORDER BY id
INSERT INTO td VALUES (7, NULL)
SELECT * FROM te ORDER BY id
# inside a transaction the trigger's writes belong to it
BEGIN
INSERT INTO td VALUES (8, 8)
ROLLBACK
SELECT * FROM td ORDER BY id
SELECT * FROM te ORDER BY id
# a stored procedure that writes: loops, IF, OUT parameters, SELECT ... INTO
--CREATE TABLE pw (id INT PRIMARY KEY, v INT)
--CREATE PROCEDURE p_fill(IN n INT, OUT total INT) BEGIN DECLARE i INT DEFAULT 1; SET total = 0; WHILE i <= n DO INSERT INTO pw VALUES (i, i * i); SET total = total + i * i; SET i = i + 1; END WHILE; END
--CREATE PROCEDURE p_bump(IN d INT) BEGIN UPDATE pw SET v = v + d WHERE id % 2 = 0; DELETE FROM pw WHERE v > 100; END
--CREATE PROCEDURE p_count(OUT c INT) BEGIN SELECT COUNT(*) INTO c FROM pw; END
--CREATE PROCEDURE p_fail() BEGIN INSERT INTO pw VALUES (100, 1); INSERT INTO pw VALUES (1, 1); INSERT INTO pw VALUES (101, 1); END
--CALL p_fill(6, @t)
SELECT @t
SELECT * FROM pw ORDER BY id
CALL p_bump(5)
SELECT * FROM pw ORDER BY id
--CALL p_count(@c)
SELECT @c
CALL p_fail()
SELECT * FROM pw ORDER BY id
BEGIN
CALL p_fill(2, @t2)
ROLLBACK
SELECT * FROM pw ORDER BY id
SELECT COUNT(*) FROM pw
