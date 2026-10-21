# MAX_EXECUTION_TIME (R46): a SELECT that runs longer than the hint or SET max_execution_time says fails with error 3024; the session goes on, other statements are not timed.
# (COUNT(*) over a cross join is left out: MySQL counts it in a join buffer within the limit, RuSQL stops it -- the limit is the aim there.)
SELECT /*+ MAX_EXECUTION_TIME(50) */ a.id FROM t a, t b, t c, t d, t e, t f, t g, t h ORDER BY a.id LIMIT 3
SELECT /*+ MAX_EXECUTION_TIME(50) */ a.g, SUM(b.i) FROM t a, t b, t c, t d, t e, t f, t g, t h GROUP BY a.g
SELECT /*+ MAX_EXECUTION_TIME(50) */ a.g, COUNT(*) FROM t a, t b, t c, t d, t e, t f, t g, t h WHERE a.i + b.i > 1 GROUP BY a.g
SELECT /*+ MAX_EXECUTION_TIME(60000) */ COUNT(*) FROM t a, t b
SELECT /*+ MAX_EXECUTION_TIME(0) */ COUNT(*) FROM t a, t b, t c
SELECT 1
SET max_execution_time = 50
SELECT @@max_execution_time
SELECT a.id FROM t a, t b, t c, t d, t e, t f, t g, t h ORDER BY a.id LIMIT 3
SELECT COUNT(*) FROM t a, t b
SET max_execution_time = 0
SELECT COUNT(*) FROM t a, t b, t c
SET SESSION max_execution_time = 50
SELECT @@session.max_execution_time
SELECT a.id FROM t a, t b, t c, t d, t e, t f, t g, t h ORDER BY a.id LIMIT 3
SET @@max_execution_time = 0
SELECT @@max_execution_time
