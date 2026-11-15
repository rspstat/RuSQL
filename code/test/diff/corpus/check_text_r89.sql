# R89 (2026-11-16): a CHECK condition the way MySQL writes it in information_schema.check_constraints.CHECK_CLAUSE (and SHOW CREATE TABLE): columns back-quoted in the case they were typed in,
# every comparison and every AND / OR in brackets (a chain of the same AND / OR in one pair), lower case words, strings as _utf8mb4'..', a negative number as -(5), a NOT pushed into the
# comparison, function names as MySQL names them, CASE as (case when .. then .. end). Measured on MySQL 8.4.9. A line is a statement, '--' a setup statement run on both.
# (not here: what the parser does not keep -- -qty is 0 - qty, 1e3 is 1000, TRUE is 1, a double-quoted string, IN (a column), BETWEEN a column AND 5; those are shown as typed)
# Run: python vs_mysql.py <engine_server.exe> corpus/check_text_r89.sql --proto --errno
--DROP TABLE IF EXISTS ck0
CREATE TABLE ck0 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c0 CHECK (qty >= 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c0'
--DROP TABLE IF EXISTS ck1
CREATE TABLE ck1 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c1 CHECK (qty > 0 AND qty < 100))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c1'
--DROP TABLE IF EXISTS ck2
CREATE TABLE ck2 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c2 CHECK (qty > 0 OR qty IS NULL))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c2'
--DROP TABLE IF EXISTS ck3
CREATE TABLE ck3 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c3 CHECK (NOT (qty = 5)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c3'
--DROP TABLE IF EXISTS ck4
CREATE TABLE ck4 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c4 CHECK (qty BETWEEN 1 AND 10))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c4'
--DROP TABLE IF EXISTS ck5
CREATE TABLE ck5 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c5 CHECK (qty NOT BETWEEN 1 AND 10))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c5'
--DROP TABLE IF EXISTS ck6
CREATE TABLE ck6 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c6 CHECK (qty IN (1, 2, 3)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c6'
--DROP TABLE IF EXISTS ck7
CREATE TABLE ck7 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c7 CHECK (qty NOT IN (1, 2)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c7'
--DROP TABLE IF EXISTS ck8
CREATE TABLE ck8 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c8 CHECK (name LIKE 'a%'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c8'
--DROP TABLE IF EXISTS ck9
CREATE TABLE ck9 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c9 CHECK (name NOT LIKE 'a%'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c9'
--DROP TABLE IF EXISTS ck10
CREATE TABLE ck10 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c10 CHECK (name <> ''))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c10'
--DROP TABLE IF EXISTS ck11
CREATE TABLE ck11 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c11 CHECK (name != 'x'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c11'
--DROP TABLE IF EXISTS ck12
CREATE TABLE ck12 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c12 CHECK (qty IS NOT NULL))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c12'
--DROP TABLE IF EXISTS ck13
CREATE TABLE ck13 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c13 CHECK (qty IS NULL OR qty > 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c13'
--DROP TABLE IF EXISTS ck14
CREATE TABLE ck14 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c14 CHECK (qty + price > 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c14'
--DROP TABLE IF EXISTS ck15
CREATE TABLE ck15 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c15 CHECK (qty * 2 = price))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c15'
--DROP TABLE IF EXISTS ck16
CREATE TABLE ck16 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c16 CHECK (qty - 1 >= 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c16'
--DROP TABLE IF EXISTS ck17
CREATE TABLE ck17 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c17 CHECK (qty / 2 > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c17'
--DROP TABLE IF EXISTS ck18
CREATE TABLE ck18 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c18 CHECK ((qty > 0)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c18'
--DROP TABLE IF EXISTS ck19
CREATE TABLE ck19 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c19 CHECK (((qty > 0) AND (price > 0))))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c19'
--DROP TABLE IF EXISTS ck20
CREATE TABLE ck20 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c20 CHECK (qty > 0 AND (price > 0 OR qty < 5)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c20'
--DROP TABLE IF EXISTS ck21
CREATE TABLE ck21 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c21 CHECK (UPPER(name) = 'X'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c21'
--DROP TABLE IF EXISTS ck22
CREATE TABLE ck22 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c22 CHECK (LENGTH(name) > 2))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c22'
--DROP TABLE IF EXISTS ck23
CREATE TABLE ck23 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c23 CHECK (CHAR_LENGTH(name) BETWEEN 2 AND 9))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c23'
--DROP TABLE IF EXISTS ck24
CREATE TABLE ck24 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c24 CHECK (COALESCE(qty, 0) >= 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c24'
--DROP TABLE IF EXISTS ck25
CREATE TABLE ck25 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c25 CHECK (qty % 2 = 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c25'
--DROP TABLE IF EXISTS ck26
CREATE TABLE ck26 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c26 CHECK (qty MOD 2 = 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c26'
--DROP TABLE IF EXISTS ck27
CREATE TABLE ck27 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c27 CHECK (qty > -1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c27'
--DROP TABLE IF EXISTS ck28
CREATE TABLE ck28 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c28 CHECK (price >= 0.5))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c28'
--DROP TABLE IF EXISTS ck29
CREATE TABLE ck29 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c29 CHECK (CASE WHEN qty > 0 THEN 1 ELSE 0 END = 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c29'
--DROP TABLE IF EXISTS ck30
CREATE TABLE ck30 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c30 CHECK (CASE WHEN qty > 0 THEN 'p' WHEN qty < 0 THEN 'n' END = 'p'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c30'
--DROP TABLE IF EXISTS ck31
CREATE TABLE ck31 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c31 CHECK (ABS(qty) < 10 AND NOT name IS NULL))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c31'
--DROP TABLE IF EXISTS ck32
CREATE TABLE ck32 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c32 CHECK (NOT (qty > 3)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c32'
--DROP TABLE IF EXISTS ck33
CREATE TABLE ck33 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c33 CHECK (NOT (qty >= 3)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c33'
--DROP TABLE IF EXISTS ck34
CREATE TABLE ck34 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c34 CHECK (NOT (qty < 3)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c34'
--DROP TABLE IF EXISTS ck35
CREATE TABLE ck35 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c35 CHECK (NOT (qty <= 3)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c35'
--DROP TABLE IF EXISTS ck36
CREATE TABLE ck36 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c36 CHECK (NOT (qty IS NULL)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c36'
--DROP TABLE IF EXISTS ck37
CREATE TABLE ck37 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c37 CHECK (NOT (qty IN (1, 2))))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c37'
--DROP TABLE IF EXISTS ck38
CREATE TABLE ck38 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c38 CHECK (NOT (qty BETWEEN 1 AND 2)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c38'
--DROP TABLE IF EXISTS ck39
CREATE TABLE ck39 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c39 CHECK (NOT (qty > 0 AND price > 0)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c39'
--DROP TABLE IF EXISTS ck40
CREATE TABLE ck40 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c40 CHECK (NOT (qty > 0 OR price > 0)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c40'
--DROP TABLE IF EXISTS ck41
CREATE TABLE ck41 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c41 CHECK (NOT (name LIKE 'a')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c41'
--DROP TABLE IF EXISTS ck42
CREATE TABLE ck42 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c42 CHECK (LOWER(name) = 'x'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c42'
--DROP TABLE IF EXISTS ck43
CREATE TABLE ck43 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c43 CHECK (LCASE(name) = 'x'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c43'
--DROP TABLE IF EXISTS ck44
CREATE TABLE ck44 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c44 CHECK (CHARACTER_LENGTH(name) > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c44'
--DROP TABLE IF EXISTS ck45
CREATE TABLE ck45 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c45 CHECK (ABS(qty) > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c45'
--DROP TABLE IF EXISTS ck46
CREATE TABLE ck46 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c46 CHECK (IFNULL(qty, 0) > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c46'
--DROP TABLE IF EXISTS ck47
CREATE TABLE ck47 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c47 CHECK (CONCAT(name, 'a') = 'xa'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c47'
--DROP TABLE IF EXISTS ck48
CREATE TABLE ck48 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c48 CHECK (ROUND(price) > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c48'
--DROP TABLE IF EXISTS ck49
CREATE TABLE ck49 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c49 CHECK (ROUND(price, 1) > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c49'
--DROP TABLE IF EXISTS ck50
CREATE TABLE ck50 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c50 CHECK (CEIL(price) > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c50'
--DROP TABLE IF EXISTS ck51
CREATE TABLE ck51 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c51 CHECK (CEILING(price) > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c51'
--DROP TABLE IF EXISTS ck52
CREATE TABLE ck52 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c52 CHECK (FLOOR(price) > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c52'
--DROP TABLE IF EXISTS ck53
CREATE TABLE ck53 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c53 CHECK (SUBSTRING(name, 1, 2) = 'ab'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c53'
--DROP TABLE IF EXISTS ck54
CREATE TABLE ck54 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c54 CHECK (SUBSTR(name, 1) = 'ab'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c54'
--DROP TABLE IF EXISTS ck55
CREATE TABLE ck55 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c55 CHECK (LEFT(name, 2) = 'ab'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c55'
--DROP TABLE IF EXISTS ck56
CREATE TABLE ck56 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c56 CHECK (TRIM(name) = 'ab'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c56'
--DROP TABLE IF EXISTS ck57
CREATE TABLE ck57 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c57 CHECK (YEAR(created) > 2000))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c57'
--DROP TABLE IF EXISTS ck58
CREATE TABLE ck58 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c58 CHECK (MONTH(created) = 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c58'
--DROP TABLE IF EXISTS ck59
CREATE TABLE ck59 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c59 CHECK (NULLIF(qty, 0) > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c59'
--DROP TABLE IF EXISTS ck60
CREATE TABLE ck60 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c60 CHECK (GREATEST(qty, 1) > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c60'
--DROP TABLE IF EXISTS ck61
CREATE TABLE ck61 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c61 CHECK (LEAST(qty, 1) > 1))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c61'
--DROP TABLE IF EXISTS ck62
CREATE TABLE ck62 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c62 CHECK (MOD(qty, 2) = 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c62'
--DROP TABLE IF EXISTS ck63
CREATE TABLE ck63 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c63 CHECK (qty = (price)))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c63'
--DROP TABLE IF EXISTS ck64
CREATE TABLE ck64 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c64 CHECK ((qty) > ((price))))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c64'
--DROP TABLE IF EXISTS ck65
CREATE TABLE ck65 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c65 CHECK (name = 'it''s'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c65'
--DROP TABLE IF EXISTS ck66
CREATE TABLE ck66 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c66 CHECK (name = 'a\\b'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c66'
--DROP TABLE IF EXISTS ck67
CREATE TABLE ck67 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c67 CHECK (name = ''))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c67'
--DROP TABLE IF EXISTS ck68
CREATE TABLE ck68 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c68 CHECK (qty IS NOT NULL AND price IS NULL))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c68'
--DROP TABLE IF EXISTS ck69
CREATE TABLE ck69 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c69 CHECK (qty >= 1 AND qty <= 9 AND price > 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c69'
--DROP TABLE IF EXISTS ck70
CREATE TABLE ck70 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c70 CHECK (qty > 0 OR qty < -5 OR price = 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c70'
--DROP TABLE IF EXISTS ck71
CREATE TABLE ck71 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c71 CHECK (qty + 1 + 2 > 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c71'
--DROP TABLE IF EXISTS ck72
CREATE TABLE ck72 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c72 CHECK (qty * (price + 1) > 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c72'
--DROP TABLE IF EXISTS ck73
CREATE TABLE ck73 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c73 CHECK (qty - (price - 1) > 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c73'
--DROP TABLE IF EXISTS ck74
CREATE TABLE ck74 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c74 CHECK (qty / price > 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c74'
--DROP TABLE IF EXISTS ck75
CREATE TABLE ck75 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c75 CHECK ((qty + price) * 2 > 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c75'
--DROP TABLE IF EXISTS ck76
CREATE TABLE ck76 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c76 CHECK (price > 1.50))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c76'
--DROP TABLE IF EXISTS ck77
CREATE TABLE ck77 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c77 CHECK (price > 0.10))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c77'
--DROP TABLE IF EXISTS ck78
CREATE TABLE ck78 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c78 CHECK (qty > 007))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c78'
--DROP TABLE IF EXISTS ck79
CREATE TABLE ck79 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c79 CHECK (qty = 1.0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c79'
--DROP TABLE IF EXISTS ck80
CREATE TABLE ck80 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c80 CHECK (qty = -1.5))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c80'
--DROP TABLE IF EXISTS ck81
CREATE TABLE ck81 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c81 CHECK (name IN ('a', 'b')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c81'
--DROP TABLE IF EXISTS ck82
CREATE TABLE ck82 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c82 CHECK (name LIKE '%x_'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c82'
--DROP TABLE IF EXISTS ck83
CREATE TABLE ck83 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c83 CHECK (name LIKE price))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c83'
--DROP TABLE IF EXISTS ck84
CREATE TABLE ck84 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c84 CHECK (name = CONCAT('a','b')))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c84'
--DROP TABLE IF EXISTS ck85
CREATE TABLE ck85 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c85 CHECK (CHAR_LENGTH(name) > 2 AND LENGTH(name) < 9))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c85'
--DROP TABLE IF EXISTS ck86
CREATE TABLE ck86 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c86 CHECK (QTY > 0))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c86'
--DROP TABLE IF EXISTS ck87
CREATE TABLE ck87 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c87 CHECK (Qty > 0 AND PRICE < 5))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c87'
--DROP TABLE IF EXISTS ck88
CREATE TABLE ck88 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c88 CHECK (created > '2020-01-01'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c88'
--DROP TABLE IF EXISTS ck89
CREATE TABLE ck89 (qty INT, price DECIMAL(10,2), name VARCHAR(20), created DATE, CONSTRAINT c89 CHECK (name REGEXP '^a'))
SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = DATABASE() AND constraint_name = 'c89'
--DROP TABLE IF EXISTS ck0
--DROP TABLE IF EXISTS ck1
--DROP TABLE IF EXISTS ck2
--DROP TABLE IF EXISTS ck3
--DROP TABLE IF EXISTS ck4
--DROP TABLE IF EXISTS ck5
--DROP TABLE IF EXISTS ck6
--DROP TABLE IF EXISTS ck7
--DROP TABLE IF EXISTS ck8
--DROP TABLE IF EXISTS ck9
--DROP TABLE IF EXISTS ck10
--DROP TABLE IF EXISTS ck11
--DROP TABLE IF EXISTS ck12
--DROP TABLE IF EXISTS ck13
--DROP TABLE IF EXISTS ck14
--DROP TABLE IF EXISTS ck15
--DROP TABLE IF EXISTS ck16
--DROP TABLE IF EXISTS ck17
--DROP TABLE IF EXISTS ck18
--DROP TABLE IF EXISTS ck19
--DROP TABLE IF EXISTS ck20
--DROP TABLE IF EXISTS ck21
--DROP TABLE IF EXISTS ck22
--DROP TABLE IF EXISTS ck23
--DROP TABLE IF EXISTS ck24
--DROP TABLE IF EXISTS ck25
--DROP TABLE IF EXISTS ck26
--DROP TABLE IF EXISTS ck27
--DROP TABLE IF EXISTS ck28
--DROP TABLE IF EXISTS ck29
--DROP TABLE IF EXISTS ck30
--DROP TABLE IF EXISTS ck31
--DROP TABLE IF EXISTS ck32
--DROP TABLE IF EXISTS ck33
--DROP TABLE IF EXISTS ck34
--DROP TABLE IF EXISTS ck35
--DROP TABLE IF EXISTS ck36
--DROP TABLE IF EXISTS ck37
--DROP TABLE IF EXISTS ck38
--DROP TABLE IF EXISTS ck39
--DROP TABLE IF EXISTS ck40
--DROP TABLE IF EXISTS ck41
--DROP TABLE IF EXISTS ck42
--DROP TABLE IF EXISTS ck43
--DROP TABLE IF EXISTS ck44
--DROP TABLE IF EXISTS ck45
--DROP TABLE IF EXISTS ck46
--DROP TABLE IF EXISTS ck47
--DROP TABLE IF EXISTS ck48
--DROP TABLE IF EXISTS ck49
--DROP TABLE IF EXISTS ck50
--DROP TABLE IF EXISTS ck51
--DROP TABLE IF EXISTS ck52
--DROP TABLE IF EXISTS ck53
--DROP TABLE IF EXISTS ck54
--DROP TABLE IF EXISTS ck55
--DROP TABLE IF EXISTS ck56
--DROP TABLE IF EXISTS ck57
--DROP TABLE IF EXISTS ck58
--DROP TABLE IF EXISTS ck59
--DROP TABLE IF EXISTS ck60
--DROP TABLE IF EXISTS ck61
--DROP TABLE IF EXISTS ck62
--DROP TABLE IF EXISTS ck63
--DROP TABLE IF EXISTS ck64
--DROP TABLE IF EXISTS ck65
--DROP TABLE IF EXISTS ck66
--DROP TABLE IF EXISTS ck67
--DROP TABLE IF EXISTS ck68
--DROP TABLE IF EXISTS ck69
--DROP TABLE IF EXISTS ck70
--DROP TABLE IF EXISTS ck71
--DROP TABLE IF EXISTS ck72
--DROP TABLE IF EXISTS ck73
--DROP TABLE IF EXISTS ck74
--DROP TABLE IF EXISTS ck75
--DROP TABLE IF EXISTS ck76
--DROP TABLE IF EXISTS ck77
--DROP TABLE IF EXISTS ck78
--DROP TABLE IF EXISTS ck79
--DROP TABLE IF EXISTS ck80
--DROP TABLE IF EXISTS ck81
--DROP TABLE IF EXISTS ck82
--DROP TABLE IF EXISTS ck83
--DROP TABLE IF EXISTS ck84
--DROP TABLE IF EXISTS ck85
--DROP TABLE IF EXISTS ck86
--DROP TABLE IF EXISTS ck87
--DROP TABLE IF EXISTS ck88
--DROP TABLE IF EXISTS ck89
