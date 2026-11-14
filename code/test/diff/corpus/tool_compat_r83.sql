# R83 (2026-11-15): what mysqldump, mysqlshow, schema tools (DBeaver, Workbench, ORMs) and hand-written SQL send -- found by the R81 audit (corpus/tool_gaps.sql held the gaps)
# A line is a statement, '--' a setup statement run on both. Run: python vs_mysql.py <engine_server.exe> corpus/tool_compat_r83.sql --proto --errno
# (SHOW TRIGGERS, SHOW PROCEDURE / FUNCTION STATUS and SHOW CREATE TRIGGER have the time of the creation in a column: tool_check_r83.py compares them without it)
--CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL, UNIQUE KEY uq_name (name))
--CREATE TABLE chi (id INT PRIMARY KEY AUTO_INCREMENT, pid INT NOT NULL, qty INT DEFAULT 1 COMMENT 'amount', CONSTRAINT fk_chi_par FOREIGN KEY (pid) REFERENCES par (id), CONSTRAINT ck_qty CHECK (qty >= 0), KEY ix_pid_qty (pid, qty)) COMMENT='children'
--CREATE TABLE cu (a INT, b INT, c INT, UNIQUE KEY ab (a, b), UNIQUE (c))
--CREATE TABLE ckt (id INT, q INT CHECK (q > 0), CONSTRAINT named CHECK (id < 9), CHECK (id <> q))
--INSERT INTO par VALUES (1,'a'),(2,'b'),(3,'c')
--INSERT INTO chi (pid, qty) VALUES (1,5),(2,7),(2,1)
--CREATE VIEW vw AS SELECT id, name FROM par
--CREATE VIEW vw_agg AS SELECT pid, COUNT(*) AS n FROM chi GROUP BY pid
--CREATE VIEW vw_join AS SELECT par.id, chi.qty FROM par JOIN chi ON chi.pid = par.id
--CREATE VIEW vw_chk AS SELECT id, name FROM par WHERE id > 1 WITH CHECK OPTION
--CREATE PROCEDURE sp_one(IN x INT, OUT y INT) SET y = x + 1
--CREATE PROCEDURE sp_two(IN a INT, INOUT b VARCHAR(20), OUT c DECIMAL(8,2)) NO SQL DETERMINISTIC COMMENT 'it''s' BEGIN SELECT 1; END
--CREATE FUNCTION fn_two(x INT) RETURNS INT DETERMINISTIC RETURN x * 2
--CREATE FUNCTION fn_three(s VARCHAR(20), d DECIMAL(8,2)) RETURNS VARCHAR(30) CHARSET utf8mb4 COMMENT 'hello' READS SQL DATA BEGIN RETURN CONCAT(s, 'x'); END
--CREATE FUNCTION fn_up(a TINYINT UNSIGNED) RETURNS TINYINT UNSIGNED DETERMINISTIC RETURN a
--CREATE TRIGGER tr_par BEFORE INSERT ON par FOR EACH ROW SET NEW.name = LOWER(NEW.name)
--CREATE TRIGGER tr_chi AFTER DELETE ON chi FOR EACH ROW BEGIN UPDATE par SET name = name WHERE id = OLD.pid; END
# --- 1. mysqldump sends these (default options: LOCK TABLES ... LOCAL; --single-transaction: SELECT /*!40001 SQL_NO_CACHE */; the header: SHOW CREATE DATABASE; --routines: SHOW CREATE PROCEDURE / FUNCTION)
LOCK TABLES `par` READ /*!32311 LOCAL */
UNLOCK TABLES
LOCK TABLES `par` WRITE, `chi` READ LOCAL
UNLOCK TABLES
LOCK TABLES `par` LOW_PRIORITY WRITE
UNLOCK TABLES
SELECT /*!40001 SQL_NO_CACHE */ * FROM `par` ORDER BY id
SHOW CREATE DATABASE `v`
SHOW CREATE DATABASE IF NOT EXISTS `v`
SHOW CREATE SCHEMA v
SHOW CREATE DATABASE nodb
SHOW CREATE PROCEDURE sp_one
SHOW CREATE PROCEDURE sp_two
SHOW CREATE PROCEDURE `sp_one`
SHOW CREATE PROCEDURE nope
SHOW CREATE FUNCTION fn_two
SHOW CREATE FUNCTION fn_three
SHOW CREATE FUNCTION fn_up
SHOW CREATE FUNCTION nope
# --- 2. schema tools: the routines, their parameters, the triggers, the CHECK constraints, the UNIQUE keys, the views
SELECT routine_name, routine_type, data_type, dtd_identifier, character_maximum_length, numeric_precision, numeric_scale, routine_body, routine_definition FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
SELECT routine_name, is_deterministic, sql_data_access, security_type, routine_comment, definer, external_language, parameter_style, sql_path FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
SELECT specific_name, ordinal_position, parameter_mode, parameter_name, data_type, dtd_identifier, character_maximum_length, numeric_precision, numeric_scale, routine_type FROM information_schema.parameters WHERE specific_schema = 'v' ORDER BY specific_name, ordinal_position
SELECT trigger_name, event_manipulation, event_object_table, action_order, action_statement, action_orientation, action_timing, action_reference_old_row, action_reference_new_row, definer FROM information_schema.triggers WHERE trigger_schema = 'v' ORDER BY trigger_name
SELECT constraint_schema, constraint_name FROM information_schema.check_constraints WHERE constraint_schema = 'v' ORDER BY constraint_name
SELECT table_name, constraint_name, constraint_type FROM information_schema.table_constraints WHERE table_schema = 'v' AND table_name IN ('cu', 'par', 'ckt') ORDER BY table_name, constraint_name
SELECT table_name, constraint_name, column_name, ordinal_position FROM information_schema.key_column_usage WHERE table_schema = 'v' ORDER BY table_name, constraint_name, ordinal_position
SELECT table_name, is_updatable, check_option, view_definition IS NULL OR view_definition = '' FROM information_schema.views WHERE table_schema = 'v' ORDER BY table_name
SELECT collation_name, id, is_default, is_compiled, sortlen, pad_attribute FROM information_schema.collations WHERE collation_name IN ('utf8mb4_general_ci', 'utf8mb4_bin', 'utf8mb4_unicode_ci', 'utf8mb4_0900_ai_ci', 'utf8mb4_0900_as_cs', 'utf8mb4_0900_bin') ORDER BY id
SHOW /*!32332 FULL */ COLUMNS FROM `par` FROM `v` LIKE '%'
SHOW FULL COLUMNS FROM chi LIKE 'q%'
SHOW FULL TABLES
SHOW FULL TABLES WHERE Table_type = 'VIEW'
SHOW FULL TABLES WHERE Table_type = 'BASE TABLE'
SHOW FULL TABLES LIKE 'v%'
# --- 3. hand-written SQL: a string as the name of a column, the words that only advise the optimizer
SELECT id AS 'a b' FROM par ORDER BY id
SELECT id 'a2' FROM par ORDER BY id
SELECT 1 AS 'x', 2 'y z', name AS "dq" FROM par ORDER BY id LIMIT 1
SELECT 'a' 'b'
SELECT 'a' 'b' 'c' AS 'abc'
SELECT id AS 'it''s' FROM par ORDER BY id LIMIT 1
SELECT COUNT(*) AS 'how many' FROM par
SELECT id, id + 1 'next' FROM par ORDER BY id
SELECT SQL_NO_CACHE id FROM par ORDER BY id
SELECT HIGH_PRIORITY id FROM par ORDER BY id
SELECT SQL_SMALL_RESULT id FROM par ORDER BY id
SELECT SQL_BIG_RESULT id FROM par ORDER BY id
SELECT SQL_BUFFER_RESULT id FROM par ORDER BY id
SELECT SQL_NO_CACHE HIGH_PRIORITY DISTINCT pid FROM chi ORDER BY pid
SELECT DISTINCT SQL_NO_CACHE pid FROM chi ORDER BY pid
SELECT DISTINCTROW pid FROM chi ORDER BY pid
SELECT ALL id FROM par ORDER BY id
SELECT STRAIGHT_JOIN par.id, chi.qty FROM par JOIN chi ON chi.pid = par.id ORDER BY par.id, chi.qty
SELECT par.id, chi.qty FROM par STRAIGHT_JOIN chi ON chi.pid = par.id ORDER BY par.id, chi.qty
SELECT par.id FROM par STRAIGHT_JOIN chi ON chi.pid = par.id WHERE chi.qty > 1 ORDER BY par.id
SELECT SQL_NO_CACHE * FROM par ORDER BY id
SELECT SQL_NO_CACHE COUNT(*) FROM par
SELECT id FROM par WHERE id IN (SELECT SQL_NO_CACHE pid FROM chi) ORDER BY id
SELECT HIGH_PRIORITY id FROM par UNION SELECT pid FROM chi ORDER BY 1
SELECT SQL_CACHE id FROM par
SELECT SQL_NO_CACHE_X id FROM par
SELECT id AS 'a' 'b' FROM par
SELECT * FROM par AS 'p'
# --- 4. mysqldump (and old applications) start with sql_mode = '': a column that is neither grouped nor aggregated is a value of some row of the group, not an error (the groups here hold one value, so the pick does not matter)
--CREATE TABLE em (id INT, v INT)
--CREATE TABLE gp (g INT, v INT, w INT)
--INSERT INTO gp VALUES (1,10,1),(1,10,2),(2,20,3),(3,30,4),(3,30,5),(3,30,6)
SET sql_mode = ''
SELECT g, v FROM gp GROUP BY g ORDER BY g
SELECT g, v, COUNT(*) FROM gp GROUP BY g ORDER BY g
SELECT g, v + 1 AS next, SUM(w) FROM gp GROUP BY g ORDER BY g
SELECT g, v, MAX(w) FROM gp GROUP BY g HAVING MAX(w) > 2 ORDER BY g
SELECT id, COUNT(*) FROM em
SELECT id, v + 1, COUNT(*) FROM em
SELECT g, COUNT(*) FROM gp WHERE g = 2
SELECT COUNT(*), 1, 'x' FROM gp
SELECT pid, COUNT(*) FROM chi GROUP BY pid ORDER BY pid
SET sql_mode = 'ONLY_FULL_GROUP_BY'
SELECT g, v FROM gp GROUP BY g
SELECT g, COUNT(*) FROM gp
SET sql_mode = 'ANSI'
SELECT g, COUNT(*) FROM gp
SET sql_mode = ''
SELECT g, v FROM gp GROUP BY g ORDER BY g
SET sql_mode = 'ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION'
SELECT g, v FROM gp GROUP BY g
