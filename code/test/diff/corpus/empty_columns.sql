# The columns of an answer of no rows (R56): a SELECT that finds nothing still says its columns (cursor.description of a driver, the headers of a client's grid), as the answer of a
# SELECT that finds rows does. The corpus compares the names MySQL gives with the names RuSQL gives, over every kind of select-list item and of FROM.
--CREATE TABLE ec_p (id INT PRIMARY KEY, name VARCHAR(10), v INT)
--CREATE TABLE ec_c (id INT PRIMARY KEY, pid INT, note VARCHAR(10), FOREIGN KEY (pid) REFERENCES ec_p(id))
--INSERT INTO ec_p VALUES (1, 'a', 10), (2, 'b', 20)
--INSERT INTO ec_c VALUES (1, 1, 'x'), (2, 2, 'y')
--CREATE VIEW ec_v AS SELECT id, name FROM ec_p
SELECT * FROM ec_p WHERE id < 0
SELECT id, name FROM ec_p WHERE id < 0
SELECT name, id FROM ec_p WHERE id < 0
SELECT id AS ident, name AS label FROM ec_p WHERE id < 0
SELECT p.id, p.name FROM ec_p p WHERE p.id < 0
SELECT ec_p.id FROM ec_p WHERE id < 0
SELECT p.* FROM ec_p p WHERE p.id < 0
SELECT p.*, c.note FROM ec_p p JOIN ec_c c ON c.pid = p.id WHERE p.id < 0
SELECT * FROM ec_p p JOIN ec_c c ON c.pid = p.id WHERE p.id < 0
SELECT p.id, c.id FROM ec_p p LEFT JOIN ec_c c ON c.pid = p.id WHERE p.id < 0
SELECT COUNT(*) FROM ec_p WHERE id < 0 GROUP BY name
SELECT name, COUNT(*) FROM ec_p WHERE id < 0 GROUP BY name
SELECT name, SUM(v) AS total FROM ec_p WHERE id < 0 GROUP BY name
SELECT name, MAX(v), MIN(v), AVG(v) FROM ec_p WHERE id < 0 GROUP BY name
SELECT name, COUNT(*) AS n FROM ec_p GROUP BY name HAVING COUNT(*) > 5
SELECT DISTINCT name FROM ec_p WHERE id < 0
SELECT id, name FROM ec_p WHERE id < 0 ORDER BY name DESC
SELECT id, name FROM ec_p ORDER BY id LIMIT 0
SELECT d.id, d.name FROM (SELECT id, name FROM ec_p WHERE id < 0) d
SELECT * FROM (SELECT id, v AS value FROM ec_p WHERE id < 0) d
SELECT * FROM ec_v WHERE id < 0
SELECT id, name FROM ec_v WHERE id < 0
WITH c AS (SELECT id, name FROM ec_p WHERE id < 0) SELECT * FROM c
WITH c (a, b) AS (SELECT id, name FROM ec_p WHERE id < 0) SELECT a, b FROM c
WITH c AS (SELECT id, v FROM ec_p) SELECT id, v FROM c WHERE v > 100
SELECT id FROM ec_p WHERE id < 0 UNION SELECT pid FROM ec_c WHERE id < 0
SELECT id AS first_name_column FROM ec_p WHERE id < 0 UNION ALL SELECT pid FROM ec_c WHERE id < 0
SELECT id, (SELECT COUNT(*) FROM ec_c WHERE ec_c.pid = ec_p.id) AS kids FROM ec_p WHERE id < 0
SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM ec_p WHERE id < 0
SELECT id, CASE WHEN v > 10 THEN 'big' ELSE 'small' END AS size FROM ec_p WHERE id < 0
SELECT id, UPPER(name) AS up FROM ec_p WHERE id < 0
SELECT id, v + 1 AS next FROM ec_p WHERE id < 0
SELECT NOW() FROM ec_p WHERE id < 0
SELECT name FROM ec_p WHERE id < 0 FOR UPDATE
SELECT `id`, `name` FROM ec_p WHERE id < 0
PREPARE ec_st FROM 'SELECT id, name FROM ec_p WHERE id < ?'
--SET @zero = 0
EXECUTE ec_st USING @zero
SELECT id, name FROM ec_p WHERE id > 5 AND id < 4
SHOW TABLES LIKE 'nomatch%'
SELECT id FROM ec_p WHERE id = 99
--CREATE TABLE ec_n (id INT PRIMARY KEY, name VARCHAR(10), z INT)
--INSERT INTO ec_n VALUES (1, 'a', 5)
SELECT * FROM ec_p JOIN ec_c USING (id) WHERE id < 0
SELECT * FROM ec_p LEFT JOIN ec_c USING (id) WHERE id < 0
SELECT * FROM ec_p RIGHT JOIN ec_c USING (id) WHERE id < 0
SELECT * FROM ec_p NATURAL JOIN ec_c WHERE id < 0
SELECT * FROM ec_p NATURAL JOIN ec_n WHERE id < 0
SELECT * FROM ec_p JOIN ec_n USING (id, name) WHERE id < 0
SELECT * FROM ec_p JOIN ec_c USING (id) JOIN ec_n USING (id) WHERE id < 0
SELECT id, name FROM ec_p JOIN ec_n USING (id, name) WHERE id < 0
SELECT p.*, c.note FROM ec_p p JOIN ec_c c USING (id) WHERE id < 0
SELECT * FROM ec_p p NATURAL JOIN ec_n n WHERE p.id < 0
SELECT * FROM ec_p JOIN ec_c USING (id) ORDER BY id
SELECT * FROM ec_p NATURAL JOIN ec_n
--CREATE PROCEDURE ec_proc_empty() BEGIN SELECT id, name FROM ec_p WHERE id < 0; END
--CREATE PROCEDURE ec_proc_star() BEGIN SELECT * FROM ec_p WHERE id < 0; END
--CREATE PROCEDURE ec_proc_join() BEGIN SELECT * FROM ec_p JOIN ec_c USING (id) WHERE id < 0; END
--CREATE PROCEDURE ec_proc_two() BEGIN SELECT id FROM ec_p; SELECT name AS label FROM ec_p WHERE id < 0; END
--CREATE PROCEDURE ec_proc_set() BEGIN DECLARE n INT DEFAULT 0; SELECT id AS ident FROM ec_p WHERE id < 0; SET n = 1; END
CALL ec_proc_empty()
CALL ec_proc_star()
CALL ec_proc_join()
CALL ec_proc_two()
CALL ec_proc_set()
SELECT 'a'
SELECT 'a b'
SELECT 'a', 'b'
SELECT 'a' AS x
SELECT 'a' FROM ec_p WHERE id < 0
SELECT 'a' FROM ec_p WHERE id = 1
SELECT 'it''s'
SELECT 'a' + 1
SELECT UPPER('a')
SELECT 1 WHERE 1 = 0
SELECT 1 WHERE 1 = 1
SELECT 'a' WHERE 1 = 0
SELECT 1, 'b' WHERE 2 > 1
SELECT 1 WHERE NULL
SELECT 1 WHERE EXISTS (SELECT 1 FROM ec_p WHERE id = 1)
SELECT 1 WHERE EXISTS (SELECT 1 FROM ec_p WHERE id < 0)
SELECT 1 WHERE 1 IN (SELECT id FROM ec_p)
SELECT 1 AS one WHERE 1 = 0 UNION SELECT 2
SELECT 1 WHERE 1 = 0 UNION ALL SELECT 2
SELECT 2 UNION ALL SELECT 1 WHERE 1 = 0
SELECT * FROM ec_p LEFT JOIN ec_c USING (id) ORDER BY id
SELECT * FROM ec_p RIGHT JOIN ec_c USING (id) ORDER BY id
SELECT * FROM ec_p RIGHT JOIN ec_c USING (id) JOIN ec_n USING (id) ORDER BY id
SELECT ec_c.*, ec_p.name FROM ec_p RIGHT JOIN ec_c USING (id) ORDER BY id
