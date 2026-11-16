# R91 (2026-11-16): AVG used in an expression or a comparison. MySQL keeps the quotient of AVG with 9 digits (cut off, not rounded: AVG over 1, 2, 2 holds 1.666666666) and only SHOWS the scale of the argument + 4 (1.6667): so AVG(v) * 3 shows 5.0000 (not 5.0001), AVG(v) = 1.6667 is false, AVG(v) / 1 shows 1.66666667.
# Measured on MySQL 8.4.9. (Not here: ROUND(x, D) / TRUNCATE(x, D) with a D beyond the places x SHOWS -- ROUND(10 / 3, 6) is 3.3333 in MySQL, RuSQL gives 3.333333, an older difference of every quotient;  `FROM (SELECT AVG(v) AS x ...) q WHERE x = 1.6667`, which MySQL answers with no row because it pushes the condition into the derived table as HAVING AVG(v) = 1.6667 -- RuSQL makes the table first. AVG(x) OVER () is already rounded to its shown scale there: AVG(v) OVER () * 3 is 5.0001.) A line is a statement, '--' a setup statement run on both. Run: python vs_mysql.py <engine_server.exe> corpus/avg_expr_r91.sql --proto --errno
--DROP TABLE IF EXISTS av
--CREATE TABLE av (id INT PRIMARY KEY, v INT, d DECIMAL(10,2), e DECIMAL(20,10), f DOUBLE, g VARCHAR(10), grp INT)
--INSERT INTO av VALUES (1, 1, 1.00, 1.0000000001, 1.5, '1', 1), (2, 2, 1.00, 1.0000000001, 2.5, '2', 1), (3, 2, 1.01, 1.0000000002, 3.5, '2', 2), (4, 3, 2.50, 2.0, 4.5, '3', 2), (5, 5, 0.33, 3.3333333333, 5.5, '5', 2)
SELECT AVG(v) FROM av WHERE id <= 3
SELECT AVG(v) * 3 FROM av WHERE id <= 3
SELECT AVG(v) + 0 FROM av WHERE id <= 3
SELECT AVG(v) * 1 FROM av WHERE id <= 3
SELECT AVG(v) / 1 FROM av WHERE id <= 3
SELECT AVG(v) / 3 FROM av WHERE id <= 3
SELECT AVG(v) / 3 * 3 FROM av WHERE id <= 3
SELECT AVG(v) - 1 FROM av WHERE id <= 3
SELECT 5 - AVG(v) * 3 FROM av WHERE id <= 3
SELECT AVG(v) * 3.0 FROM av WHERE id <= 3
SELECT AVG(v) * 3.00 FROM av WHERE id <= 3
SELECT AVG(v) * AVG(v) FROM av WHERE id <= 3
SELECT AVG(v) + AVG(v) FROM av WHERE id <= 3
SELECT AVG(v) * 6 FROM av WHERE id <= 3
SELECT AVG(v) * 9 FROM av WHERE id <= 3
SELECT AVG(v) * 3 * 1000000000 FROM av WHERE id <= 3
SELECT AVG(v) * 1000000000 FROM av WHERE id <= 3
SELECT AVG(v) % 2 FROM av WHERE id <= 3
SELECT -AVG(v) FROM av WHERE id <= 3
SELECT ABS(AVG(v)) FROM av WHERE id <= 3
SELECT IFNULL(AVG(v), 0) * 3 FROM av WHERE id <= 3
SELECT COALESCE(AVG(v), 0) * 3 FROM av WHERE id <= 3
SELECT IF(AVG(v) > 1, AVG(v), 0) * 3 FROM av WHERE id <= 3
SELECT ROUND(AVG(v) * 3, 4) FROM av WHERE id <= 3
SELECT ROUND(AVG(v) * 3, 2) FROM av WHERE id <= 3
SELECT ROUND(AVG(v), 3) FROM av WHERE id <= 3
SELECT TRUNCATE(AVG(v) * 3, 3) FROM av WHERE id <= 3
SELECT ROUND(AVG(v), 2) * 3 FROM av WHERE id <= 3
SELECT TRUNCATE(AVG(v), 2) * 3 FROM av WHERE id <= 3
SELECT FLOOR(AVG(v) * 3) FROM av WHERE id <= 3
SELECT CEIL(AVG(v) * 3) FROM av WHERE id <= 3
SELECT CAST(AVG(v) AS CHAR) FROM av WHERE id <= 3
SELECT CONCAT(AVG(v), 'x') FROM av WHERE id <= 3
SELECT CAST(AVG(v) * 3 AS CHAR) FROM av WHERE id <= 3
SELECT AVG(v) = 1.6667 FROM av WHERE id <= 3
SELECT AVG(v) = 5 / 3 FROM av WHERE id <= 3
SELECT AVG(v) > 1.6666 FROM av WHERE id <= 3
SELECT AVG(v) < 1.6667 FROM av WHERE id <= 3
SELECT AVG(v) >= 1.6667 FROM av WHERE id <= 3
SELECT AVG(v) <> 1.6667 FROM av WHERE id <= 3
SELECT AVG(v) BETWEEN 1.6666 AND 1.6666667 FROM av WHERE id <= 3
SELECT AVG(v) BETWEEN 1.6667 AND 1.7 FROM av WHERE id <= 3
SELECT AVG(v) IN (1.6667) FROM av WHERE id <= 3
SELECT AVG(v) * 3 = 5 FROM av WHERE id <= 3
SELECT AVG(v) * 3 > 5 FROM av WHERE id <= 3
SELECT AVG(v) * 3 < 5 FROM av WHERE id <= 3
SELECT AVG(v) * 9 = 15 FROM av WHERE id <= 3
SELECT grp, AVG(v) * 3 FROM av GROUP BY grp ORDER BY grp
SELECT grp, AVG(v) * 3 FROM av WHERE id <> 1 GROUP BY grp ORDER BY grp
SELECT grp, AVG(v) FROM av GROUP BY grp HAVING AVG(v) = 1.6667
SELECT grp, AVG(v) FROM av GROUP BY grp HAVING AVG(v) = 2.5
SELECT grp, AVG(v) FROM av GROUP BY grp HAVING AVG(v) * 3 = 5
SELECT grp, AVG(v) FROM av GROUP BY grp HAVING AVG(v) < 3.3334 ORDER BY grp
SELECT grp, AVG(v) FROM av GROUP BY grp HAVING AVG(v) > 1.6666 AND AVG(v) < 1.6667 ORDER BY grp
SELECT grp, AVG(v) * 3 AS a3 FROM av GROUP BY grp HAVING a3 = 5
SELECT grp, AVG(v) FROM av GROUP BY grp ORDER BY AVG(v) * 3
SELECT grp, AVG(v) FROM av GROUP BY grp ORDER BY AVG(v) * 3 DESC
SELECT AVG(v) * 3 FROM av WHERE id <= 3 HAVING AVG(v) * 3 < 5
SELECT AVG(d) FROM av WHERE id <= 3
SELECT AVG(d) * 3 FROM av WHERE id <= 3
SELECT AVG(d) * 1 FROM av WHERE id <= 3
SELECT AVG(d) * 1000000000 FROM av WHERE id <= 3
SELECT AVG(d) / 3 FROM av WHERE id <= 3
SELECT AVG(d) + AVG(d) FROM av WHERE id <= 3
SELECT AVG(d) = 1.003333 FROM av WHERE id <= 3
SELECT AVG(d) > 1.003333 FROM av WHERE id <= 3
SELECT AVG(e) FROM av WHERE id <= 3
SELECT AVG(e) * 3 FROM av WHERE id <= 3
SELECT AVG(e) * 1 FROM av WHERE id <= 3
SELECT AVG(e) * 1000000000 FROM av WHERE id <= 3
SELECT AVG(e) * 10000000000 FROM av WHERE id <= 3
SELECT AVG(f) FROM av
SELECT AVG(f) * 3 FROM av
SELECT AVG(f) * 3 = 9 FROM av
SELECT AVG(g) FROM av WHERE id <= 3
SELECT AVG(g) * 3 FROM av WHERE id <= 3
SELECT AVG(v) FROM av WHERE id > 99
SELECT AVG(v) * 3 FROM av WHERE id > 99
SELECT AVG(DISTINCT v) FROM av
SELECT AVG(DISTINCT v) * 3 FROM av
SELECT AVG(DISTINCT v) = 2.5 FROM av
SELECT AVG(v) FROM av
SELECT AVG(v) * 3 FROM av
SELECT AVG(v) * 7 FROM av
SELECT AVG(v) / 7 FROM av
SELECT AVG(v) / 7 * 7 FROM av
SELECT AVG(v) * AVG(d) FROM av
SELECT AVG(v) + AVG(d) FROM av
SELECT AVG(v) / AVG(d) FROM av
SELECT AVG(v) + COUNT(*) FROM av
SELECT AVG(v) * COUNT(*) FROM av
SELECT AVG(v) + SUM(v) FROM av
SELECT SUM(v) / COUNT(*) * 3 FROM av WHERE id <= 3
SELECT SUM(v) / COUNT(*) = 5 / 3 FROM av WHERE id <= 3
SELECT id, AVG(v) OVER () * 3 FROM av WHERE id <= 3 ORDER BY id
SELECT id, AVG(v) OVER () FROM av WHERE id <= 3 ORDER BY id
SELECT id, AVG(v) OVER (PARTITION BY grp) * 3 FROM av ORDER BY id
SELECT id, AVG(v) OVER (ORDER BY id) * 3 FROM av ORDER BY id
SELECT id, AVG(v) OVER () = 1.6667 FROM av WHERE id <= 3 ORDER BY id
SELECT 5 / 3 = 1.6667
SELECT 5 / 3 * 3
SELECT (5 / 3) * 3 = 5
SELECT 1 / 3 * 3
SELECT 1 / 3 * 1000000000
SELECT grp, ROUND(AVG(v), 2) FROM av GROUP BY grp ORDER BY grp
SELECT grp, FORMAT(AVG(v) * 3, 4) FROM av GROUP BY grp ORDER BY grp
SELECT (SELECT AVG(v) * 3 FROM av WHERE id <= 3)
SELECT * FROM (SELECT AVG(v) * 3 AS x FROM av WHERE id <= 3) q
SELECT * FROM (SELECT AVG(v) AS x FROM av WHERE id <= 3) q WHERE x * 3 = 5
SELECT x * 3 FROM (SELECT AVG(v) AS x FROM av WHERE id <= 3) q
SELECT CASE WHEN AVG(v) > 1.6666 THEN 'a' ELSE 'b' END FROM av WHERE id <= 3
SELECT CASE WHEN AVG(v) = 1.6667 THEN 'a' ELSE 'b' END FROM av WHERE id <= 3
SELECT id FROM av WHERE v > (SELECT AVG(v) FROM av) ORDER BY id
SELECT id FROM av WHERE v * 3 > (SELECT AVG(v) * 3 FROM av WHERE id <= 3) ORDER BY id
SELECT id FROM av WHERE v = (SELECT AVG(v) FROM av WHERE id IN (2, 3)) ORDER BY id
SELECT id FROM av WHERE v + 0.0001 > (SELECT AVG(v) FROM av WHERE id <= 3) ORDER BY id
--DROP TABLE IF EXISTS tmp_avg
--CREATE TABLE tmp_avg AS SELECT AVG(v) AS a FROM av WHERE id <= 3
SELECT a, a * 3 FROM tmp_avg
SELECT a = 1.6667 FROM tmp_avg
SELECT IFNULL(AVG(v), 0) * 3 FROM av WHERE id > 99
SELECT COALESCE(AVG(v), 0) FROM av WHERE id > 99
SELECT AVG(v), AVG(v) * 3 FROM av WHERE id <= 3
SELECT AVG(DISTINCT v) FROM av
SELECT grp, AVG(v) FROM av GROUP BY grp ORDER BY AVG(v) * 3 DESC
SELECT ROUND(AVG(v), 3) FROM av WHERE id <= 3
