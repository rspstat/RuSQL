# The order of the columns of `*` over a chain of joins with USING / NATURAL (R58): MySQL reads (a JOIN b USING (x)) JOIN c USING (y) as a tree -- each join puts its merged columns
# in front, then what is left of its left side, then what is left of its right side -- so a(x,y,a) JOIN b(x,b) USING (x) JOIN c(y,c) USING (y) shows y, x, a, b, c.
--CREATE TABLE ua (x INT, y INT, a INT)
--CREATE TABLE ub (x INT, b INT)
--CREATE TABLE uc (y INT, c INT)
--CREATE TABLE ud (x INT, y INT, z INT)
--INSERT INTO ua VALUES (1,10,100),(2,20,200),(3,30,300)
--INSERT INTO ub VALUES (1,11),(2,22),(4,44)
--INSERT INTO uc VALUES (10,111),(20,222),(50,555)
--INSERT INTO ud VALUES (1,10,7),(2,99,8)
SELECT * FROM ua JOIN ub USING (x) JOIN uc USING (y) ORDER BY y
SELECT * FROM ua JOIN ub USING (x) JOIN uc USING (y) WHERE 1 = 0
SELECT * FROM ua LEFT JOIN ub USING (x) LEFT JOIN uc USING (y) ORDER BY a
SELECT * FROM ua JOIN ub USING (x) JOIN ud USING (x, y) ORDER BY x
SELECT * FROM ua JOIN ub USING (x) JOIN ud USING (y, x) ORDER BY x
SELECT * FROM ua JOIN ub ON ua.x = ub.x JOIN uc USING (y) ORDER BY a
SELECT * FROM ua JOIN uc USING (y) JOIN ub ON ua.x = ub.x ORDER BY a
SELECT * FROM ua RIGHT JOIN ub USING (x) JOIN uc USING (y) ORDER BY y
SELECT * FROM ua JOIN uc USING (y) RIGHT JOIN ub USING (x) ORDER BY b
SELECT * FROM ua NATURAL JOIN ub NATURAL JOIN uc ORDER BY y
SELECT * FROM ua NATURAL JOIN ud ORDER BY x
SELECT * FROM ud NATURAL JOIN ua ORDER BY x
SELECT * FROM ua JOIN ud USING (y, x) ORDER BY x
SELECT * FROM ud JOIN ua USING (y, x) ORDER BY x
SELECT * FROM ua JOIN ub USING (x) JOIN uc USING (y) JOIN ud USING (x, y) ORDER BY y
SELECT * FROM ua JOIN ub USING (x) JOIN uc USING (y) JOIN ud USING (y, x) ORDER BY y
SELECT * FROM ud RIGHT JOIN ua USING (y, x) ORDER BY x
SELECT * FROM ua RIGHT JOIN ud USING (y, x) ORDER BY x
SELECT ua.*, uc.* FROM ua JOIN ub USING (x) JOIN uc USING (y) ORDER BY ua.a
SELECT x, y, a, b, c FROM ua JOIN ub USING (x) JOIN uc USING (y) ORDER BY y
SELECT COUNT(*) FROM ua JOIN ub USING (x) JOIN uc USING (y)
