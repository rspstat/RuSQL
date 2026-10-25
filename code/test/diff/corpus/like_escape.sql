# LIKE and its escape character (R54): `\%` and `\_` are two characters in a string (the pattern then reads them as the characters themselves), a backslash that ends the
# pattern is a backslash, `ESCAPE 'c'` names another escape character (an empty one: none), and the plain prefix `LIKE 'ab%'` still reads an index.
SELECT LENGTH('a\_b'), LENGTH('a\%'), LENGTH('a\qb'), 'a\qb'
SELECT 'a%b' LIKE 'a\%b', 'axb' LIKE 'a\%b', 'a_b' LIKE 'a\_b', 'axb' LIKE 'a\_b', '%' LIKE '\%', 'x' LIKE '\%', 'a%' LIKE 'a%\%', 'ab' LIKE 'a%\%'
SELECT 'a\\' LIKE 'a\\\\', 'a\\' LIKE 'a\\', 'a' LIKE 'a\\', 'a\\b' LIKE 'a\\\\b', 'ab' LIKE 'a\\b'
SELECT 'a%b' LIKE 'a!%b' ESCAPE '!', 'axb' LIKE 'a!%b' ESCAPE '!', 'a!b' LIKE 'a!!b' ESCAPE '!', 'a_b' LIKE 'a!_b' ESCAPE '!', 'axb' LIKE 'a!_b' ESCAPE '!'
SELECT 'a\\b' LIKE 'a\\b' ESCAPE '!', 'a\\%b' LIKE 'a\%b' ESCAPE '!', 'axb' LIKE 'a\%b' ESCAPE '!', 'a%b' LIKE 'a\%b' ESCAPE '!'
SELECT 'a!' LIKE 'a!' ESCAPE '!', 'a' LIKE 'a!' ESCAPE '!', 'a#' LIKE 'a##' ESCAPE '#', 'a#b' LIKE 'a#%b' ESCAPE '#'
SELECT 'a\\b' LIKE 'a\\b' ESCAPE '', 'ab' LIKE 'a\\b' ESCAPE '', 'a%b' LIKE 'a\\%b' ESCAPE '\\'
SELECT 'axb' NOT LIKE 'a!%b' ESCAPE '!', 'a%b' NOT LIKE 'a!%b' ESCAPE '!'
SELECT 'AB' LIKE 'a\%' , 'a%' LIKE 'A\%', 'é_' LIKE 'e\_', 'e_' LIKE 'É\_'
SELECT NULL LIKE 'a\%', 'a' LIKE NULL ESCAPE '!'
SELECT 'a' LIKE 'a' ESCAPE '!!'
--CREATE TABLE lk (id INT PRIMARY KEY, name VARCHAR(20))
--CREATE INDEX ix_name ON lk (name)
--INSERT INTO lk VALUES (1,'a_1'),(2,'ab1'),(3,'a_2'),(4,'a%x'),(5,'axx'),(6,'b_1'),(7,'a\\b'),(8,'a'),(9,'A_9'),(10,NULL)
SELECT id FROM lk WHERE name LIKE 'a\_%' ORDER BY id
SELECT id FROM lk WHERE name LIKE 'a\%%' ORDER BY id
SELECT id FROM lk WHERE name LIKE 'a!_%' ESCAPE '!' ORDER BY id
SELECT id FROM lk WHERE name LIKE 'a%' AND name NOT LIKE 'a\_%' ORDER BY id
SELECT id FROM lk WHERE name LIKE 'a%' ORDER BY id
SELECT id FROM lk WHERE name NOT LIKE 'a%' ORDER BY id
SELECT id FROM lk WHERE name LIKE 'a\\\\b' ORDER BY id
SELECT id FROM lk WHERE name LIKE 'a\\b%' ORDER BY id
SELECT id FROM lk WHERE name LIKE 'a\\\\b%' ORDER BY id
SELECT id FROM lk WHERE name LIKE 'ab\\%' ORDER BY id
SELECT id FROM lk WHERE name LIKE '%\_%' ORDER BY id
SELECT id FROM lk WHERE name LIKE '%\%%' ORDER BY id
SELECT id FROM lk WHERE name LIKE '_\__' ORDER BY id
SELECT id FROM lk WHERE name LIKE 'a#_%' ESCAPE '#' ORDER BY id
SELECT id FROM lk WHERE name NOT LIKE 'a#_%' ESCAPE '#' ORDER BY id
SELECT COUNT(*) FROM lk WHERE name LIKE 'a\_%'
UPDATE lk SET name = 'z' WHERE name LIKE 'a\_1'
DELETE FROM lk WHERE name LIKE 'b\_%'
SELECT id, name FROM lk ORDER BY id
--CREATE TABLE pat (id INT PRIMARY KEY, p VARCHAR(10))
--INSERT INTO pat VALUES (1, 'a!_'), (2, 'a\\_'), (3, 'a%'), (4, 'a\\%'), (5, NULL)
SELECT id FROM pat WHERE 'a_' LIKE p ORDER BY id
SELECT id FROM pat WHERE 'a%' LIKE p ORDER BY id
SELECT id FROM pat WHERE 'ab' LIKE p ORDER BY id
SELECT id, 'a_' LIKE p ESCAPE '!' FROM pat ORDER BY id
SELECT id, 'a%' LIKE p ESCAPE '!' FROM pat ORDER BY id
SELECT id FROM pat WHERE 'a_' LIKE CONCAT('a', '!', '_') ESCAPE '!' ORDER BY id
SELECT id FROM pat WHERE 'ab' LIKE CONCAT('a', '!', '_') ESCAPE '!' ORDER BY id
SELECT id FROM pat WHERE 'a!' LIKE CONCAT(p, '!') ESCAPE '!' ORDER BY id
--CREATE TABLE xak (a INT)
--CREATE TABLE x_k (a INT)
SHOW TABLES LIKE 'x\_k'
SHOW TABLES LIKE 'x_k'
SHOW TABLES LIKE 'x%'
