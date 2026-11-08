# The COLLATE clause and BINARY (R77): a string constant or a column read under another collation, columns declared with one, COLLATION() / CHARSET(), the errors. The default
# collation utf8mb4_0900_ai_ci is in collation.sql. A line is a statement, '--' a setup statement run on both.
# NOT YET ON THE SCOREBOARD: RuSQL has one collation for the whole engine (see RECOMMAND.md, "COLLATE utf8mb4_bin / BINARY"); this corpus measures what is different.
--CREATE TABLE cs (id INT PRIMARY KEY, s VARCHAR(10) COLLATE utf8mb4_bin)
--INSERT INTO cs VALUES (1, 'a'), (2, 'A'), (3, 'b'), (4, 'á'), (5, 'B')
--CREATE TABLE ci (id INT PRIMARY KEY, s VARCHAR(10))
--INSERT INTO ci VALUES (1, 'a'), (2, 'A'), (3, 'b'), (4, 'á'), (5, 'B')
# a constant under another collation
SELECT 'a' = 'A', 'a' = 'á', 'a' < 'B'
SELECT 'a' COLLATE utf8mb4_bin = 'A', 'a' = 'A' COLLATE utf8mb4_bin, 'a' COLLATE utf8mb4_bin < 'B'
SELECT 'a' COLLATE utf8mb4_0900_ai_ci = 'A', 'a' COLLATE utf8mb4_0900_as_cs = 'A', 'a' COLLATE utf8mb4_0900_as_ci = 'A', 'a' COLLATE utf8mb4_0900_as_ci = 'á'
SELECT 'a' COLLATE utf8mb4_general_ci = 'A', 'a' COLLATE utf8mb4_general_ci = 'á', 'ß' COLLATE utf8mb4_general_ci = 'ss', 'ß' COLLATE utf8mb4_0900_ai_ci = 'ss'
SELECT BINARY 'a' = 'A', BINARY 'a' = 'a', 'a' = BINARY 'A'
SELECT COLLATION('a'), COLLATION('a' COLLATE utf8mb4_bin), COLLATION(BINARY 'a'), COLLATION(1), COLLATION(NOW())
SELECT CHARSET('a'), CHARSET(BINARY 'a'), CHARSET(1), CHARSET(CONVERT('a' USING latin1))
SELECT 'a' COLLATE nosuch
SELECT 'a' COLLATE utf8mb4_bin = 'A' COLLATE utf8mb4_general_ci
SELECT 'a' COLLATE utf8mb4_bin = 'A' COLLATE utf8mb4_bin
# a column declared with a collation
SELECT id FROM cs WHERE s = 'a' ORDER BY id
SELECT id FROM cs WHERE s = 'A' ORDER BY id
SELECT s FROM cs ORDER BY s
SELECT s FROM cs ORDER BY s DESC
SELECT DISTINCT s FROM cs ORDER BY s
SELECT s, COUNT(*) FROM cs GROUP BY s ORDER BY s
SELECT MIN(s), MAX(s) FROM cs
SELECT id FROM cs WHERE s LIKE 'a%' ORDER BY id
SELECT id FROM cs WHERE s LIKE 'A%' ORDER BY id
SELECT id FROM cs WHERE s IN ('a', 'B') ORDER BY id
SELECT id FROM cs WHERE s BETWEEN 'a' AND 'b' ORDER BY id
SELECT id FROM cs WHERE s > 'A' ORDER BY id
SELECT id FROM cs WHERE s = 'A' COLLATE utf8mb4_0900_ai_ci ORDER BY id
SELECT id FROM ci WHERE s = 'a' COLLATE utf8mb4_bin ORDER BY id
SELECT id FROM ci WHERE s COLLATE utf8mb4_bin = 'A' ORDER BY id
SELECT s FROM ci ORDER BY s COLLATE utf8mb4_bin
SELECT s FROM ci ORDER BY BINARY s
SELECT DISTINCT s COLLATE utf8mb4_bin FROM ci ORDER BY 1
SELECT s COLLATE utf8mb4_bin, COUNT(*) FROM ci GROUP BY s COLLATE utf8mb4_bin ORDER BY 1
SELECT c1.id, c2.id FROM cs c1 JOIN ci c2 ON c1.s = c2.s ORDER BY c1.id, c2.id
SELECT c1.id, c2.id FROM ci c1 JOIN ci c2 ON c1.s = c2.s ORDER BY c1.id, c2.id
SELECT COLLATION(s) FROM cs LIMIT 1
SELECT COLLATION(s) FROM ci LIMIT 1
SELECT UPPER(s), LOWER(s), LENGTH(s), CONCAT(s, 'x') FROM cs WHERE id = 4
SELECT s = 'a' COLLATE utf8mb4_0900_ai_ci FROM cs ORDER BY id
SELECT id FROM cs WHERE s = 'a' COLLATE utf8mb4_general_ci
# keys: a UNIQUE index or a primary key on such a column
CREATE TABLE ck (s VARCHAR(10) COLLATE utf8mb4_bin, UNIQUE KEY (s))
INSERT INTO ck VALUES ('a'), ('A'), ('á')
INSERT INTO ck VALUES ('a')
SELECT s FROM ck ORDER BY s
CREATE TABLE cp (s VARCHAR(10) COLLATE utf8mb4_bin PRIMARY KEY)
INSERT INTO cp VALUES ('x'), ('X')
SELECT s FROM cp WHERE s = 'x'
# a table with a default collation
CREATE TABLE ct (id INT, s VARCHAR(10)) DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin
INSERT INTO ct VALUES (1, 'a'), (2, 'A')
SELECT id FROM ct WHERE s = 'a' ORDER BY id
SELECT COLLATION(s) FROM ct LIMIT 1
CREATE TABLE ct2 (id INT, s VARCHAR(10) COLLATE utf8mb4_0900_ai_ci) DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin
INSERT INTO ct2 VALUES (1, 'a'), (2, 'A')
SELECT id FROM ct2 WHERE s = 'a' ORDER BY id
SELECT COLLATION(s) FROM ct2 LIMIT 1
# what the schema says
SELECT COLLATION_NAME FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'cs' AND column_name = 's'
SELECT COLLATION_NAME FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'ci' AND column_name = 's'
SELECT CHARACTER_SET_NAME, COLLATION_NAME FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'ct' AND column_name = 's'
SELECT TABLE_COLLATION FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'ct'
SHOW FULL COLUMNS FROM cs
SHOW COLLATION LIKE 'utf8mb4_bin'
SHOW COLLATION LIKE 'utf8mb4_0900_ai_ci'
SHOW CHARACTER SET LIKE 'utf8mb4'
SELECT COLLATION_NAME, CHARACTER_SET_NAME, IS_DEFAULT FROM information_schema.collations WHERE COLLATION_NAME IN ('utf8mb4_bin', 'utf8mb4_0900_ai_ci') ORDER BY 1
SELECT CHARACTER_SET_NAME, DEFAULT_COLLATE_NAME FROM information_schema.character_sets WHERE CHARACTER_SET_NAME = 'utf8mb4'
# changing a collation
ALTER TABLE ci MODIFY s VARCHAR(10) COLLATE utf8mb4_bin
SELECT id FROM ci WHERE s = 'a' ORDER BY id
SELECT COLLATION(s) FROM ci LIMIT 1
ALTER TABLE cs MODIFY s VARCHAR(10) COLLATE utf8mb4_general_ci
SELECT id FROM cs WHERE s = 'a' ORDER BY id
SELECT id FROM cs WHERE s = 'á' ORDER BY id
SELECT CONVERT('a' USING utf8mb4), CAST('a' AS CHAR CHARACTER SET utf8mb4), CAST('a' AS BINARY)
SELECT CAST('abc' AS CHAR(2) CHARACTER SET utf8mb4)
