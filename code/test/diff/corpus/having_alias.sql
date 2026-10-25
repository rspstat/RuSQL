# HAVING names a select-list alias (R52): the alias of an expression, an aggregate, a column, a function or a CASE; MySQL reads the alias before a column of the
# same name, also when the table has a column called like the alias (`d`, `v`, `name`, `id`).
--CREATE TABLE ha (id INT PRIMARY KEY, g INT, name VARCHAR(10), v INT, d DATE)
--INSERT INTO ha VALUES (1,1,'a',10,'2024-01-01'),(2,1,'a',10,'2024-01-01'),(3,2,'b',20,'2024-02-02'),(4,3,'c',NULL,NULL),(5,3,'c',5,'2024-03-03')
SELECT g, SUM(v) * 2 AS total2 FROM ha GROUP BY g HAVING total2 = 40 ORDER BY g
SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d = 40 ORDER BY g
SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d > 15 ORDER BY g
SELECT g, COUNT(*) * 2 AS name FROM ha GROUP BY g HAVING name = 4 ORDER BY g
SELECT g, ANY_VALUE(v) * 2 AS d FROM ha GROUP BY g HAVING d = 40 ORDER BY g
SELECT g, SUM(v) AS v FROM ha GROUP BY g HAVING v > 15 ORDER BY g
SELECT g, COUNT(*) AS id FROM ha GROUP BY g HAVING id = 2
SELECT g, name AS n FROM ha GROUP BY g, name HAVING n = 'a'
SELECT g, name AS d FROM ha GROUP BY g, name HAVING d = 'b'
SELECT g, UPPER(name) AS u FROM ha GROUP BY g, name HAVING u = 'A'
SELECT g, CASE WHEN SUM(v) > 15 THEN 'big' ELSE 'small' END AS size FROM ha GROUP BY g HAVING size = 'big' ORDER BY g
SELECT g, SUM(v) AS s, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d > 15 AND s < 30 ORDER BY g
SELECT g, ANY_VALUE(d) AS first_d, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d = 40 ORDER BY g
SELECT g + 1 AS g1, COUNT(*) FROM ha GROUP BY g + 1 HAVING g1 = 2
SELECT SUM(v) * 2 AS d FROM ha HAVING d = 90
SELECT SUM(v) * 2 AS d FROM ha HAVING d = 91
SELECT g, SUM(v) * 2 AS d FROM ha WHERE d IS NOT NULL GROUP BY g ORDER BY g
SELECT g, SUM(v) AS total FROM ha GROUP BY g HAVING total > 10 AND total < 25 ORDER BY total DESC
SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d IS NULL
SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING nosuch = 1
SELECT g, SUM(v) * 2 AS dbl FROM ha GROUP BY g HAVING DBL = 40 ORDER BY g
SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING D = 40 ORDER BY g
SELECT g, name AS N FROM ha GROUP BY g, name HAVING n = 'a'
SELECT g AS grp_id, COUNT(*) AS c FROM ha GROUP BY g HAVING grp_id = 1
SELECT COUNT(*) AS c FROM ha HAVING c > 3
SELECT COUNT(*) AS c FROM ha HAVING c > 7
SELECT g, SUM(v) AS s FROM ha GROUP BY g HAVING s * 2 > 30 ORDER BY g
SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d IN (40, 10) ORDER BY g
SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d BETWEEN 10 AND 40 ORDER BY g
SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d = (SELECT MAX(v) * 2 FROM ha) ORDER BY g
SELECT g, SUM(v) * 2 AS d, name AS n FROM ha GROUP BY g, name HAVING d > 15 AND n = 'a'
SELECT a.g, SUM(b.v) * 2 AS d FROM ha a JOIN ha b ON a.g = b.g GROUP BY a.g HAVING d > 50 ORDER BY a.g
SELECT DISTINCT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d > 15 ORDER BY g
SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d > 15 ORDER BY d DESC, g LIMIT 1
SELECT g, ANY_VALUE(name) AS d FROM ha GROUP BY g HAVING d = 'a'
SELECT g, SUM(v) * 2 AS d FROM ha GROUP BY g HAVING d = d
