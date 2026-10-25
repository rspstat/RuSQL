# ANY_VALUE (R45): the value of some row of the group, so a column the group does not fix can be selected under ONLY_FULL_GROUP_BY. Groups here hold one value, so the
# answer does not depend on which row MySQL picks.
--CREATE TABLE av (id INT PRIMARY KEY, grp INT, name VARCHAR(10), val INT, d DATE)
--INSERT INTO av VALUES (1,1,'a',10,'2024-01-01'),(2,1,'a',10,'2024-01-01'),(3,2,'b',20,'2024-02-02'),(4,3,NULL,NULL,NULL)
SELECT grp, ANY_VALUE(name), COUNT(*) FROM av GROUP BY grp ORDER BY grp
SELECT grp, ANY_VALUE(val) FROM av GROUP BY grp ORDER BY grp
SELECT grp, ANY_VALUE(d), ANY_VALUE(name) AS n FROM av GROUP BY grp ORDER BY grp
SELECT ANY_VALUE(name) FROM av WHERE grp = 1
SELECT ANY_VALUE(val) FROM av WHERE grp = 3
SELECT ANY_VALUE(val) FROM av WHERE grp = 99
SELECT grp, ANY_VALUE(name) FROM av GROUP BY grp ORDER BY ANY_VALUE(name), grp
SELECT grp, ANY_VALUE(val) + 1 FROM av GROUP BY grp ORDER BY grp
SELECT grp, ANY_VALUE(name), SUM(val), MAX(id) FROM av GROUP BY grp ORDER BY grp
SELECT id, ANY_VALUE(name) FROM av GROUP BY id ORDER BY id
SELECT ANY_VALUE(id) FROM av WHERE id = 3
SELECT ANY_VALUE(name), ANY_VALUE(val) FROM av WHERE grp = 2
SELECT grp, name FROM av GROUP BY grp
SELECT grp, ANY_VALUE(name) FROM av
SELECT COUNT(*), ANY_VALUE(grp) FROM av WHERE grp = 2
SELECT grp FROM av GROUP BY grp HAVING ANY_VALUE(val) > 5
SELECT grp, val FROM av GROUP BY grp, val HAVING ANY_VALUE(val) > 5 ORDER BY grp
SELECT grp, ANY_VALUE(val) FROM av GROUP BY grp HAVING ANY_VALUE(val) > 5
SELECT grp, SUM(val) FROM av GROUP BY grp HAVING SUM(val) > 5 AND ANY_VALUE(val) > 1
# R51: ORDER BY ANY_VALUE(x) of a column the select list does not hold, and a HAVING that names the alias of ANY_VALUE(x) (both used to be silently wrong)
SELECT grp, COUNT(*) FROM av GROUP BY grp ORDER BY ANY_VALUE(val) DESC, grp
SELECT grp, COUNT(*) FROM av GROUP BY grp ORDER BY ANY_VALUE(name), grp
SELECT grp FROM av GROUP BY grp ORDER BY ANY_VALUE(val) + grp
SELECT grp, ANY_VALUE(val) AS a FROM av GROUP BY grp HAVING a > 5 ORDER BY grp
SELECT grp, ANY_VALUE(val) * 2 AS dbl FROM av GROUP BY grp HAVING dbl = 40
SELECT grp, ANY_VALUE(val) AS a FROM av GROUP BY grp HAVING a IS NULL
SELECT grp, SUM(val) AS s, ANY_VALUE(val) AS a FROM av GROUP BY grp HAVING s > 5 AND a > 5 ORDER BY ANY_VALUE(val) DESC, grp
