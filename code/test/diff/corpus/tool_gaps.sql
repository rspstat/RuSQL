# R81 audit (2026-11-14): gaps found while checking RECOMMAND.md against real MySQL 8.4.9 -- what mysqldump / mysqlshow / schema tools / hand-written SQL send. NOT in the
# scoreboard (make_scoreboard.py): every group below still differs. R83 (2026-11-15) moved what mysqldump, mysqlshow, schema tools and a string alias / select modifiers needed
# to corpus/tool_compat_r83.sql (on the scoreboard); what is left is the rest of sql_mode, small syntax and variables. Run: python vs_mysql.py <engine_server.exe> corpus/tool_gaps.sql --proto --errno
# A line is a statement, '--' a setup statement run on both. The groups match the rows of RECOMMAND.md ("R81").
--CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL, UNIQUE KEY uq_name (name))
--CREATE TABLE chi (id INT PRIMARY KEY AUTO_INCREMENT, pid INT NOT NULL, qty INT DEFAULT 1 COMMENT 'amount', CONSTRAINT fk_chi_par FOREIGN KEY (pid) REFERENCES par (id), CONSTRAINT ck_qty CHECK (qty >= 0), KEY ix_pid_qty (pid, qty)) COMMENT='children'
--INSERT INTO par VALUES (1,'a'),(2,'b')
--INSERT INTO chi (pid, qty) VALUES (1,5),(2,7)
--CREATE TABLE ckt (id INT, q INT CHECK (q > 0), CONSTRAINT named CHECK (id < 9), CHECK (id <> q))
# --- 4. sql_mode is always strict (apps that SET sql_mode = '' for legacy SQL): ONLY_FULL_GROUP_BY is read since R83, the rest is not
SET sql_mode = ''
INSERT INTO chi (pid, qty) VALUES (1, 'abc')
INSERT INTO par VALUES (9, REPEAT('x', 30))
INSERT INTO chi (pid, qty) VALUES (1, 1 / 0)
SET sql_mode = 'ANSI_QUOTES'
SELECT "id" FROM par ORDER BY 1
SET sql_mode = 'STRICT_TRANS_TABLES'
# --- 4b. SHOW CREATE TABLE writes the CHECK of a column and the table's own, named; the text of the condition is the one typed (MySQL writes `q` > 0 with quoted names)
SHOW CREATE TABLE ckt
# --- 5. small syntax: CONVERT ... USING, DO, ROW(), TABLE, VALUES ROW, INSERT ... AS alias, DROP CHECK, ALTER CHECK
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
# --- 6. variables and numbers
SET NAMES utf8mb4 COLLATE utf8mb4_unicode_ci
SELECT @@collation_connection, @@auto_increment_offset
SELECT 18446744073709551615 + 1
SELECT CAST(18446744073709551615 AS SIGNED)
