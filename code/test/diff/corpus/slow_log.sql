# The slow query log's two server switches (R59) and FOUND_ROWS() after an aggregate: SET GLOBAL slow_query_log takes ON / OFF / TRUE / FALSE / 1 / 0 (and the same as text) and
# refuses another word or number with 1231; long_query_time takes a number, refuses text with 1232 and CLAMPS a number out of range to 0 .. 31536000 (a warning, no error); a
# one-row answer of an aggregate (no "N rows" line in RuSQL's text) is one row to FOUND_ROWS(), as every other answer is the rows it has.
--CREATE TABLE sl (id INT PRIMARY KEY, g INT, v INT)
--INSERT INTO sl VALUES (1, 1, 10), (2, 1, 20), (3, 2, 30), (4, 2, NULL), (5, 3, 50)
SELECT @@global.slow_query_log, @@global.long_query_time
SET GLOBAL slow_query_log = ON
SELECT @@slow_query_log, @@global.slow_query_log
SET GLOBAL slow_query_log = 'OFF'
SELECT @@slow_query_log
SET GLOBAL slow_query_log = TRUE
SELECT @@slow_query_log
SET GLOBAL slow_query_log = FALSE
SELECT @@slow_query_log
SET GLOBAL slow_query_log = 1
SELECT @@slow_query_log
SET GLOBAL slow_query_log = 0
SELECT @@slow_query_log
SET GLOBAL slow_query_log = maybe
SET GLOBAL slow_query_log = 2
SELECT @@slow_query_log
SET GLOBAL long_query_time = 0.5
SELECT @@global.long_query_time
SET GLOBAL long_query_time = 0
SELECT @@global.long_query_time
SET GLOBAL long_query_time = 2.25
SELECT @@global.long_query_time
SET GLOBAL long_query_time = 'abc'
SET GLOBAL long_query_time = -1
SELECT @@global.long_query_time
SET GLOBAL long_query_time = 99999999999
SELECT @@global.long_query_time
SET GLOBAL long_query_time = 10
SELECT @@global.long_query_time
SELECT COUNT(*) FROM sl
SELECT FOUND_ROWS()
SELECT SUM(v), MAX(v) FROM sl
SELECT FOUND_ROWS()
SELECT COUNT(*) FROM sl WHERE id > 100
SELECT FOUND_ROWS()
SELECT SUM(v) FROM sl WHERE id > 100
SELECT FOUND_ROWS()
SELECT COUNT(DISTINCT g), AVG(v) FROM sl
SELECT FOUND_ROWS()
SELECT MAX(v) FROM sl HAVING MAX(v) > 100
SELECT FOUND_ROWS()
SELECT g, COUNT(*) FROM sl GROUP BY g
SELECT FOUND_ROWS()
SELECT g FROM sl GROUP BY g HAVING COUNT(*) > 100
SELECT FOUND_ROWS()
SELECT v FROM sl WHERE id = 1
SELECT FOUND_ROWS()
SELECT DISTINCT g FROM sl
SELECT FOUND_ROWS()
SELECT 1
SELECT FOUND_ROWS()
SELECT id FROM sl LIMIT 2
SELECT FOUND_ROWS()
