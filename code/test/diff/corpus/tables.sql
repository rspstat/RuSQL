# selecting and filtering
SELECT id, i, d, s FROM t ORDER BY id
SELECT id FROM t WHERE i > 5 ORDER BY id
SELECT id FROM t WHERE i = NULL ORDER BY id
SELECT id FROM t WHERE i IS NULL ORDER BY id
SELECT id FROM t WHERE NOT (i > 5) ORDER BY id
SELECT id FROM t WHERE i BETWEEN 0 AND 10 ORDER BY id
SELECT id FROM t WHERE i NOT BETWEEN 0 AND 10 ORDER BY id
SELECT id FROM t WHERE i IN (7, 10, NULL) ORDER BY id
SELECT id FROM t WHERE i NOT IN (7, 10) ORDER BY id
SELECT id FROM t WHERE i NOT IN (7, 10, NULL) ORDER BY id
SELECT id FROM t WHERE s = '10' ORDER BY id
SELECT id FROM t WHERE s = 10 ORDER BY id
SELECT id FROM t WHERE s > 9 ORDER BY id
SELECT id FROM t WHERE s > '9' ORDER BY id
SELECT id FROM t WHERE s < '9' ORDER BY id
SELECT id FROM t WHERE s LIKE '%an%' ORDER BY id
SELECT id FROM t WHERE s LIKE 'a%' ORDER BY id
SELECT id FROM t WHERE s LIKE '_pple' ORDER BY id
SELECT id FROM t WHERE s NOT LIKE '%a%' ORDER BY id
SELECT id FROM t WHERE s = '' ORDER BY id
SELECT id FROM t WHERE s IS NULL OR s = '' ORDER BY id
SELECT id FROM t WHERE d > 1 AND d < 3 ORDER BY id
SELECT id FROM t WHERE d = 3.25 ORDER BY id
SELECT id FROM t WHERE d = 100 ORDER BY id
SELECT id FROM t WHERE i = d ORDER BY id
SELECT id FROM t WHERE i > d ORDER BY id
SELECT id FROM t WHERE dt > '2024-03-01' ORDER BY id
SELECT id FROM t WHERE dt = '2024-03-05' ORDER BY id
SELECT id FROM t WHERE dt BETWEEN '2024-01-01' AND '2024-03-31' ORDER BY id
SELECT id FROM t WHERE YEAR(dt) = 2024 ORDER BY id
SELECT id FROM t WHERE MONTH(dt) = 3 ORDER BY id
SELECT id FROM t WHERE dt IS NULL ORDER BY id
SELECT id FROM t WHERE (i > 5 AND g = 3) OR i IS NULL ORDER BY id
SELECT id FROM t WHERE i > 5 AND (g = 3 OR g = 4) ORDER BY id
SELECT id, i + d FROM t ORDER BY id
SELECT id, i * 2, d / 2, i - d, i % 4 FROM t ORDER BY id
SELECT id, IFNULL(i, -1), COALESCE(d, i, 0), NULLIF(i, 7) FROM t ORDER BY id
SELECT id, CASE WHEN i IS NULL THEN 'n' WHEN i > 9 THEN 'big' ELSE 'small' END FROM t ORDER BY id
SELECT id, CASE g WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'many' END FROM t ORDER BY id
SELECT id, IF(i > 5, 'y', 'n') FROM t ORDER BY id
SELECT id, UPPER(s), LENGTH(s), CONCAT(s, '!') FROM t ORDER BY id
SELECT id, SUBSTRING(s, 2, 3), LEFT(s, 2), RIGHT(s, 2), REPLACE(s, 'a', 'A') FROM t ORDER BY id
SELECT id, s + 0, s * 1, CAST(s AS SIGNED) FROM t ORDER BY id
SELECT id, dt, DATE_ADD(dt, INTERVAL 1 MONTH), DATEDIFF(dt, '2024-01-01'), DAYNAME(dt) FROM t ORDER BY id
SELECT id, DATE_FORMAT(dt, '%Y-%m'), LAST_DAY(dt), YEAR(dt), MONTH(dt), DAY(dt) FROM t ORDER BY id
# distinct, order, limit
SELECT DISTINCT g FROM t ORDER BY g
SELECT DISTINCT i FROM t ORDER BY i
SELECT DISTINCT g, i FROM t ORDER BY g, i
SELECT id FROM t ORDER BY i, id
SELECT id FROM t ORDER BY i DESC, id
SELECT id, i FROM t ORDER BY d DESC, id
SELECT id FROM t ORDER BY s, id
SELECT id FROM t ORDER BY dt, id
SELECT id FROM t ORDER BY g DESC, i ASC, id
SELECT id FROM t ORDER BY id LIMIT 3
SELECT id FROM t ORDER BY id LIMIT 3 OFFSET 2
SELECT id FROM t ORDER BY id LIMIT 2, 3
SELECT id FROM t ORDER BY id DESC LIMIT 1
SELECT id FROM t ORDER BY id LIMIT 0
SELECT id, i FROM t ORDER BY IFNULL(i, 999), id
SELECT id, i % 3 AS r FROM t ORDER BY r, id
# aggregates
SELECT COUNT(*), COUNT(i), COUNT(DISTINCT i), COUNT(d), COUNT(s) FROM t
SELECT SUM(i), AVG(i), MIN(i), MAX(i) FROM t
SELECT SUM(d), AVG(d), MIN(d), MAX(d) FROM t
SELECT MIN(s), MAX(s), MIN(dt), MAX(dt) FROM t
SELECT SUM(i + d), AVG(i * 2), SUM(DISTINCT i), AVG(DISTINCT i) FROM t
SELECT STDDEV(i), VARIANCE(i), STDDEV_POP(i), VAR_POP(i), STDDEV_SAMP(i), VAR_SAMP(i) FROM t
SELECT GROUP_CONCAT(i ORDER BY i), GROUP_CONCAT(DISTINCT g ORDER BY g SEPARATOR '-') FROM t
SELECT COUNT(*), SUM(i) FROM t WHERE id > 100
SELECT g, COUNT(*), SUM(i), AVG(i), MIN(i), MAX(i) FROM t GROUP BY g ORDER BY g
SELECT g, COUNT(DISTINCT i), SUM(d), MAX(s) FROM t GROUP BY g ORDER BY g
SELECT g, GROUP_CONCAT(i ORDER BY i SEPARATOR ','), COUNT(s) FROM t GROUP BY g ORDER BY g
SELECT g, SUM(i) AS total FROM t GROUP BY g HAVING SUM(i) > 10 ORDER BY g
SELECT g, COUNT(*) FROM t GROUP BY g HAVING COUNT(*) >= 3 ORDER BY g
SELECT g % 2 AS parity, COUNT(*) FROM t GROUP BY g % 2 ORDER BY parity
SELECT YEAR(dt) AS y, COUNT(*) FROM t GROUP BY YEAR(dt) ORDER BY y
SELECT i, COUNT(*) FROM t GROUP BY i ORDER BY i
SELECT g, i, COUNT(*) FROM t GROUP BY g, i ORDER BY g, i
SELECT g, SUM(i) FROM t GROUP BY g WITH ROLLUP
SELECT COUNT(*) FROM (SELECT DISTINCT g FROM t) x
SELECT SUM(i) / COUNT(*), SUM(i) / COUNT(i) FROM t
SELECT MAX(i) - MIN(i), MAX(d) / MIN(d + 1) FROM t
# joins
SELECT t.id, u.name FROM t JOIN u ON u.id = t.g ORDER BY t.id
SELECT t.id, u.name FROM t LEFT JOIN u ON u.id = t.g ORDER BY t.id
SELECT t.id, u.name FROM t RIGHT JOIN u ON u.id = t.g ORDER BY t.id, u.id
SELECT t.id, u.k FROM t JOIN u ON u.k = t.i ORDER BY t.id, u.id
SELECT t.id, u.id FROM t LEFT JOIN u ON u.k = t.i AND u.id < 3 ORDER BY t.id, u.id
SELECT u.id, COUNT(t.id) FROM u LEFT JOIN t ON t.g = u.id GROUP BY u.id ORDER BY u.id
SELECT t.id, u.name FROM t, u WHERE t.g = u.id ORDER BY t.id
SELECT t.id, u.id FROM t CROSS JOIN u WHERE t.id < 3 ORDER BY t.id, u.id
SELECT a.id, b.id FROM t a JOIN t b ON a.g = b.g AND a.id < b.id ORDER BY a.id, b.id
SELECT t.id, u.name FROM t JOIN u USING (id) ORDER BY t.id
SELECT t.id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.k = t.i) ORDER BY t.id
SELECT t.id FROM t WHERE t.i IN (SELECT k FROM u) ORDER BY t.id
SELECT t.id FROM t WHERE t.i NOT IN (SELECT k FROM u WHERE k IS NOT NULL) ORDER BY t.id
SELECT t.id FROM t WHERE t.i > (SELECT AVG(k) FROM u) ORDER BY t.id
SELECT id, (SELECT COUNT(*) FROM t t2 WHERE t2.g = t.g) AS c FROM t ORDER BY id
SELECT id, (SELECT name FROM u WHERE u.id = t.g) FROM t ORDER BY id
SELECT x.g, x.c FROM (SELECT g, COUNT(*) AS c FROM t GROUP BY g) x WHERE x.c > 2 ORDER BY x.g
# set operations
SELECT g FROM t WHERE g < 3 UNION SELECT id FROM u ORDER BY 1
SELECT g FROM t WHERE g < 3 UNION ALL SELECT id FROM u ORDER BY 1
SELECT id FROM t INTERSECT SELECT id FROM u ORDER BY 1
SELECT id FROM t EXCEPT SELECT id FROM u ORDER BY 1
# windows
SELECT id, ROW_NUMBER() OVER (ORDER BY id) FROM t ORDER BY id
SELECT id, g, RANK() OVER (PARTITION BY g ORDER BY i) FROM t ORDER BY id
SELECT id, g, SUM(i) OVER (PARTITION BY g) FROM t ORDER BY id
SELECT id, SUM(i) OVER (ORDER BY id) FROM t ORDER BY id
SELECT id, LAG(i) OVER (ORDER BY id), LEAD(i) OVER (ORDER BY id) FROM t ORDER BY id
SELECT id, DENSE_RANK() OVER (ORDER BY g), NTILE(3) OVER (ORDER BY id) FROM t ORDER BY id
# values and expressions over columns
SELECT id, i = d, i <> d, i <=> d, i IS NULL, NOT i FROM t ORDER BY id
SELECT id, (i > 5) + (d > 1) FROM t ORDER BY id
SELECT id, i BETWEEN 5 AND 20, s IN ('10', '9'), s LIKE '1%' FROM t ORDER BY id
SELECT id, i > 5 AND d > 1, i > 5 OR d > 1 FROM t ORDER BY id
SELECT id, ABS(i), ROUND(d), ROUND(d, 1), FLOOR(d), CEIL(d), SIGN(d) FROM t ORDER BY id
SELECT id, POW(i, 2), SQRT(ABS(d)), MOD(i, 3), GREATEST(i, d), LEAST(i, d) FROM t ORDER BY id
SELECT id, FORMAT(d, 1), CONCAT_WS('-', i, d, s), LPAD(s, 6, '*'), REVERSE(s) FROM t ORDER BY id
SELECT id, TRIM(s), LTRIM(s), RTRIM(s), CHAR_LENGTH(s), INSTR(s, 'a') FROM t ORDER BY id
SELECT id, UPPER(s) = LOWER(s), s = 'APPLE', s = 'apple' FROM t ORDER BY id
# writes
--CREATE TABLE w (id INT PRIMARY KEY, n INT NOT NULL DEFAULT 5, s VARCHAR(10), u INT UNIQUE)
INSERT INTO w (id, s, u) VALUES (1, 'a', 10), (2, 'b', 20)
SELECT * FROM w ORDER BY id
INSERT INTO w (id, s, u) VALUES (3, 'c', 10)
INSERT INTO w VALUES (3, NULL, 'x', 30)
INSERT INTO w (id, n) VALUES (4, NULL)
INSERT INTO w (id, s, u) VALUES (5, 'toolongvalue12345', 50)
INSERT INTO w (id, s, u) VALUES (1, 'dup', 60)
INSERT INTO w (id, s, u) VALUES (6, 'f', 'abc')
UPDATE w SET n = n + 1 WHERE id < 3
SELECT * FROM w ORDER BY id
UPDATE w SET u = 20 WHERE id = 1
UPDATE w SET n = NULL WHERE id = 1
UPDATE w SET s = CONCAT(s, s) WHERE id = 2
SELECT * FROM w ORDER BY id
DELETE FROM w WHERE n > 5
SELECT * FROM w ORDER BY id
INSERT INTO w (id, s, u) VALUES (7, 'g', 70), (8, 'h', 80)
REPLACE INTO w (id, s, u) VALUES (7, 'G', 71)
INSERT INTO w (id, s, u) VALUES (7, 'x', 72) ON DUPLICATE KEY UPDATE s = 'dup', u = u + 1
SELECT * FROM w ORDER BY id
INSERT IGNORE INTO w (id, s, u) VALUES (7, 'y', 99), (9, 'i', 90)
SELECT * FROM w ORDER BY id
TRUNCATE TABLE w
SELECT COUNT(*) FROM w
# INSERT / REPLACE ... SET col = value is (cols) VALUES (values): conflicts (IGNORE, ON DUPLICATE KEY UPDATE, REPLACE), expressions and a column named twice
--CREATE TABLE si (id INT PRIMARY KEY AUTO_INCREMENT, n INT DEFAULT 7, s VARCHAR(20), v INT UNIQUE)
INSERT INTO si SET id = 1, n = 5, s = 'one', v = 10
INSERT INTO si SET s = 'auto'
INSERT INTO si SET id = 5, n = DEFAULT, s = NULL, v = 50
INSERT INTO si SET id = 6, n = 1 + 2 * 3, s = CONCAT('a', 'b'), v = 60 + 1
INSERT INTO si SET id = 7, s = 'it''s', v = -3
INSERT IGNORE INTO si SET id = 1, s = 'dup'
INSERT INTO si SET id = 1, s = 'upd', v = 10 ON DUPLICATE KEY UPDATE s = 'changed', n = n + 1
INSERT INTO si SET id = 9, s = 'nine' ON DUPLICATE KEY UPDATE n = 0
REPLACE INTO si SET id = 5, s = 'replaced', v = 55
REPLACE si SET id = 12, s = 'twelve'
SELECT id, n, s, v FROM si ORDER BY id
INSERT INTO si SET id = 1, v = 999
INSERT INTO si SET id = 20, v = 10
INSERT INTO si (s) SET id = 1
INSERT INTO si SET
INSERT INTO si SET id 1
INSERT INTO si SET id = 1,
INSERT INTO si SET id = 21, id = 22
INSERT INTO si (id, id) VALUES (30, 31)
SELECT id, n, s, v FROM si ORDER BY id
# x op ANY / SOME / ALL (subquery): some / every value, NULLs leave the answer open, an empty subquery is FALSE for ANY and TRUE for ALL
--CREATE TABLE qt (id INT PRIMARY KEY, a INT, name VARCHAR(10))
--CREATE TABLE qs (k INT PRIMARY KEY, grp INT, c INT, nm VARCHAR(10))
--INSERT INTO qt VALUES (1, 1, 'Alice'), (2, 2, 'bob'), (3, 3, 'carol'), (4, NULL, 'dave')
--INSERT INTO qs VALUES (1, 1, 2, 'ALICE'), (2, 1, 3, 'zed'), (3, 2, 2, 'x'), (4, 2, NULL, 'y')
SELECT id FROM qt WHERE a > ANY (SELECT c FROM qs WHERE grp = 1) ORDER BY id
SELECT id FROM qt WHERE a >= ANY (SELECT c FROM qs WHERE grp = 1) ORDER BY id
SELECT id FROM qt WHERE a < ANY (SELECT c FROM qs WHERE grp = 1) ORDER BY id
SELECT id FROM qt WHERE a = ANY (SELECT c FROM qs WHERE grp = 1) ORDER BY id
SELECT id FROM qt WHERE a = SOME (SELECT c FROM qs WHERE grp = 1) ORDER BY id
SELECT id FROM qt WHERE a <> ANY (SELECT c FROM qs WHERE grp = 1) ORDER BY id
SELECT id FROM qt WHERE a < ALL (SELECT c FROM qs WHERE grp = 1) ORDER BY id
SELECT id FROM qt WHERE a <= ALL (SELECT c FROM qs WHERE grp = 1) ORDER BY id
SELECT id FROM qt WHERE a > ALL (SELECT c FROM qs WHERE grp = 1) ORDER BY id
SELECT id FROM qt WHERE a = ALL (SELECT c FROM qs WHERE grp = 1) ORDER BY id
SELECT id FROM qt WHERE a <> ALL (SELECT c FROM qs WHERE grp = 1) ORDER BY id
SELECT id FROM qt WHERE a = ANY (SELECT c FROM qs WHERE grp = 2) ORDER BY id
SELECT id FROM qt WHERE a <> ALL (SELECT c FROM qs WHERE grp = 2) ORDER BY id
SELECT id FROM qt WHERE a > ALL (SELECT c FROM qs WHERE grp = 2) ORDER BY id
SELECT id FROM qt WHERE a < ALL (SELECT c FROM qs WHERE grp = 2) ORDER BY id
SELECT id FROM qt WHERE a >= ANY (SELECT c FROM qs WHERE grp = 2) ORDER BY id
SELECT id FROM qt WHERE NOT (a = ANY (SELECT c FROM qs WHERE grp = 2)) ORDER BY id
SELECT id FROM qt WHERE a > ANY (SELECT c FROM qs WHERE grp = 9) ORDER BY id
SELECT id FROM qt WHERE a > ALL (SELECT c FROM qs WHERE grp = 9) ORDER BY id
SELECT id FROM qt WHERE a = ALL (SELECT c FROM qs WHERE grp = 9) ORDER BY id
SELECT id FROM qt WHERE NOT (a > ANY (SELECT c FROM qs WHERE grp = 9)) ORDER BY id
SELECT id FROM qt WHERE NOT (a > ANY (SELECT c FROM qs WHERE grp = 1)) ORDER BY id
SELECT id FROM qt WHERE a = ANY (SELECT c FROM qs) ORDER BY id
SELECT id FROM qt WHERE a <> ALL (SELECT c FROM qs WHERE c IS NOT NULL) ORDER BY id
SELECT id FROM qt WHERE name = ANY (SELECT nm FROM qs) ORDER BY id
SELECT id FROM qt WHERE name <> ALL (SELECT nm FROM qs) ORDER BY id
SELECT id FROM qt WHERE a <= ANY (SELECT c FROM qs WHERE qs.grp = qt.id) ORDER BY id
SELECT id FROM qt WHERE a <= ALL (SELECT c FROM qs WHERE qs.grp = qt.id) ORDER BY id
SELECT id FROM qt WHERE a > ANY (SELECT c FROM qs WHERE grp = 1) OR id = 1 ORDER BY id
SELECT id FROM qt WHERE a >= ALL (SELECT c FROM qs WHERE grp = 1) AND id < 4 ORDER BY id
SELECT id, a > ALL (SELECT c FROM qs WHERE grp = 1) FROM qt ORDER BY id
SELECT grp FROM qs GROUP BY grp HAVING grp < ALL (SELECT id FROM qt WHERE id > 1) ORDER BY grp
UPDATE qt SET name = 'big' WHERE a >= ALL (SELECT c FROM qs WHERE grp = 1)
SELECT id, name FROM qt ORDER BY id
DELETE FROM qt WHERE a < ANY (SELECT c FROM qs WHERE grp = 1)
SELECT id FROM qt ORDER BY id
SELECT id FROM qt WHERE a > ANY (1, 2)
SELECT id FROM qt WHERE a > ALL
SELECT id FROM qt WHERE a IS ANY (SELECT c FROM qs)
SELECT id FROM qt WHERE a > ANY (SELECT c FROM qs WHERE k = 1) + 1
# row constructors: (a, b) = (1, 2), < <= > >= <> by the first values that differ, IN a list of rows or a subquery of rows, NULLs
--CREATE TABLE rt (id INT PRIMARY KEY, a INT, b INT, n VARCHAR(5))
--CREATE TABLE rs (k INT PRIMARY KEY, x INT, y INT, m VARCHAR(5))
--INSERT INTO rt VALUES (1, 1, 1, 'A'), (2, 1, 2, 'b'), (3, 2, 1, 'C'), (4, 2, 2, 'd'), (5, NULL, 1, 'e'), (6, 1, NULL, 'f')
--INSERT INTO rs VALUES (1, 1, 1, 'a'), (2, 2, 2, 'B'), (3, NULL, 3, 'z')
SELECT id FROM rt WHERE (a, b) = (1, 2) ORDER BY id
SELECT id FROM rt WHERE (a, b) <> (1, 2) ORDER BY id
SELECT id FROM rt WHERE (a, b) < (2, 1) ORDER BY id
SELECT id FROM rt WHERE (a, b) <= (2, 1) ORDER BY id
SELECT id FROM rt WHERE (a, b) > (1, 1) ORDER BY id
SELECT id FROM rt WHERE (a, b) >= (1, 1) ORDER BY id
SELECT id FROM rt WHERE (a, b, id) = (2, 2, 4) ORDER BY id
SELECT id FROM rt WHERE (a, b, id) < (2, 2, 4) ORDER BY id
SELECT id FROM rt WHERE (a, b) = (1 + 1, 4 / 2) ORDER BY id
SELECT id FROM rt WHERE (a, b) IN ((1, 1), (2, 2)) ORDER BY id
SELECT id FROM rt WHERE (a, b) NOT IN ((1, 1), (2, 2)) ORDER BY id
SELECT id FROM rt WHERE (a, b) IN ((1, NULL)) ORDER BY id
SELECT id FROM rt WHERE (a, b) NOT IN ((1, NULL)) ORDER BY id
SELECT id FROM rt WHERE id > 1 AND (a, b) = (2, 2) ORDER BY id
SELECT id FROM rt WHERE (a, b) = (1, 2) OR id = 6 ORDER BY id
SELECT id FROM rt WHERE NOT ((a, b) = (1, 2)) ORDER BY id
SELECT id FROM rt WHERE (a, b) IN (SELECT x, y FROM rs) ORDER BY id
SELECT id FROM rt WHERE (a, b) NOT IN (SELECT x, y FROM rs) ORDER BY id
SELECT id FROM rt WHERE (a, b) IN (SELECT x, y FROM rs WHERE x > 9) ORDER BY id
SELECT id FROM rt WHERE (a, b) NOT IN (SELECT x, y FROM rs WHERE x > 9) ORDER BY id
SELECT id FROM rt WHERE (a, b) IN (SELECT x, y FROM rs WHERE rs.x = rt.a) ORDER BY id
SELECT id FROM rt WHERE (n, a) IN (SELECT m, x FROM rs) ORDER BY id
SELECT id FROM rt WHERE (a, b) IN (SELECT x, y FROM rs WHERE k < 3) OR id = 5 ORDER BY id
SELECT id FROM rt WHERE (a, b) IN (SELECT x FROM rs)
SELECT id FROM rt WHERE (a, b) = (1, 2, 3)
SELECT id FROM rt WHERE (a, b) IN ((1, 2), (3))
UPDATE rt SET n = 'hit' WHERE (a, b) IN (SELECT x, y FROM rs)
SELECT id FROM rt WHERE n = 'hit' ORDER BY id
DELETE FROM rt WHERE (a, b) IN ((1, 1), (2, 2))
SELECT id FROM rt ORDER BY id
