# R96c: the names HAVING and WHERE read, compared with a real MySQL 8.4.9 --exact --errno.
# HAVING without GROUP BY and without an aggregate filters rows (it is not an aggregated query); HAVING reads the names of the select list (columns by name or as the source of an alias,
# aliases, `*` and `t.*`) and the columns grouped by -- any other name is unknown (1054), even one two tables have; two select items of one name are ambiguous (1052); inside an aggregate
# HAVING reads the columns of the tables (an ambiguous name is 1052). A select-list alias is not a name of WHERE, ON or the select list itself (1054): it used to be a column no row has.
--CREATE TABLE ua (x INT, a INT)
--CREATE TABLE ub (x INT, y INT, b INT)
--CREATE TABLE uc (y INT, c INT)
--CREATE TABLE ud (x INT, y INT, d INT)
--INSERT INTO ua VALUES (1, 10), (2, 20), (3, 30)
--INSERT INTO ub VALUES (1, 5, 100), (2, 6, 200), (3, 5, 300)
--INSERT INTO uc VALUES (5, 1000), (6, 2000)
--INSERT INTO ud VALUES (1, 5, 7), (2, 6, 8), (3, 5, 9)
SELECT a FROM ua JOIN ub USING (x) JOIN uc USING (y) JOIN ud USING (x) HAVING y > 0
SELECT a FROM ua JOIN ub ON ua.x = ub.x HAVING x > 0
SELECT ua.x, ub.x FROM ua JOIN ub ON ua.x = ub.x HAVING x > 0
SELECT a FROM ua JOIN ub ON ua.x = ub.x GROUP BY a HAVING x > 0
SELECT a FROM ua JOIN ub ON ua.x = ub.x HAVING ua.x > 0
SELECT a FROM ua JOIN ub ON ua.x = ub.x ORDER BY x
SELECT a FROM ua JOIN ub ON ua.x = ub.x GROUP BY x
SELECT a FROM ua JOIN ub ON ua.x = ub.x WHERE x > 0
SELECT a, COUNT(*) FROM ua JOIN ub ON ua.x = ub.x GROUP BY a HAVING COUNT(x) > 0
SELECT a AS x FROM ua JOIN ub ON ua.x = ub.x HAVING x > 0
SELECT * FROM ua JOIN ub ON ua.x = ub.x HAVING x > 0
SELECT ua.* FROM ua JOIN ub ON ua.x = ub.x HAVING x > 0
SELECT a FROM ua HAVING a > 10
SELECT a FROM ua HAVING x > 1
SELECT a FROM ua GROUP BY a HAVING a > 10
SELECT a FROM ua GROUP BY x HAVING x > 1
SELECT x AS k FROM ua HAVING x > 1
SELECT x AS k FROM ua HAVING k > 1
SELECT a + 1 AS k FROM ua HAVING k > 11
SELECT a FROM ua HAVING SUM(x) > 0
SELECT a FROM ua ORDER BY x
SELECT a FROM ua GROUP BY x HAVING a > 1
SELECT a + 1 FROM ua HAVING a > 11
SELECT a FROM ua HAVING ua.a > 10
SELECT ua.a FROM ua HAVING a > 10
SELECT a FROM ua HAVING ua.x > 1
SELECT COUNT(*) FROM ua HAVING x > 1
SELECT COUNT(*) FROM ua HAVING COUNT(*) > 1
SELECT a, a FROM ua HAVING a > 10
SELECT * FROM ua HAVING x > 1
SELECT x, a FROM ua WHERE x > 1 HAVING a > 20 ORDER BY a
SELECT a FROM ua WHERE x > 0 HAVING a > 10 AND a < 30
SELECT a FROM ua HAVING a IN (10, 20) ORDER BY a
SELECT a FROM ua HAVING (SELECT 1) = 1
SELECT 1 FROM ua HAVING a > 10
SELECT a FROM ua HAVING 1 = 1 ORDER BY a LIMIT 2
SELECT COUNT(*) FROM ua GROUP BY x HAVING a > 1
SELECT x, COUNT(*) FROM ua GROUP BY x HAVING a > 1
SELECT COUNT(*) FROM ua GROUP BY x HAVING x > 1 ORDER BY 1
SELECT a, COUNT(*) FROM ua GROUP BY a HAVING x > 1
SELECT x, COUNT(*) AS n FROM ua GROUP BY x HAVING n > 0 AND x > 1 ORDER BY x
SELECT x FROM ua GROUP BY x HAVING SUM(a) > 15 AND x > 1
SELECT x FROM ua GROUP BY x HAVING a > 15
SELECT ua.x FROM ua JOIN ub ON ua.x = ub.x GROUP BY ua.x HAVING b > 100
SELECT ua.x FROM ua JOIN ub ON ua.x = ub.x GROUP BY ua.x HAVING SUM(b) > 100
SELECT ua.x FROM ua JOIN ub ON ua.x = ub.x GROUP BY ua.x HAVING ub.x > 1
SELECT ua.x FROM ua JOIN ub ON ua.x = ub.x GROUP BY ua.x HAVING x > 1
SELECT ua.x FROM ua JOIN ub ON ua.x = ub.x HAVING x > 1
SELECT ua.x FROM ua JOIN ub ON ua.x = ub.x HAVING ua.x > 1
SELECT x FROM ua JOIN ub USING (x) HAVING x > 1
SELECT x FROM ua JOIN ub USING (x) GROUP BY x HAVING x > 1
SELECT a FROM ua JOIN ub USING (x) HAVING y > 5
SELECT a FROM ua JOIN ub USING (x) HAVING x > 1
SELECT a FROM ua JOIN ub USING (x) HAVING b > 100
SELECT x AS k FROM ua WHERE k > 1
SELECT x AS k FROM ua WHERE k = 1 OR x = 2
SELECT a + 1 AS k FROM ua WHERE k > 11
SELECT x AS k FROM ua GROUP BY k
SELECT x AS k FROM ua ORDER BY k
SELECT x AS k FROM ua WHERE x > 0 GROUP BY x HAVING k > 0
SELECT x AS k, k + 1 FROM ua
SELECT x AS k FROM ua WHERE EXISTS (SELECT 1 FROM ua u2 WHERE u2.x = k)
SELECT * FROM (SELECT x AS k FROM ua) d WHERE k > 1
SELECT x AS k FROM ua JOIN (SELECT 1 AS j) t ON j = x
SELECT x FROM ua WHERE x > 1 AND a IN (SELECT a AS x FROM ua WHERE x > 10)
SELECT a AS x FROM ua WHERE x > 1
