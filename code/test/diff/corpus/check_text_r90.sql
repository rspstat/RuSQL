# R90 (2026-11-16): NOT REGEXP / NOT RLIKE / REGEXP_LIKE() in a CHECK condition as MySQL writes it: `(not(regexp_like(`name`,_utf8mb4'x')))`, `regexp_like(`name`,_utf8mb4'x')` (R89 left them as typed).
# Measured on MySQL 8.4.9. (not here: a comparison with a REGEXP as an operand -- `(a NOT REGEXP 'x') = 1` -- shown as typed.) A line is a statement, '--' a setup statement run on both.
# Run: python vs_mysql.py <engine_server.exe> corpus/check_text_r90.sql --proto --errno
--DROP TABLE IF EXISTS cn0
CREATE TABLE cn0 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k0 CHECK (name NOT REGEXP '^a'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k0'
--DROP TABLE IF EXISTS cn1
CREATE TABLE cn1 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k1 CHECK (name REGEXP '^a'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k1'
--DROP TABLE IF EXISTS cn2
CREATE TABLE cn2 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k2 CHECK (NOT (name REGEXP '^a')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k2'
--DROP TABLE IF EXISTS cn3
CREATE TABLE cn3 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k3 CHECK (name NOT RLIKE 'x'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k3'
--DROP TABLE IF EXISTS cn4
CREATE TABLE cn4 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k4 CHECK (name RLIKE 'x'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k4'
--DROP TABLE IF EXISTS cn5
CREATE TABLE cn5 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k5 CHECK (NOT name REGEXP 'x'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k5'
--DROP TABLE IF EXISTS cn6
CREATE TABLE cn6 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k6 CHECK (NOT (name NOT REGEXP 'x')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k6'
--DROP TABLE IF EXISTS cn7
CREATE TABLE cn7 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k7 CHECK (NOT (NOT (name REGEXP 'x'))))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k7'
--DROP TABLE IF EXISTS cn8
CREATE TABLE cn8 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k8 CHECK (name REGEXP 'a' AND qty > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k8'
--DROP TABLE IF EXISTS cn9
CREATE TABLE cn9 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k9 CHECK (name NOT REGEXP 'a' OR qty > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k9'
--DROP TABLE IF EXISTS cn10
CREATE TABLE cn10 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k10 CHECK (NOT (name NOT REGEXP 'a' OR qty > 1)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k10'
--DROP TABLE IF EXISTS cn11
CREATE TABLE cn11 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k11 CHECK (NOT (name REGEXP 'a' AND qty > 1)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k11'
--DROP TABLE IF EXISTS cn12
CREATE TABLE cn12 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k12 CHECK (name NOT REGEXP '^[a-z]+$' AND LENGTH(name) > 2))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k12'
--DROP TABLE IF EXISTS cn13
CREATE TABLE cn13 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k13 CHECK (name NOT REGEXP 'it''s'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k13'
--DROP TABLE IF EXISTS cn14
CREATE TABLE cn14 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k14 CHECK (name NOT REGEXP '\d'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k14'
--DROP TABLE IF EXISTS cn15
CREATE TABLE cn15 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k15 CHECK (REGEXP_LIKE(name, 'a')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k15'
--DROP TABLE IF EXISTS cn16
CREATE TABLE cn16 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k16 CHECK (NOT REGEXP_LIKE(name, 'a')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k16'
--DROP TABLE IF EXISTS cn17
CREATE TABLE cn17 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k17 CHECK (name NOT LIKE 'a%'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k17'
--DROP TABLE IF EXISTS cn18
CREATE TABLE cn18 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k18 CHECK (REGEXP_LIKE(name, 'a', 'i')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k18'
--DROP TABLE IF EXISTS cn19
CREATE TABLE cn19 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k19 CHECK (REGEXP_LIKE(name, '^a') AND qty > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k19'
--DROP TABLE IF EXISTS cn20
CREATE TABLE cn20 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k20 CHECK (NOT REGEXP_LIKE(name, 'a') OR qty > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k20'
--DROP TABLE IF EXISTS cn21
CREATE TABLE cn21 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k21 CHECK (name REGEXP 'a' AND REGEXP_LIKE(name, 'b')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k21'
--DROP TABLE IF EXISTS cn22
CREATE TABLE cn22 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k22 CHECK (REGEXP_LIKE(name, '^[A-Z]{2}[0-9]+$')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k22'
--DROP TABLE IF EXISTS cn23
CREATE TABLE cn23 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k23 CHECK (qty > 0 AND NOT REGEXP_LIKE(name, 'x')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k23'
--DROP TABLE IF EXISTS cn24
CREATE TABLE cn24 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k24 CHECK (NOT (REGEXP_LIKE(name, 'a') AND qty > 1)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k24'
--DROP TABLE IF EXISTS cn25
CREATE TABLE cn25 (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT k25 CHECK (REGEXP_LIKE(LOWER(name), 'a')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'k25'
