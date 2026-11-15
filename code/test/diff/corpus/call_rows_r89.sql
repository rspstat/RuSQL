# R89 (2026-11-16): the OK packet and ROW_COUNT() of CALL: the row count of the last statement the body ran that sets one (INSERT / UPDATE / DELETE: the rows; SELECT ... INTO: 1; SET @var, DDL: 0;
# a query that gives a result set: 0; SET of a local variable, DECLARE, IF / WHILE do not change it). Measured on MySQL 8.4.9. A line is a statement, '--' a setup statement run on both.
# Run: python vs_mysql.py <engine_server.exe> corpus/call_rows_r89.sql --proto --errno
--DROP TABLE IF EXISTS t
--DROP TABLE IF EXISTS q
--CREATE TABLE t (id INT PRIMARY KEY, a INT)
--INSERT INTO t VALUES (1,1),(2,2),(3,3),(4,4)
--DROP PROCEDURE IF EXISTS p_insert2
--CREATE PROCEDURE p_insert2() BEGIN INSERT INTO t VALUES (10,1),(11,1); END
--DROP PROCEDURE IF EXISTS p_update3
--CREATE PROCEDURE p_update3() BEGIN UPDATE t SET a = a + 100 WHERE id <= 3; END
--DROP PROCEDURE IF EXISTS p_update_none
--CREATE PROCEDURE p_update_none() BEGIN UPDATE t SET a = a WHERE id <= 3; END
--DROP PROCEDURE IF EXISTS p_delete1
--CREATE PROCEDURE p_delete1() BEGIN DELETE FROM t WHERE id = 10; END
--DROP PROCEDURE IF EXISTS p_select_into
--CREATE PROCEDURE p_select_into() BEGIN SELECT COUNT(*) INTO @n FROM t; END
--DROP PROCEDURE IF EXISTS p_select
--CREATE PROCEDURE p_select() BEGIN SELECT * FROM t; END
--DROP PROCEDURE IF EXISTS p_set
--CREATE PROCEDURE p_set() BEGIN SET @x = 5; END
--DROP PROCEDURE IF EXISTS p_two
--CREATE PROCEDURE p_two() BEGIN INSERT INTO t VALUES (20,1); UPDATE t SET a = 0 WHERE id <= 2; END
--DROP PROCEDURE IF EXISTS p_insert_then_select
--CREATE PROCEDURE p_insert_then_select() BEGIN INSERT INTO t VALUES (21,1); SELECT 1; END
--DROP PROCEDURE IF EXISTS p_insert_then_set
--CREATE PROCEDURE p_insert_then_set() BEGIN INSERT INTO t VALUES (22,1); SET @y = 1; END
--DROP PROCEDURE IF EXISTS p_declare
--CREATE PROCEDURE p_declare() BEGIN DECLARE v INT DEFAULT 0; SELECT COUNT(*) INTO v FROM t; END
--DROP PROCEDURE IF EXISTS p_ddl
--CREATE PROCEDURE p_ddl() BEGIN CREATE TABLE IF NOT EXISTS q (z INT); END
--DROP PROCEDURE IF EXISTS p_if
--CREATE PROCEDURE p_if() BEGIN IF 1 = 1 THEN UPDATE t SET a = a + 1 WHERE id = 1; END IF; END
--DROP PROCEDURE IF EXISTS p_loop
--CREATE PROCEDURE p_loop() BEGIN DECLARE i INT DEFAULT 0; WHILE i < 3 DO INSERT INTO t VALUES (100 + i, i); SET i = i + 1; END WHILE; END
--DROP PROCEDURE IF EXISTS p_update_then_local
--CREATE PROCEDURE p_update_then_local() BEGIN DECLARE k INT DEFAULT 0; UPDATE t SET a = a + 1 WHERE id <= 2; SET k = 5; END
CALL p_insert2()
SELECT ROW_COUNT()
CALL p_update3()
SELECT ROW_COUNT()
CALL p_update_none()
SELECT ROW_COUNT()
CALL p_delete1()
SELECT ROW_COUNT()
CALL p_select_into()
SELECT ROW_COUNT()
CALL p_select()
SELECT ROW_COUNT()
CALL p_set()
SELECT ROW_COUNT()
CALL p_two()
SELECT ROW_COUNT()
CALL p_insert_then_select()
SELECT ROW_COUNT()
CALL p_insert_then_set()
SELECT ROW_COUNT()
CALL p_declare()
SELECT ROW_COUNT()
CALL p_ddl()
SELECT ROW_COUNT()
CALL p_if()
SELECT ROW_COUNT()
CALL p_loop()
SELECT ROW_COUNT()
CALL p_update_then_local()
SELECT ROW_COUNT()
