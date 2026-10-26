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
