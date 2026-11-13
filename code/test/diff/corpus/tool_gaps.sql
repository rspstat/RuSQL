# R81 audit (2026-11-14): gaps found while checking RECOMMAND.md against real MySQL 8.4.9 -- what mysqldump / mysqlshow / schema tools / hand-written SQL send. NOT in the
# scoreboard (make_scoreboard.py): every group below still differs. Run: python vs_mysql.py <engine_server.exe> corpus/tool_gaps.sql --proto --errno
# A line is a statement, '--' a setup statement run on both. The groups match the rows of RECOMMAND.md ("R81").
--CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL, UNIQUE KEY uq_name (name))
--CREATE TABLE chi (id INT PRIMARY KEY AUTO_INCREMENT, pid INT NOT NULL, qty INT DEFAULT 1 COMMENT 'amount', CONSTRAINT fk_chi_par FOREIGN KEY (pid) REFERENCES par (id), CONSTRAINT ck_qty CHECK (qty >= 0), KEY ix_pid_qty (pid, qty)) COMMENT='children'
--INSERT INTO par VALUES (1,'a'),(2,'b')
--INSERT INTO chi (pid, qty) VALUES (1,5),(2,7)
--CREATE VIEW vw AS SELECT id, name FROM par
--CREATE TABLE bl (id INT PRIMARY KEY, b BLOB, s VARCHAR(10))
--CREATE PROCEDURE sp_one(IN x INT, OUT y INT) SET y = x + 1
--CREATE FUNCTION fn_two(x INT) RETURNS INT DETERMINISTIC RETURN x * 2
--CREATE TRIGGER tr_par BEFORE INSERT ON par FOR EACH ROW SET NEW.name = LOWER(NEW.name)
--CREATE TABLE ty (m MEDIUMINT, t TINYTEXT, c CHAR(5), mt MEDIUMTEXT, mb MEDIUMBLOB, nc NCHAR(3), y YEAR)
# --- 1. mysqldump (default options send LOCK TABLES ... LOCAL, SELECT /*!40001 SQL_NO_CACHE */, SHOW CREATE DATABASE; --routines / --triggers send SHOW CREATE PROCEDURE / FUNCTION / TRIGGER)
LOCK TABLES `par` READ /*!32311 LOCAL */
UNLOCK TABLES
SELECT /*!40001 SQL_NO_CACHE */ * FROM `par` ORDER BY id
SHOW CREATE DATABASE IF NOT EXISTS `v`
SHOW CREATE PROCEDURE sp_one
SHOW CREATE FUNCTION fn_two
SHOW CREATE TRIGGER tr_par
SHOW TRIGGERS
# --- 2. schema tools (mysqlshow db table sends SHOW FULL COLUMNS ... FROM db LIKE; DBeaver / ORMs read information_schema)
SHOW /*!32332 FULL */ COLUMNS FROM `par` FROM `v` LIKE '%'
SHOW FULL TABLES
SHOW PROCEDURE STATUS WHERE Db = 'v'
SHOW FUNCTION STATUS WHERE Db = 'v'
SELECT table_name, constraint_name, column_name FROM information_schema.key_column_usage WHERE table_schema = 'v' ORDER BY table_name, constraint_name, ordinal_position
SELECT routine_name, routine_type FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
SELECT specific_name, ordinal_position, parameter_mode, parameter_name FROM information_schema.parameters WHERE specific_schema = 'v' ORDER BY specific_name, ordinal_position
SELECT trigger_name, event_manipulation, event_object_table, action_timing FROM information_schema.triggers WHERE trigger_schema = 'v'
SELECT constraint_name FROM information_schema.check_constraints WHERE constraint_schema = 'v' ORDER BY constraint_name
SELECT table_name, is_updatable, view_definition IS NULL OR view_definition = '' FROM information_schema.views WHERE table_schema = 'v'
SELECT collation_name FROM information_schema.collations WHERE collation_name = 'utf8mb4_bin'
# --- 3. hand-written SQL: a string as alias, select modifiers
SELECT id AS 'a b' FROM par ORDER BY id
SELECT id 'a2' FROM par ORDER BY id
SELECT SQL_NO_CACHE id FROM par ORDER BY id
SELECT HIGH_PRIORITY id FROM par ORDER BY id
SELECT SQL_SMALL_RESULT id FROM par ORDER BY id
SELECT STRAIGHT_JOIN par.id FROM par JOIN chi ON chi.pid = par.id ORDER BY par.id
# --- 4. silent wrong answers: hex / bit literals stored as their digits; LATERAL dropped without an error; SHA2 with a length other than 256 is NULL
INSERT INTO bl VALUES (1, 0x4142, 0x41)
SELECT id, HEX(b), s, LENGTH(b) FROM bl
SELECT 0x41 + 0, 0b101 + 0
SELECT X'41'
SELECT b'101'
SELECT _utf8mb4'abc'
SELECT N'abc'
INSERT INTO bl VALUES (2, _binary'xy', 'z')
SELECT * FROM par, LATERAL (SELECT COUNT(*) c FROM chi WHERE chi.pid = par.id) l ORDER BY par.id
SELECT SHA2('a', 224), SHA2('a', 384), SHA2('a', 512)
# --- 5. COMMENT is accepted and dropped
SELECT table_comment FROM information_schema.tables WHERE table_schema = 'v' AND table_name = 'chi'
SELECT column_comment FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'chi' AND column_name = 'qty'
ALTER TABLE par COMMENT = 'x'
SELECT table_comment FROM information_schema.tables WHERE table_schema = 'v' AND table_name = 'par'
# --- 6. sql_mode is always strict / ONLY_FULL_GROUP_BY (apps that SET sql_mode = '' for legacy SQL)
SET sql_mode = ''
SELECT pid, qty FROM chi GROUP BY pid
SELECT id, COUNT(*) FROM chi
INSERT INTO chi (pid, qty) VALUES (1, 'abc')
INSERT INTO par VALUES (9, REPEAT('x', 30))
INSERT INTO chi (pid, qty) VALUES (1, 1 / 0)
SET sql_mode = 'ANSI_QUOTES'
SELECT "id" FROM par ORDER BY 1
SET sql_mode = 'STRICT_TRANS_TABLES'
# --- 7. column types the engine stores as another type (range / padding / limits)
INSERT INTO ty (m) VALUES (9000000)
INSERT INTO ty (t) VALUES (REPEAT('x', 300))
INSERT INTO ty (c) VALUES ('ab  ')
SELECT CONCAT(c, '|'), CHAR_LENGTH(c) FROM ty WHERE c IS NOT NULL
INSERT INTO ty (y) VALUES (1800)
SHOW COLUMNS FROM ty
# --- 8. small syntax: CONVERT ... USING, DO, ROW(), TABLE, VALUES ROW, INSERT ... AS alias, DROP CHECK, ALTER CHECK
SELECT CONVERT('abc' USING utf8mb4)
DO 1
SELECT ROW(1, 2) = ROW(1, 2)
TABLE par ORDER BY id LIMIT 1
VALUES ROW(1, 2), ROW(3, 4)
INSERT INTO par VALUES (1, 'z') AS new ON DUPLICATE KEY UPDATE name = new.name
ALTER TABLE chi DROP CHECK ck_qty
ALTER TABLE chi ADD CONSTRAINT ck3 CHECK (qty < 99) NOT ENFORCED
SELECT "q""t"
SELECT {d '2024-01-01'}
# --- 9. variables and numbers
SET NAMES utf8mb4 COLLATE utf8mb4_unicode_ci
SELECT @@collation_connection, @@auto_increment_offset
SELECT 18446744073709551615 + 1
SELECT CAST(18446744073709551615 AS SIGNED)
