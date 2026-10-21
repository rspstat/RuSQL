# exploratory probe 3: DML forms, transactions, views, routines, misc
--CREATE TABLE acct (id INT PRIMARY KEY AUTO_INCREMENT, owner VARCHAR(10) NOT NULL, bal DECIMAL(10,2) NOT NULL DEFAULT 0, tag VARCHAR(5) UNIQUE)
--INSERT INTO acct (owner, bal, tag) VALUES ('ann', 100, 'a'), ('bob', 50, 'b'), ('cy', 0, NULL)
SELECT LAST_INSERT_ID()
INSERT INTO acct (owner, bal) VALUES ('di', 10), ('ed', 20)
SELECT LAST_INSERT_ID(), ROW_COUNT()
UPDATE acct SET bal = bal + 1 WHERE id <= 3
SELECT ROW_COUNT()
DELETE FROM acct WHERE owner = 'ed'
SELECT ROW_COUNT(), FOUND_ROWS()
SELECT owner FROM acct ORDER BY id LIMIT 2
SELECT FOUND_ROWS()
SELECT SQL_CALC_FOUND_ROWS owner FROM acct ORDER BY id LIMIT 2
SELECT FOUND_ROWS()
INSERT INTO acct (id, owner) VALUES (3, 'dup')
INSERT IGNORE INTO acct (id, owner) VALUES (3, 'dup'), (10, 'new')
SELECT ROW_COUNT()
INSERT INTO acct (id, owner, bal) VALUES (1, 'x', 1) ON DUPLICATE KEY UPDATE bal = bal + VALUES(bal)
SELECT bal FROM acct WHERE id = 1
REPLACE INTO acct (id, owner, bal) VALUES (2, 'bob2', 5)
SELECT owner, bal FROM acct WHERE id = 2
UPDATE acct SET bal = bal * 2 ORDER BY id DESC LIMIT 2
SELECT id, bal FROM acct ORDER BY id
DELETE FROM acct WHERE bal < 15 ORDER BY id LIMIT 1
SELECT COUNT(*) FROM acct
# transactions
START TRANSACTION
INSERT INTO acct (owner) VALUES ('t1')
SAVEPOINT s1
INSERT INTO acct (owner) VALUES ('t2')
ROLLBACK TO SAVEPOINT s1
SELECT owner FROM acct WHERE owner LIKE 't%'
COMMIT
SELECT owner FROM acct WHERE owner LIKE 't%'
BEGIN
UPDATE acct SET bal = 0
ROLLBACK
SELECT SUM(bal) > 0 FROM acct
SET autocommit = 0
INSERT INTO acct (owner) VALUES ('auto0')
ROLLBACK
SELECT COUNT(*) FROM acct WHERE owner = 'auto0'
SET autocommit = 1
SELECT @@autocommit, @@sql_mode LIKE '%STRICT%'
# views
CREATE VIEW rich AS SELECT id, owner, bal FROM acct WHERE bal > 40
SELECT * FROM rich ORDER BY id
UPDATE rich SET bal = bal + 1 WHERE id = 1
SELECT bal FROM acct WHERE id = 1
INSERT INTO rich (owner, bal) VALUES ('viewins', 99)
SELECT COUNT(*) FROM acct WHERE owner = 'viewins'
CREATE OR REPLACE VIEW rich AS SELECT id, owner FROM acct
SELECT * FROM rich WHERE id = 1
ALTER VIEW rich AS SELECT id FROM acct
SELECT * FROM rich WHERE id = 1
DROP VIEW rich
# prepared statements and routines
PREPARE st FROM 'SELECT owner FROM acct WHERE id = ?'
SET @pid = 1
EXECUTE st USING @pid
DEALLOCATE PREPARE st
CREATE FUNCTION tax(x DECIMAL(10,2)) RETURNS DECIMAL(10,2) DETERMINISTIC RETURN x * 0.1
SELECT tax(100)
SELECT owner, tax(bal) FROM acct WHERE id = 1
DROP FUNCTION tax
CREATE PROCEDURE addbal(IN p INT, IN amt DECIMAL(10,2)) UPDATE acct SET bal = bal + amt WHERE id = p
CALL addbal(1, 5)
SELECT bal FROM acct WHERE id = 1
DROP PROCEDURE addbal
CREATE TABLE audit (id INT PRIMARY KEY AUTO_INCREMENT, note VARCHAR(30))
CREATE TRIGGER trg_acct AFTER UPDATE ON acct FOR EACH ROW INSERT INTO audit (note) VALUES (CONCAT('upd ', NEW.id, ' ', OLD.bal, '->', NEW.bal))
UPDATE acct SET bal = bal + 1 WHERE id = 1
SELECT note FROM audit
DROP TRIGGER trg_acct
SELECT COUNT(*) FROM audit
# locking reads, table maintenance
SELECT owner FROM acct WHERE id = 1 FOR UPDATE
SELECT owner FROM acct WHERE id = 1 LOCK IN SHARE MODE
ANALYZE TABLE acct
CHECK TABLE acct
OPTIMIZE TABLE acct
SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'acct'
SHOW CREATE TABLE audit
SHOW TABLE STATUS LIKE 'acct'
SHOW VARIABLES LIKE 'autocommit'
SHOW STATUS LIKE 'Threads_connected'
SHOW WARNINGS
SHOW ENGINES
SHOW CHARACTER SET LIKE 'utf8mb4'
SHOW COLLATION LIKE 'utf8mb4_0900_ai_ci'
SELECT @@version LIKE '8%', @@global.sql_mode IS NOT NULL, @@session.autocommit
USE v
SELECT DATABASE()

# the OK packet's affected rows (R44): the rows an UPDATE changed, ON DUPLICATE KEY UPDATE 1 / 2 / 0, REPLACE 2 for a replaced row
--CREATE TABLE ar (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(10), u INT UNIQUE, n INT DEFAULT 0)
INSERT INTO ar (v, u) VALUES ('a', 1), ('b', 2), ('c', 3)
UPDATE ar SET n = 5 WHERE u <= 3
UPDATE ar SET n = 5 WHERE u <= 3
UPDATE ar SET n = 6 WHERE u <= 2
INSERT INTO ar (v, u) VALUES ('p', 2), ('q', 20), ('r', 3) ON DUPLICATE KEY UPDATE v = CONCAT(v, '!')
INSERT INTO ar (v, u) VALUES ('p', 2), ('q', 20) ON DUPLICATE KEY UPDATE v = v
REPLACE INTO ar (v, u) VALUES ('r1', 1), ('r2', 30)
INSERT INTO ar (v, u) VALUES ('t1', 500), ('t1', 500), ('t2', 501) ON DUPLICATE KEY UPDATE n = n + 1
DELETE FROM ar WHERE u >= 500
INSERT IGNORE INTO ar (v, u) VALUES ('d', 2), ('e', 90)
# the statements of a trigger do not change what the statement that fired it affected
--CREATE TABLE lgp (id INT PRIMARY KEY AUTO_INCREMENT, k INT)
INSERT INTO lgp (k) VALUES (1), (1), (1)
CREATE TRIGGER ar_au_p AFTER UPDATE ON ar FOR EACH ROW UPDATE lgp SET k = k + 1
CREATE TRIGGER ar_ad_p AFTER DELETE ON ar FOR EACH ROW UPDATE lgp SET k = k + 1
INSERT INTO ar (v, u) VALUES ('g1', 800), ('g2', 801)
UPDATE ar SET n = 77 WHERE u >= 800
UPDATE ar SET n = 77 WHERE u >= 800
DELETE FROM ar WHERE u >= 800
