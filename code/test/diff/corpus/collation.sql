# the default collation (utf8mb4_0900_ai_ci): case and accents do not count, a space does, a text is ordered by its weights
--CREATE TABLE ci (id INT PRIMARY KEY, name VARCHAR(20), grp VARCHAR(5), n INT)
--INSERT INTO ci VALUES (1, 'Alice', 'a', 1), (2, 'alice', 'A', 2), (3, 'ALICE', 'b', 3), (4, 'Bob', 'b', 4), (5, 'bob ', 'B', 5), (6, 'Émile', 'c', 6), (7, 'emile', 'C', 7), (8, 'Straße', 'd', 8), (9, 'STRASSE', 'D', 9), (10, '한글', 'e', 10), (11, '가나', 'E', 11), (12, 'Zed', NULL, 12), (13, '_x', 'f', 13), (14, '-x', 'f', 14), (15, '1x', 'g', 15), (16, 'Xx', 'g', 16)
--CREATE TABLE ci_idx (id INT PRIMARY KEY, name VARCHAR(20), grp VARCHAR(5), n INT)
--INSERT INTO ci_idx VALUES (1, 'Alice', 'a', 1), (2, 'alice', 'A', 2), (3, 'ALICE', 'b', 3), (4, 'Bob', 'b', 4), (5, 'bob ', 'B', 5), (6, 'Émile', 'c', 6), (7, 'emile', 'C', 7), (8, 'Straße', 'd', 8), (9, 'STRASSE', 'D', 9), (10, '한글', 'e', 10), (11, '가나', 'E', 11), (12, 'Zed', NULL, 12), (13, '_x', 'f', 13), (14, '-x', 'f', 14), (15, '1x', 'g', 15), (16, 'Xx', 'g', 16)
--CREATE INDEX ci_idx_name ON ci_idx (name)
--CREATE INDEX ci_idx_grp ON ci_idx (grp)
--CREATE TABLE ck (name VARCHAR(20) PRIMARY KEY, v INT)
--INSERT INTO ck VALUES ('Alice', 1), ('bob', 2), ('Émile', 3), ('zed', 4)
--CREATE TABLE cj (id INT PRIMARY KEY, who VARCHAR(20))
--INSERT INTO cj VALUES (1, 'ALICE'), (2, 'BOB'), (3, 'emile'), (4, 'nobody'), (5, 'Zed'), (6, 'zed ')
# equality, inequality and ranges
SELECT id FROM ci WHERE name = 'alice' ORDER BY id
SELECT id FROM ci WHERE name = 'ALICE' ORDER BY id
SELECT id FROM ci WHERE name <> 'alice' ORDER BY id
SELECT id FROM ci WHERE name = 'emile' ORDER BY id
SELECT id FROM ci WHERE name = 'strasse' ORDER BY id
SELECT id FROM ci WHERE name = 'bob' ORDER BY id
SELECT id FROM ci WHERE name > 'bob' ORDER BY id
SELECT id FROM ci WHERE name >= 'Bob' AND name < 'Zed' ORDER BY id
SELECT id FROM ci WHERE name BETWEEN 'a' AND 'c' ORDER BY id
SELECT id FROM ci WHERE name NOT BETWEEN 'a' AND 'c' ORDER BY id
SELECT id FROM ci WHERE name IN ('ALICE', 'zed', 'EMILE') ORDER BY id
SELECT id FROM ci WHERE name NOT IN ('ALICE', 'zed') ORDER BY id
SELECT id FROM ci WHERE grp = 'a' ORDER BY id
SELECT id FROM ci WHERE grp IN ('A', 'C') ORDER BY id
SELECT id FROM ci_idx WHERE name = 'alice' ORDER BY id
SELECT id FROM ci_idx WHERE name = 'ALICE' ORDER BY id
SELECT id FROM ci_idx WHERE name = 'emile' ORDER BY id
SELECT id FROM ci_idx WHERE name = 'strasse' ORDER BY id
SELECT id FROM ci_idx WHERE name > 'bob' ORDER BY id
SELECT id FROM ci_idx WHERE name >= 'Bob' AND name < 'Zed' ORDER BY id
SELECT id FROM ci_idx WHERE name BETWEEN 'a' AND 'c' ORDER BY id
SELECT id FROM ci_idx WHERE name IN ('ALICE', 'zed', 'EMILE') ORDER BY id
SELECT id FROM ci_idx WHERE grp = 'a' ORDER BY id
SELECT id FROM ci_idx WHERE grp IN ('A', 'C') ORDER BY id
SELECT id FROM ci_idx WHERE grp <> 'a' ORDER BY id
SELECT id, name FROM ci WHERE name = 'bob ' ORDER BY id
SELECT id, name FROM ci_idx WHERE name = 'bob ' ORDER BY id
SELECT id FROM ci WHERE name = 'bobby' ORDER BY id
SELECT id FROM ck WHERE name = 'x' ORDER BY id
# LIKE and REGEXP
SELECT id FROM ci WHERE name LIKE 'ali%' ORDER BY id
SELECT id FROM ci WHERE name LIKE 'ALI%' ORDER BY id
SELECT id FROM ci WHERE name LIKE '%CE' ORDER BY id
SELECT id FROM ci WHERE name LIKE 'e_ile' ORDER BY id
SELECT id FROM ci WHERE name LIKE 'str%' ORDER BY id
SELECT id FROM ci WHERE name NOT LIKE 'a%' ORDER BY id
SELECT id FROM ci WHERE name LIKE '__' ORDER BY id
SELECT id FROM ci WHERE name LIKE '한%' ORDER BY id
SELECT id FROM ci_idx WHERE name LIKE 'ali%' ORDER BY id
SELECT id FROM ci_idx WHERE name LIKE 'ALI%' ORDER BY id
SELECT id FROM ci_idx WHERE name LIKE 'e%' ORDER BY id
SELECT id FROM ci_idx WHERE name LIKE 'str%' ORDER BY id
SELECT id FROM ci_idx WHERE name LIKE 'b%' ORDER BY id
SELECT id FROM ci WHERE name REGEXP '^ali' ORDER BY id
SELECT id FROM ci WHERE name REGEXP 'CE$' ORDER BY id
SELECT id FROM ci WHERE name NOT REGEXP '^a' ORDER BY id
# ordering
SELECT id FROM ci ORDER BY name, id
SELECT id FROM ci ORDER BY name DESC, id
SELECT id FROM ci_idx ORDER BY name, id
SELECT id FROM ci_idx ORDER BY name DESC, id
SELECT id FROM ci ORDER BY grp, id
SELECT id FROM ci ORDER BY grp DESC, id
SELECT name FROM ci ORDER BY name, id LIMIT 5
SELECT name FROM ci ORDER BY name DESC, id LIMIT 5
SELECT id FROM ci ORDER BY name, id LIMIT 4 OFFSET 3
SELECT id FROM ci WHERE name > 'b' ORDER BY name, id
SELECT id FROM ci_idx WHERE name > 'b' ORDER BY name, id
# grouping, distinct, aggregates
SELECT COUNT(DISTINCT name) FROM ci
SELECT COUNT(DISTINCT grp) FROM ci
SELECT COUNT(DISTINCT name) FROM ci_idx
SELECT LOWER(name), COUNT(*) FROM ci GROUP BY name ORDER BY LOWER(name)
SELECT COUNT(*) FROM (SELECT name FROM ci GROUP BY name) q
SELECT COUNT(*) FROM (SELECT DISTINCT name FROM ci) q
SELECT COUNT(*) FROM (SELECT DISTINCT grp FROM ci) q
SELECT COUNT(*) FROM (SELECT grp FROM ci GROUP BY grp) q
SELECT COUNT(*) FROM (SELECT grp, n > 5 FROM ci GROUP BY grp, n > 5) q
SELECT grp, COUNT(*), SUM(n) FROM ci GROUP BY grp ORDER BY SUM(n), 1
SELECT grp, COUNT(*) FROM ci_idx GROUP BY grp ORDER BY COUNT(*), 1
SELECT COUNT(*) FROM (SELECT grp FROM ci_idx GROUP BY grp) q
SELECT LOWER(MIN(name)), LOWER(MAX(name)) FROM ci
SELECT LOWER(MIN(name)), LOWER(MAX(name)) FROM ci_idx
SELECT LOWER(MIN(grp)), LOWER(MAX(grp)) FROM ci
SELECT GROUP_CONCAT(DISTINCT LOWER(name) ORDER BY LOWER(name)) FROM ci
SELECT ROW_NUMBER() OVER (PARTITION BY grp ORDER BY id) AS r, id FROM ci ORDER BY id
SELECT SUM(n) OVER (PARTITION BY grp) AS s, id FROM ci ORDER BY id
SELECT RANK() OVER (ORDER BY name) AS r, id FROM ci ORDER BY id
# sets and subqueries
SELECT COUNT(*) FROM (SELECT name FROM ci UNION SELECT who FROM cj) q
SELECT COUNT(*) FROM (SELECT name FROM ci UNION ALL SELECT who FROM cj) q
SELECT COUNT(*) FROM (SELECT name FROM ci INTERSECT SELECT who FROM cj) q
SELECT COUNT(*) FROM (SELECT name FROM ci WHERE id <> 5 UNION SELECT who FROM cj WHERE id <> 6) q
SELECT COUNT(*) FROM (SELECT name FROM ci WHERE id <> 5 INTERSECT SELECT who FROM cj WHERE id <> 6) q
SELECT COUNT(*) FROM (SELECT name FROM ci WHERE id <> 5 EXCEPT SELECT who FROM cj WHERE id <> 6) q
SELECT id FROM ci WHERE id <> 5 AND name IN (SELECT who FROM cj WHERE id <> 6) ORDER BY id
SELECT COUNT(*) FROM (SELECT grp FROM ci UNION SELECT grp FROM ci_idx) q
SELECT COUNT(*) FROM (SELECT grp FROM ci INTERSECT SELECT grp FROM ci_idx) q
SELECT COUNT(*) FROM (SELECT name FROM ci EXCEPT SELECT who FROM cj) q
SELECT id FROM ci WHERE name IN (SELECT who FROM cj) ORDER BY id
SELECT id FROM ci WHERE name NOT IN (SELECT who FROM cj) ORDER BY id
SELECT id FROM ci_idx WHERE name IN (SELECT who FROM cj) ORDER BY id
SELECT id FROM cj WHERE who IN (SELECT name FROM ck) ORDER BY id
SELECT id FROM ci WHERE EXISTS (SELECT 1 FROM cj WHERE cj.who = ci.name) ORDER BY id
SELECT id FROM ci WHERE NOT EXISTS (SELECT 1 FROM cj WHERE cj.who = ci.name) ORDER BY id
SELECT id, (SELECT COUNT(*) FROM cj WHERE cj.who = ci.name) FROM ci ORDER BY id
SELECT id FROM ci WHERE name = (SELECT who FROM cj WHERE id = 1) ORDER BY id
SELECT id FROM ci WHERE name > (SELECT who FROM cj WHERE id = 2) ORDER BY id
# joins
SELECT ci.id, cj.id FROM ci JOIN cj ON ci.name = cj.who ORDER BY ci.id, cj.id
SELECT ci.id, cj.id FROM ci LEFT JOIN cj ON ci.name = cj.who ORDER BY ci.id, cj.id
SELECT ci.id, cj.id FROM ci RIGHT JOIN cj ON ci.name = cj.who ORDER BY ci.id, cj.id
SELECT ci.id, cj.id FROM ci_idx ci JOIN cj ON ci.name = cj.who ORDER BY ci.id, cj.id
SELECT cj.id, ck.v FROM cj JOIN ck ON ck.name = cj.who ORDER BY cj.id
SELECT cj.id, ck.v FROM cj LEFT JOIN ck ON ck.name = cj.who ORDER BY cj.id
SELECT ck.name, cj.id FROM ck JOIN cj ON cj.who = ck.name ORDER BY cj.id
SELECT COUNT(*) FROM ci a JOIN ci b ON a.name = b.name
SELECT COUNT(*) FROM ci_idx a JOIN ci_idx b ON a.name = b.name
SELECT COUNT(*) FROM ci a JOIN ci b ON a.grp = b.grp
# string functions
SELECT STRCMP('a', 'A'), STRCMP('a', 'b'), STRCMP('b', 'A'), STRCMP('é', 'e'), STRCMP('ß', 'ss'), STRCMP('a', 'a ')
SELECT INSTR('Hello', 'h'), INSTR('HELLO', 'ell'), LOCATE('L', 'hello'), LOCATE('L', 'hello', 4), INSTR('héllo', 'E')
SELECT FIND_IN_SET('B', 'a,b,c'), FIND_IN_SET('b', 'A,B,C'), FIND_IN_SET('d', 'a,b,c')
SELECT FIELD('B', 'a', 'b', 'c'), FIELD('c', 'A', 'B', 'C'), FIELD('z', 'a')
SELECT GREATEST('a', 'B', 'c'), LEAST('b', 'A', 'C'), GREATEST('apple', 'APPLES'), LEAST('Zed', 'zebra')
SELECT REPLACE('Hello', 'h', 'J'), REPLACE('Hello', 'H', 'J'), SUBSTRING_INDEX('a,B,c', 'b', 1), SUBSTRING_INDEX('a,B,c', 'B', 1)
SELECT NULLIF('A', 'a'), NULLIF('A', 'b'), IF('a' = 'A', 1, 0), CASE 'a' WHEN 'A' THEN 'yes' ELSE 'no' END
SELECT 'a' = 'A', 'a' < 'B', 'a' = 'a ', 'é' = 'e', 'ß' = 'ss', 'a' IN ('A'), 'a' LIKE 'A', 'a' BETWEEN 'A' AND 'B'
SELECT COALESCE(NULL, 'x') = 'X', UPPER('é') = 'É', LOWER('É') = 'é'
SELECT id, CASE WHEN name = 'alice' THEN 1 ELSE 0 END FROM ci ORDER BY id
SELECT id, IF(grp = 'a', 'yes', 'no') FROM ci ORDER BY id
# writes: UNIQUE / PRIMARY KEY by the collation
--CREATE TABLE cu (id INT PRIMARY KEY, email VARCHAR(30) UNIQUE, tag VARCHAR(10))
--INSERT INTO cu VALUES (1, 'A@x.com', 'p'), (2, 'b@x.com', 'q')
INSERT INTO cu VALUES (3, 'a@X.COM', 'r')
INSERT INTO cu VALUES (3, 'c@x.com', 'r')
INSERT INTO cu VALUES (4, 'B@X.COM', 's')
UPDATE cu SET email = 'B@x.com' WHERE id = 1
UPDATE cu SET email = 'A@X.com' WHERE id = 1
UPDATE cu SET email = 'c@x.com' WHERE id = 2
SELECT id, email FROM cu ORDER BY id
REPLACE INTO cu VALUES (9, 'C@X.COM', 't')
SELECT id, email FROM cu ORDER BY id
INSERT INTO cu VALUES (10, 'c@x.com', 'u') ON DUPLICATE KEY UPDATE tag = 'dup'
SELECT id, email, tag FROM cu ORDER BY id
INSERT IGNORE INTO cu VALUES (11, 'C@x.com', 'v')
SELECT COUNT(*) FROM cu
INSERT INTO ck VALUES ('ALICE', 9)
INSERT INTO ck VALUES ('Émile', 9)
INSERT INTO ck VALUES ('EMILE', 9)
INSERT INTO ck VALUES ('new', 9), ('NEW', 9)
UPDATE ck SET name = 'ZED' WHERE name = 'bob'
UPDATE ck SET name = 'BOB' WHERE name = 'bob'
SELECT name, v FROM ck ORDER BY name
ALTER TABLE ci ADD UNIQUE (name)
--CREATE TABLE cd (id INT PRIMARY KEY, w VARCHAR(10))
--INSERT INTO cd VALUES (1, 'x'), (2, 'X'), (3, 'y')
ALTER TABLE cd ADD UNIQUE (w)
DELETE FROM cd WHERE id = 2
ALTER TABLE cd ADD UNIQUE (w)
INSERT INTO cd VALUES (4, 'Y')
# foreign keys by the collation
--CREATE TABLE fp (code VARCHAR(10) PRIMARY KEY, label VARCHAR(10))
--INSERT INTO fp VALUES ('abc', 'one'), ('Def', 'two')
--CREATE TABLE fc (id INT PRIMARY KEY, code VARCHAR(10), FOREIGN KEY (code) REFERENCES fp (code))
INSERT INTO fc VALUES (1, 'ABC')
INSERT INTO fc VALUES (2, 'def')
INSERT INTO fc VALUES (3, 'xyz')
DELETE FROM fp WHERE code = 'abc'
UPDATE fp SET code = 'ABCD' WHERE code = 'Def'
SELECT id, code FROM fc ORDER BY id
# recursive CTEs drop a row that repeats one by the collation (UNION), and keep it with UNION ALL
WITH RECURSIVE r AS (SELECT 'a' AS s UNION SELECT IF(s = 'a', 'A', 'a') FROM r) SELECT COUNT(*) FROM r
WITH RECURSIVE r AS (SELECT 1 AS n, 'a' AS s UNION SELECT n, UPPER(s) FROM r WHERE n < 3) SELECT COUNT(*) FROM r
WITH RECURSIVE r AS (SELECT 1 AS n, 'a' AS s UNION ALL SELECT n + 1, UPPER(s) FROM r WHERE n < 3) SELECT n, s FROM r ORDER BY n
# LIKE compares one code point to one code point
SELECT 'ß' LIKE 's', 'ß' LIKE 'ss', 'ss' LIKE 'ß', 'ß' LIKE '_', 'ß' LIKE '__', 'ss' LIKE 's_'
SELECT 'æ' LIKE 'a', 'æ' LIKE 'ae', 'Æ' LIKE 'æ', 'e' LIKE 'é', '가' LIKE '_'
# foreign key actions find the child rows by the collation
--CREATE TABLE fp2 (code VARCHAR(10) PRIMARY KEY)
--INSERT INTO fp2 VALUES ('abc'), ('Def'), ('ghi')
--CREATE TABLE fc2 (id INT PRIMARY KEY, code VARCHAR(10), FOREIGN KEY (code) REFERENCES fp2 (code) ON DELETE CASCADE ON UPDATE CASCADE)
--INSERT INTO fc2 VALUES (1, 'ABC'), (2, 'def'), (3, 'GHI')
DELETE FROM fp2 WHERE code = 'abc'
SELECT id, code FROM fc2 ORDER BY id
UPDATE fp2 SET code = 'xyz' WHERE code = 'DEF'
SELECT id, code FROM fc2 ORDER BY id
SELECT code FROM fp2 ORDER BY code
--CREATE TABLE fq2 (code VARCHAR(10) PRIMARY KEY)
--INSERT INTO fq2 VALUES ('aaa'), ('bbb')
--CREATE TABLE fr2 (id INT PRIMARY KEY, code VARCHAR(10), FOREIGN KEY (code) REFERENCES fq2 (code) ON DELETE SET NULL ON UPDATE SET NULL)
--INSERT INTO fr2 VALUES (1, 'AAA'), (2, 'BBB')
DELETE FROM fq2 WHERE code = 'aaa'
UPDATE fq2 SET code = 'ccc' WHERE code = 'BBB'
SELECT id, code FROM fr2 ORDER BY id
--CREATE TABLE fs2 (code VARCHAR(10) PRIMARY KEY)
--INSERT INTO fs2 VALUES ('kkk')
--CREATE TABLE ft2 (id INT PRIMARY KEY, code VARCHAR(10), FOREIGN KEY (code) REFERENCES fs2 (code))
--INSERT INTO ft2 VALUES (1, 'KKK')
DELETE FROM fs2 WHERE code = 'kkk'
UPDATE fs2 SET code = 'zzz' WHERE code = 'KKK'
SELECT code FROM fs2
--CREATE TABLE emp2 (id VARCHAR(10) PRIMARY KEY, boss VARCHAR(10), FOREIGN KEY (boss) REFERENCES emp2 (id))
INSERT INTO emp2 VALUES ('Root', NULL), ('kid', 'ROOT')
INSERT INTO emp2 VALUES ('Solo', 'solo')
INSERT INTO emp2 VALUES ('lost', 'nobody')
SELECT id, boss FROM emp2 ORDER BY id
# REPLACE and the rows of one statement
--CREATE TABLE ru2 (id INT PRIMARY KEY, email VARCHAR(30) UNIQUE)
REPLACE INTO ru2 VALUES (1, 'a@x.com'), (2, 'A@X.COM')
SELECT id FROM ru2
--CREATE TABLE rp2 (a VARCHAR(5), b VARCHAR(5), v INT, PRIMARY KEY (a, b))
REPLACE INTO rp2 VALUES ('x', 'y', 1), ('X', 'Y', 2)
SELECT v FROM rp2
--CREATE TABLE rq2 (id INT PRIMARY KEY, a VARCHAR(5), b VARCHAR(5), UNIQUE (a, b))
REPLACE INTO rq2 VALUES (1, 'x', 'y'), (2, 'X', 'Y')
SELECT id FROM rq2
--CREATE TABLE bt2 (name VARCHAR(10) PRIMARY KEY)
INSERT INTO bt2 VALUES ('NEW'), ('new')
INSERT INTO bt2 VALUES ('new'), ('NEW')
SELECT COUNT(*) FROM bt2
--CREATE TABLE bp2 (a VARCHAR(5), b VARCHAR(5), PRIMARY KEY (a, b))
INSERT INTO bp2 VALUES ('A', 'x'), ('a', 'X')
SELECT COUNT(*) FROM bp2
UPDATE cu SET email = 'ALICE@X.COM'
SELECT id FROM ci WHERE name IN (SELECT who FROM cj WHERE cj.id <= ci.n) ORDER BY id
SELECT id FROM ci WHERE name NOT IN (SELECT who FROM cj WHERE cj.id <= ci.n) ORDER BY id
