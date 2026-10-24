# A FLOAT column in arithmetic and comparisons (R50): MySQL reads the single-precision number widened to a DOUBLE, so 0.1 in a FLOAT column is 0.10000000149011612: it is not equal to the 0.1 of a DOUBLE
# column or of a literal, `f + 0` is 0.10000000149011612 and SUM(f) adds the widened numbers. What a FLOAT column shows is the six-digit text (corpus float.sql).
--CREATE TABLE fs (id INT PRIMARY KEY, f FLOAT, d DOUBLE, n DECIMAL(12,2), i INT)
--INSERT INTO fs VALUES (1,0.1,0.1,0.1,1),(2,0.5,0.5,0.5,2),(3,1.5,1.5,1.5,3),(4,NULL,NULL,NULL,NULL),(5,100000,100000,100000,100000),(6,3.14159,3.14159,3.14159,3)
SELECT f = d FROM fs WHERE id = 1
SELECT f = 0.1, d = 0.1, f = 0.5, f = 0.1e0 FROM fs ORDER BY id
SELECT id FROM fs WHERE f = d ORDER BY id
SELECT id FROM fs WHERE d = f ORDER BY id
SELECT id FROM fs WHERE f = 0.1
SELECT id FROM fs WHERE f = 0.5
SELECT id FROM fs WHERE f = n ORDER BY id
SELECT id FROM fs WHERE f = i ORDER BY id
SELECT id FROM fs WHERE f < d ORDER BY id
SELECT id FROM fs WHERE f > d ORDER BY id
SELECT id FROM fs WHERE f <> d ORDER BY id
SELECT id FROM fs WHERE f BETWEEN 0.1 AND 0.5 ORDER BY id
SELECT id FROM fs WHERE f IN (0.1, 0.5) ORDER BY id
SELECT id FROM fs WHERE f IN (0.5, 1.5) ORDER BY id
SELECT f + 0, f * 1, f + d, f - d FROM fs WHERE id = 1
SELECT f * 2, f / 2, -f FROM fs WHERE id IN (1, 2, 3) ORDER BY id
SELECT f + 0 FROM fs WHERE id IN (2, 3, 5) ORDER BY id
SELECT SUM(f), AVG(f), MIN(f), MAX(f) FROM fs
SELECT SUM(d), AVG(d) FROM fs
SELECT SUM(f) FROM fs WHERE id IN (2, 3)
SELECT id, SUM(f) OVER (ORDER BY id) FROM fs ORDER BY id
SELECT ABS(f), ROUND(f, 2), SQRT(f), FLOOR(f), CEIL(f) FROM fs WHERE id IN (1, 6) ORDER BY id
SELECT f, d FROM fs WHERE id = 1
SELECT f FROM fs ORDER BY f, id
SELECT f, COUNT(*) FROM fs GROUP BY f ORDER BY f
SELECT id FROM fs WHERE f > 0.4 AND f < 0.6
UPDATE fs SET f = f * 1.1 WHERE id = 3
SELECT f FROM fs WHERE id = 3
