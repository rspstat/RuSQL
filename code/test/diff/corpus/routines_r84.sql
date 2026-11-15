# R84 (2026-11-15): who may see the text of a routine, the account and the time a routine is made by, a function's name as it was written (RECOMMAND "R83 leftovers").
# A line is a statement, '--' a setup statement run on both, '@name ' a statement run by that user (password 'pw'). Run: python vs_mysql.py <engine_server.exe> corpus/routines_r84.sql --proto --errno
# (the creation time is a different value on the two servers: the lines below compare only that it is there and how it is written)
--DROP USER IF EXISTS 'alice'@'%'
--DROP USER IF EXISTS 'bob'@'%'
--DROP USER IF EXISTS 'carol'@'%'
--DROP USER IF EXISTS 'dan'@'%'
--DROP USER IF EXISTS 'erin'@'%'
--DROP USER IF EXISTS 'fran'@'%'
--DROP USER IF EXISTS 'gina'@'%'
--DROP USER IF EXISTS 'hal'@'%'
--DROP USER IF EXISTS 'ivy'@'%'
--DROP USER IF EXISTS 'jon'@'%'
--CREATE USER 'alice'@'%' IDENTIFIED BY 'pw'
--CREATE USER 'bob'@'%' IDENTIFIED BY 'pw'
--CREATE USER 'carol'@'%' IDENTIFIED BY 'pw'
--CREATE USER 'dan'@'%' IDENTIFIED BY 'pw'
--CREATE USER 'erin'@'%' IDENTIFIED BY 'pw'
--CREATE USER 'fran'@'%' IDENTIFIED BY 'pw'
--CREATE USER 'gina'@'%' IDENTIFIED BY 'pw'
--CREATE USER 'hal'@'%' IDENTIFIED BY 'pw'
--CREATE USER 'ivy'@'%' IDENTIFIED BY 'pw'
--CREATE USER 'jon'@'%' IDENTIFIED BY 'pw'
--DROP TABLE IF EXISTS x84
--DROP PROCEDURE IF EXISTS p84
--DROP PROCEDURE IF EXISTS pd84
--DROP FUNCTION IF EXISTS f84
--DROP FUNCTION IF EXISTS MixedFn84
--CREATE TABLE x84 (id INT PRIMARY KEY, n INT)
--CREATE PROCEDURE p84() SELECT 1
--CREATE FUNCTION f84() RETURNS INT DETERMINISTIC RETURN 1
--CREATE FUNCTION MixedFn84(a INT) RETURNS INT DETERMINISTIC RETURN a + 1
--CREATE TRIGGER tr84 BEFORE INSERT ON x84 FOR EACH ROW SET NEW.n = 1
--CREATE DEFINER=`bob`@`%` PROCEDURE pd84() SELECT 2
--GRANT SELECT ON v.x84 TO 'alice'@'%'
--GRANT EXECUTE ON PROCEDURE v.p84 TO 'bob'@'%'
--GRANT SELECT ON v.x84 TO 'carol'@'%'
--GRANT CREATE ROUTINE ON v.* TO 'dan'@'%'
--GRANT TRIGGER ON v.x84 TO 'erin'@'%'
--GRANT SELECT ON v.x84 TO 'fran'@'%'
--GRANT SHOW_ROUTINE ON *.* TO 'fran'@'%'
--GRANT ALTER ROUTINE ON FUNCTION v.f84 TO 'gina'@'%'
--GRANT SELECT ON v.x84 TO 'gina'@'%'
--GRANT EXECUTE ON v.* TO 'hal'@'%'
--GRANT ALTER ROUTINE ON v.* TO 'ivy'@'%'
--GRANT SELECT ON *.* TO 'jon'@'%'
--CREATE DEFINER=`dan`@`%` PROCEDURE pdan84() SELECT 3
# --- the account and the time (a routine: to the second, a trigger: to a hundredth)
SELECT routine_name, definer FROM information_schema.routines WHERE routine_name IN ('p84', 'f84', 'pd84') ORDER BY routine_name
SELECT created IS NOT NULL, last_altered = created, last_altered IS NOT NULL FROM information_schema.routines WHERE routine_name = 'p84'
SELECT created IS NOT NULL, last_altered IS NOT NULL FROM information_schema.routines WHERE routine_name = 'f84'
SELECT trigger_name, definer, created IS NOT NULL FROM information_schema.triggers WHERE trigger_name = 'tr84'
SELECT MICROSECOND(created) % 10000 FROM information_schema.triggers WHERE trigger_name = 'tr84'
SELECT MICROSECOND(created) FROM information_schema.routines WHERE routine_name = 'p84'
SELECT TIMESTAMPDIFF(MINUTE, created, NOW()) BETWEEN -1 AND 5 FROM information_schema.routines WHERE routine_name = 'p84'
SHOW CREATE PROCEDURE pd84
SHOW CREATE PROCEDURE p84
SHOW CREATE FUNCTION f84
SHOW CREATE TRIGGER nope84
# --- a function's name as it was written
SELECT routine_name, specific_name FROM information_schema.routines WHERE routine_name = 'MixedFn84'
SHOW CREATE FUNCTION MixedFn84
SHOW CREATE FUNCTION mixedfn84
SELECT MixedFn84(1), mixedfn84(2), MIXEDFN84(3)
# --- who may see the text of a routine
@alice SHOW CREATE PROCEDURE p84
@alice SHOW CREATE PROCEDURE v.p84
@alice SHOW CREATE FUNCTION f84
@alice SHOW CREATE TRIGGER tr84
@alice SHOW CREATE DATABASE v
@bob SHOW CREATE PROCEDURE p84
@bob SHOW CREATE PROCEDURE v.p84
@bob SHOW CREATE FUNCTION f84
@bob SHOW CREATE TRIGGER tr84
@bob SHOW CREATE DATABASE v
@carol SHOW CREATE PROCEDURE p84
@carol SHOW CREATE FUNCTION f84
@carol SHOW CREATE TRIGGER tr84
@dan SHOW CREATE PROCEDURE p84
@dan SHOW CREATE FUNCTION f84
@dan SHOW CREATE TRIGGER tr84
@dan SHOW CREATE DATABASE v
@erin SHOW CREATE PROCEDURE p84
@erin SHOW CREATE FUNCTION f84
@erin SELECT trigger_name, event_manipulation, action_timing, action_statement FROM information_schema.triggers WHERE trigger_schema = 'v'
@fran SHOW CREATE PROCEDURE p84
@fran SHOW CREATE FUNCTION f84
@fran SHOW CREATE TRIGGER tr84
@fran SHOW CREATE DATABASE v
@gina SHOW CREATE PROCEDURE p84
@hal SHOW CREATE PROCEDURE p84
@hal SHOW CREATE FUNCTION f84
@ivy SHOW CREATE PROCEDURE p84
@jon SHOW CREATE PROCEDURE p84
@jon SHOW CREATE FUNCTION f84
@jon SHOW CREATE TRIGGER tr84
@dan SHOW CREATE PROCEDURE pdan84
@bob SHOW CREATE PROCEDURE pdan84
@gina SHOW CREATE FUNCTION f84
@gina SHOW CREATE TRIGGER tr84
@alice SHOW CREATE PROCEDURE nope84
@alice SHOW CREATE TRIGGER nope84
@dan SHOW CREATE PROCEDURE nope84
@dan SHOW CREATE TRIGGER nope84
# --- who sees a routine in a list (SHOW ... STATUS and information_schema read the same rows)
@alice SELECT routine_name FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@bob SELECT routine_name FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@carol SELECT routine_name FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@dan SELECT routine_name FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@erin SELECT routine_name FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@fran SELECT routine_name FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@gina SELECT routine_name FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@hal SELECT routine_name, routine_definition IS NULL FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@ivy SELECT routine_name FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@jon SELECT routine_name, routine_definition IS NULL FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@dan SELECT routine_name, routine_definition IS NULL FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@fran SELECT routine_name, routine_definition IS NULL FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@bob SELECT routine_name, routine_definition IS NULL FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name
@alice SELECT trigger_name FROM information_schema.triggers WHERE trigger_schema = 'v'
@carol SELECT trigger_name FROM information_schema.triggers WHERE trigger_schema = 'v'
@dan SELECT trigger_name FROM information_schema.triggers WHERE trigger_schema = 'v'
@erin SELECT trigger_name FROM information_schema.triggers WHERE trigger_schema = 'v'
@fran SELECT trigger_name FROM information_schema.triggers WHERE trigger_schema = 'v'
@bob SELECT COUNT(*) FROM information_schema.parameters WHERE specific_schema = 'v'
@carol SELECT COUNT(*) FROM information_schema.parameters WHERE specific_schema = 'v'
@alice SHOW TRIGGERS FROM v
@fran SHOW TRIGGERS FROM v
@bob SHOW PROCEDURE STATUS WHERE Name = 'nope'
@carol SHOW FUNCTION STATUS WHERE Name = 'nope'
