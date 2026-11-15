# R85 (2026-11-15): what the CREATE of a view wrote (the account, SQL SECURITY, ALGORITHM), the time a table or a view was made, a view over a derived table, a write that reads a column
# of a view that is an expression, and gtid_mode (mysqldump asks it). A line is a statement, '--' a setup statement run on both.
# Run: python vs_mysql.py <engine_server.exe> corpus/view_meta_r85.sql --proto --errno
# (a time is a different value on the two servers: the lines below compare only that it is there; ALGORITHM is in SHOW CREATE VIEW, whose text the two write differently -- the C++ test has it)
--DROP USER IF EXISTS 'bob'@'%'
--CREATE USER 'bob'@'%' IDENTIFIED BY 'pw'
--CREATE TABLE ua5 (id INT PRIMARY KEY, k INT, name VARCHAR(10))
--INSERT INTO ua5 VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 30, 'c')
--CREATE VIEW m5_plain AS SELECT id, k FROM ua5
--CREATE DEFINER=`bob`@`%` VIEW m5_bob AS SELECT id FROM ua5
--CREATE ALGORITHM = TEMPTABLE VIEW m5_temp AS SELECT id, k FROM ua5
--CREATE ALGORITHM = MERGE VIEW m5_merge AS SELECT id, k FROM ua5
--CREATE ALGORITHM = UNDEFINED VIEW m5_undef AS SELECT id, k FROM ua5
--CREATE SQL SECURITY INVOKER VIEW m5_inv AS SELECT id FROM ua5
--CREATE ALGORITHM = MERGE DEFINER = `bob`@`%` SQL SECURITY INVOKER VIEW m5_all AS SELECT id FROM ua5
--CREATE VIEW m5_over_temp AS SELECT id FROM m5_temp
--CREATE VIEW m5_derived AS SELECT d.id FROM (SELECT id FROM ua5) d
--CREATE VIEW m5_derived_group AS SELECT d.k FROM (SELECT k FROM ua5 GROUP BY k) d
--CREATE VIEW m5_derived_agg AS SELECT d.id, COUNT(*) AS n FROM (SELECT id FROM ua5) d GROUP BY d.id
--CREATE VIEW m5_expr AS SELECT id, k + 1 AS k1, name FROM ua5
--CREATE VIEW m5_expr2 AS SELECT id, k * 2 + id AS m FROM ua5
--CREATE VIEW m5_over_expr AS SELECT id, k1 FROM m5_expr
--CREATE TABLE ub5 (id INT PRIMARY KEY, ua_id INT, qty INT)
--INSERT INTO ub5 VALUES (1, 1, 5), (2, 2, 7)
--CREATE VIEW m5_dj_left AS SELECT a.id, d.k FROM ua5 a LEFT JOIN (SELECT id, k FROM ua5) d ON a.id = d.id
--CREATE VIEW m5_dj_inner AS SELECT a.id, d.k FROM ua5 a JOIN (SELECT id, k FROM ua5) d ON a.id = d.id
--CREATE VIEW m5_join AS SELECT ua5.id, ub5.qty FROM ua5 JOIN ub5 ON ub5.ua_id = ua5.id
--CREATE ALGORITHM = TEMPTABLE VIEW m5_tjoin AS SELECT ua5.id, ub5.qty FROM ua5 JOIN ub5 ON ub5.ua_id = ua5.id
--CREATE VIEW m5_ren AS SELECT id AS ident, k + 1 AS k1 FROM ua5
# --- the account, SQL SECURITY and the flag
SELECT table_name, definer, security_type, is_updatable FROM information_schema.views WHERE table_schema = 'v' AND table_name LIKE 'm5\_%' ORDER BY table_name
# --- the time a table or a view was made (a view has no update time)
SELECT table_name, table_type, create_time IS NOT NULL FROM information_schema.tables WHERE table_schema = 'v' AND (table_name LIKE 'm5\_%' OR table_name = 'ua5') ORDER BY table_name
SELECT table_name, update_time IS NULL FROM information_schema.tables WHERE table_schema = 'v' AND table_type = 'VIEW' AND table_name LIKE 'm5\_%' ORDER BY table_name
SELECT TIMESTAMPDIFF(MINUTE, create_time, NOW()) BETWEEN -1 AND 5 FROM information_schema.tables WHERE table_name = 'ua5'
SELECT TIMESTAMPDIFF(MINUTE, create_time, NOW()) BETWEEN -1 AND 5 FROM information_schema.tables WHERE table_name = 'm5_plain'
# --- TEMPTABLE takes no write; MERGE does
UPDATE m5_temp SET k = 11 WHERE id = 1
DELETE FROM m5_temp WHERE id = 99
INSERT INTO m5_temp (id, k) VALUES (9, 9)
UPDATE m5_over_temp SET id = 1 WHERE id = 99
UPDATE m5_merge SET k = 11 WHERE id = 1
SELECT id, k FROM ua5 ORDER BY id
# --- a view over a derived table says YES and takes no write
UPDATE m5_derived SET id = 5 WHERE id = 99
DELETE FROM m5_derived WHERE id = 99
INSERT INTO m5_derived (id) VALUES (50)
UPDATE m5_derived_group SET k = 1
# --- a join over a derived table: an inner join says YES and DELETE is 1395, an outer join NO; a TEMPTABLE join view takes nothing (DELETE 1288)
DELETE FROM m5_dj_inner WHERE id = 99
UPDATE m5_dj_inner SET k = 5 WHERE id = 99
INSERT INTO m5_dj_inner (id) VALUES (9)
DELETE FROM m5_dj_left WHERE id = 99
INSERT INTO m5_dj_left (id) VALUES (9)
DELETE FROM m5_join WHERE id = 99
DELETE FROM m5_tjoin WHERE id = 99
UPDATE m5_tjoin SET qty = 1 WHERE id = 1
INSERT INTO m5_tjoin (id, qty) VALUES (9, 9)
UPDATE m5_join SET qty = 1 WHERE id = 1
# --- a write that reads a column that is an expression
UPDATE m5_expr SET name = 'x' WHERE k1 = 12
SELECT id, k, name FROM ua5 ORDER BY id
UPDATE m5_expr SET name = 'y' WHERE k1 > 12 AND k1 < 32
SELECT id, k, name FROM ua5 ORDER BY id
UPDATE m5_expr SET name = 'z' WHERE id = k1 - 11
SELECT id, k, name FROM ua5 ORDER BY id
UPDATE m5_expr SET id = k1 WHERE id = 3
SELECT id, k, name FROM ua5 ORDER BY id
UPDATE m5_over_expr SET id = 40 WHERE k1 = 21
SELECT id, k, name FROM ua5 ORDER BY id
DELETE FROM m5_expr2 WHERE m = 51
SELECT id, k, name FROM ua5 ORDER BY id
DELETE FROM m5_over_expr WHERE k1 = 31
SELECT id, k, name FROM ua5 ORDER BY id
DELETE FROM m5_expr WHERE k1 = 999
UPDATE m5_expr SET name = 'abs' WHERE ABS(k1) = 21
UPDATE m5_expr SET name = 'lit' WHERE id = k1
UPDATE m5_ren SET ident = 90 WHERE ident = 2 AND k1 = 21
SELECT id, k, name FROM ua5 ORDER BY id
UPDATE m5_expr SET k1 = 5
INSERT INTO m5_expr (id) VALUES (70)
INSERT INTO m5_expr (id, k1) VALUES (71, 1)
# --- gtid_mode
SHOW VARIABLES LIKE 'gtid_mode'
SHOW VARIABLES LIKE 'enforce_gtid_consistency'
SELECT @@gtid_mode, @@enforce_gtid_consistency
