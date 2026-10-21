# UPDATE that shifts a UNIQUE or PRIMARY KEY value (R44): InnoDB writes the rows one after the other and checks each as it goes, in the order of the primary key
# (or the one ORDER BY gives), so `SET u = u + 1` over 1, 2, 3 fails on the first row and over 3, 2, 1 does not. Each statement has a table of its own.
# A line is a statement, '--' a setup statement.
# shift up, no order
--CREATE TABLE sh_1 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_1 VALUES (1,1),(2,2),(3,3)
UPDATE sh_1 SET u = u + 1
SELECT * FROM sh_1 ORDER BY 1, 2
# shift down
--CREATE TABLE sh_2 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_2 VALUES (1,1),(2,2),(3,3)
UPDATE sh_2 SET u = u - 1
SELECT * FROM sh_2 ORDER BY 1, 2
# shift up, ORDER BY id DESC
--CREATE TABLE sh_3 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_3 VALUES (1,1),(2,2),(3,3)
UPDATE sh_3 SET u = u + 1 ORDER BY id DESC
SELECT * FROM sh_3 ORDER BY 1, 2
# shift up, ORDER BY u ASC
--CREATE TABLE sh_4 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_4 VALUES (1,1),(2,2),(3,3)
UPDATE sh_4 SET u = u + 1 ORDER BY u ASC
SELECT * FROM sh_4 ORDER BY 1, 2
# shift up, ORDER BY u DESC
--CREATE TABLE sh_5 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_5 VALUES (1,1),(2,2),(3,3)
UPDATE sh_5 SET u = u + 1 ORDER BY u DESC
SELECT * FROM sh_5 ORDER BY 1, 2
# a swap
--CREATE TABLE sh_6 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_6 VALUES (1,1),(2,2),(3,3)
UPDATE sh_6 SET u = 3 - u
SELECT * FROM sh_6 ORDER BY 1, 2
# shift up, WHERE on the primary key
--CREATE TABLE sh_7 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_7 VALUES (1,1),(2,2),(3,3)
UPDATE sh_7 SET u = u + 1 WHERE id >= 2
SELECT * FROM sh_7 ORDER BY 1, 2
# shift up, WHERE on the unique column
--CREATE TABLE sh_8 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_8 VALUES (1,1),(2,2),(3,3)
UPDATE sh_8 SET u = u + 1 WHERE u >= 2
SELECT * FROM sh_8 ORDER BY 1, 2
# shift up, WHERE on the unique column, ORDER BY id DESC
--CREATE TABLE sh_9 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_9 VALUES (1,1),(2,2),(3,3)
UPDATE sh_9 SET u = u + 1 WHERE u >= 2 ORDER BY id DESC
SELECT * FROM sh_9 ORDER BY 1, 2
# one row onto another
--CREATE TABLE sh_10 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_10 VALUES (1,1),(2,2),(3,3)
UPDATE sh_10 SET u = u + 1 WHERE id = 1
SELECT * FROM sh_10 ORDER BY 1, 2
# every row to one value
--CREATE TABLE sh_11 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_11 VALUES (1,1),(2,2),(3,3)
UPDATE sh_11 SET u = 10
SELECT * FROM sh_11 ORDER BY 1, 2
# shift up, LIMIT 1
--CREATE TABLE sh_12 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_12 VALUES (1,1),(2,2),(3,3)
UPDATE sh_12 SET u = u + 1 LIMIT 1
SELECT * FROM sh_12 ORDER BY 1, 2
# shift up, LIMIT 2
--CREATE TABLE sh_13 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_13 VALUES (1,1),(2,2),(3,3)
UPDATE sh_13 SET u = u + 1 LIMIT 2
SELECT * FROM sh_13 ORDER BY 1, 2
# shift up, ORDER BY id DESC LIMIT 2
--CREATE TABLE sh_14 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_14 VALUES (1,1),(2,2),(3,3)
UPDATE sh_14 SET u = u + 1 ORDER BY id DESC LIMIT 2
SELECT * FROM sh_14 ORDER BY 1, 2
# shift up with a gap above
--CREATE TABLE sh_15 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_15 VALUES (1,1),(2,2),(3,3)
UPDATE sh_15 SET u = u + 10 WHERE id <= 2
SELECT * FROM sh_15 ORDER BY 1, 2
# shift up two, ORDER BY id DESC
--CREATE TABLE sh_16 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_16 VALUES (1,1),(2,2),(3,3)
UPDATE sh_16 SET u = u + 2 ORDER BY id DESC
SELECT * FROM sh_16 ORDER BY 1, 2
# shift up two, no order
--CREATE TABLE sh_17 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_17 VALUES (1,1),(2,2),(3,3)
UPDATE sh_17 SET u = u + 2
SELECT * FROM sh_17 ORDER BY 1, 2
# set NULL then shift
--CREATE TABLE sh_18 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_18 VALUES (1,1),(2,2),(3,3)
UPDATE sh_18 SET u = NULL WHERE id = 1
SELECT * FROM sh_18 ORDER BY 1, 2
# move to the freed value of the next row
--CREATE TABLE sh_19 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_19 VALUES (1,1),(2,2),(3,3)
UPDATE sh_19 SET u = u + 1, id = id WHERE id < 3
SELECT * FROM sh_19 ORDER BY 1, 2
# primary key shift up
--CREATE TABLE sh_20 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_20 VALUES (1,1),(2,2),(3,3)
UPDATE sh_20 SET id = id + 1
SELECT * FROM sh_20 ORDER BY 1, 2
# primary key shift up, ORDER BY id DESC
--CREATE TABLE sh_21 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_21 VALUES (1,1),(2,2),(3,3)
UPDATE sh_21 SET id = id + 1 ORDER BY id DESC
SELECT * FROM sh_21 ORDER BY 1, 2
# primary key shift down
--CREATE TABLE sh_22 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_22 VALUES (1,1),(2,2),(3,3)
UPDATE sh_22 SET id = id - 1
SELECT * FROM sh_22 ORDER BY 1, 2
# primary key mirror
--CREATE TABLE sh_23 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_23 VALUES (1,1),(2,2),(3,3)
UPDATE sh_23 SET id = 4 - id
SELECT * FROM sh_23 ORDER BY 1, 2
# inserted 3, 1, 2: shift up
--CREATE TABLE sh_24 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_24 VALUES (3,3),(1,1),(2,2)
UPDATE sh_24 SET u = u + 1
SELECT * FROM sh_24 ORDER BY 1, 2
# inserted 3, 1, 2: shift down
--CREATE TABLE sh_25 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_25 VALUES (3,3),(1,1),(2,2)
UPDATE sh_25 SET u = u - 1
SELECT * FROM sh_25 ORDER BY 1, 2
# after an update, shift down
--CREATE TABLE sh_26 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_26 VALUES (1,1),(2,2),(3,3)
UPDATE sh_26 SET u = u + 10 WHERE id = 1
UPDATE sh_26 SET u = u - 1
SELECT * FROM sh_26 ORDER BY 1, 2
# after an update, shift up
--CREATE TABLE sh_27 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_27 VALUES (1,1),(2,2),(3,3)
UPDATE sh_27 SET u = u + 10 WHERE id = 1
UPDATE sh_27 SET u = u + 1
SELECT * FROM sh_27 ORDER BY 1, 2
# after a no-op update, shift down
--CREATE TABLE sh_28 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_28 VALUES (1,1),(2,2)
UPDATE sh_28 SET u = u WHERE id = 1
UPDATE sh_28 SET u = u - 1
SELECT * FROM sh_28 ORDER BY 1, 2
# after a no-op update, shift up
--CREATE TABLE sh_29 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_29 VALUES (1,1),(2,2)
UPDATE sh_29 SET u = u WHERE id = 1
UPDATE sh_29 SET u = u + 1
SELECT * FROM sh_29 ORDER BY 1, 2
# through a view: shift up
--CREATE TABLE sh_30 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_30 VALUES (1,1),(2,2),(3,3)
--CREATE VIEW sh_30_v AS SELECT id, u FROM sh_30
UPDATE sh_30_v SET u = u + 1
SELECT * FROM sh_30 ORDER BY 1, 2
# through a view: shift up, WHERE
--CREATE TABLE sh_31 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_31 VALUES (1,1),(2,2),(3,3)
--CREATE VIEW sh_31_v AS SELECT id, u FROM sh_31
UPDATE sh_31_v SET u = u + 1 WHERE id >= 2
SELECT * FROM sh_31 ORDER BY 1, 2
# join: shift up
--CREATE TABLE sh_32 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_32 VALUES (1,1),(2,2),(3,3)
--CREATE TABLE sh_32_s (id INT PRIMARY KEY, nu INT)
--INSERT INTO sh_32_s VALUES (1, 2), (2, 3), (3, 4)
UPDATE sh_32 JOIN sh_32_s ON sh_32.id = sh_32_s.id SET sh_32.u = sh_32_s.nu
SELECT * FROM sh_32 ORDER BY 1, 2
# join: shift down
--CREATE TABLE sh_33 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_33 VALUES (1,1),(2,2),(3,3)
--CREATE TABLE sh_33_s (id INT PRIMARY KEY, nu INT)
--INSERT INTO sh_33_s VALUES (1, 0), (2, 1), (3, 2)
UPDATE sh_33 JOIN sh_33_s ON sh_33.id = sh_33_s.id SET sh_33.u = sh_33_s.nu
SELECT * FROM sh_33 ORDER BY 1, 2
# comma join: shift up
--CREATE TABLE sh_34 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_34 VALUES (1,1),(2,2),(3,3)
--CREATE TABLE sh_34_s (id INT PRIMARY KEY, nu INT)
--INSERT INTO sh_34_s VALUES (1, 2), (2, 3), (3, 4)
UPDATE sh_34, sh_34_s SET sh_34.u = sh_34_s.nu WHERE sh_34.id = sh_34_s.id
SELECT * FROM sh_34 ORDER BY 1, 2
# join: a swap
--CREATE TABLE sh_35 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_35 VALUES (1,1),(2,2),(3,3)
--CREATE TABLE sh_35_s (id INT PRIMARY KEY, nu INT)
--INSERT INTO sh_35_s VALUES (1, 3), (2, 2), (3, 1)
UPDATE sh_35 JOIN sh_35_s ON sh_35.id = sh_35_s.id SET sh_35.u = sh_35_s.nu
SELECT * FROM sh_35 ORDER BY 1, 2
# no primary key: shift up
--CREATE TABLE sh_36 (id INT, u INT UNIQUE)
--INSERT INTO sh_36 VALUES (1,1),(2,2),(3,3)
UPDATE sh_36 SET u = u + 1
SELECT * FROM sh_36 ORDER BY 1, 2
# no primary key: shift down
--CREATE TABLE sh_37 (id INT, u INT UNIQUE)
--INSERT INTO sh_37 VALUES (1,1),(2,2),(3,3)
UPDATE sh_37 SET u = u - 1
SELECT * FROM sh_37 ORDER BY 1, 2
# composite unique: shift up
--CREATE TABLE sh_38 (id INT PRIMARY KEY, a INT, b INT, UNIQUE KEY ab (a, b))
--INSERT INTO sh_38 VALUES (1,1,1),(2,1,2),(3,1,3)
UPDATE sh_38 SET b = b + 1
SELECT * FROM sh_38 ORDER BY 1, 2
# composite unique: shift down
--CREATE TABLE sh_39 (id INT PRIMARY KEY, a INT, b INT, UNIQUE KEY ab (a, b))
--INSERT INTO sh_39 VALUES (1,1,1),(2,1,2),(3,1,3)
UPDATE sh_39 SET b = b - 1
SELECT * FROM sh_39 ORDER BY 1, 2
# composite unique: shift up, ORDER BY id DESC
--CREATE TABLE sh_40 (id INT PRIMARY KEY, a INT, b INT, UNIQUE KEY ab (a, b))
--INSERT INTO sh_40 VALUES (1,1,1),(2,1,2),(3,1,3)
UPDATE sh_40 SET b = b + 1 ORDER BY id DESC
SELECT * FROM sh_40 ORDER BY 1, 2
# composite primary key: shift up
--CREATE TABLE sh_41 (a INT, b INT, v INT, PRIMARY KEY (a, b))
--INSERT INTO sh_41 VALUES (1,1,0),(1,2,0),(1,3,0)
UPDATE sh_41 SET b = b + 1
SELECT * FROM sh_41 ORDER BY 1, 2
# composite primary key: shift down
--CREATE TABLE sh_42 (a INT, b INT, v INT, PRIMARY KEY (a, b))
--INSERT INTO sh_42 VALUES (1,1,0),(1,2,0),(1,3,0)
UPDATE sh_42 SET b = b - 1
SELECT * FROM sh_42 ORDER BY 1, 2
# composite primary key: shift up, ordered descending
--CREATE TABLE sh_43 (a INT, b INT, v INT, PRIMARY KEY (a, b))
--INSERT INTO sh_43 VALUES (1,1,0),(1,2,0),(1,3,0)
UPDATE sh_43 SET b = b + 1 ORDER BY a DESC, b DESC
SELECT * FROM sh_43 ORDER BY 1, 2
# values in reverse key order: shift down through the index
--CREATE TABLE sh_44 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_44 VALUES (1,30),(2,20),(3,10)
UPDATE sh_44 SET u = u - 10 WHERE u >= 10
SELECT * FROM sh_44 ORDER BY 1, 2
# values in reverse key order: shift down by the primary key
--CREATE TABLE sh_45 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_45 VALUES (1,30),(2,20),(3,10)
UPDATE sh_45 SET u = u - 10
SELECT * FROM sh_45 ORDER BY 1, 2
# values in reverse key order: shift up by the primary key
--CREATE TABLE sh_46 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_46 VALUES (1,30),(2,20),(3,10)
UPDATE sh_46 SET u = u + 10
SELECT * FROM sh_46 ORDER BY 1, 2
# values in reverse key order: shift up, ORDER BY u
--CREATE TABLE sh_47 (id INT PRIMARY KEY, u INT UNIQUE)
--INSERT INTO sh_47 VALUES (1,30),(2,20),(3,10)
UPDATE sh_47 SET u = u + 10 ORDER BY u
SELECT * FROM sh_47 ORDER BY 1, 2
# text: shift up
--CREATE TABLE sh_48 (id INT PRIMARY KEY, s VARCHAR(5) UNIQUE)
--INSERT INTO sh_48 VALUES (1,'a'),(2,'b'),(3,'c')
UPDATE sh_48 SET s = CHAR(ASCII(s) + 1)
SELECT * FROM sh_48 ORDER BY 1, 2
# text: shift down
--CREATE TABLE sh_49 (id INT PRIMARY KEY, s VARCHAR(5) UNIQUE)
--INSERT INTO sh_49 VALUES (1,'a'),(2,'b'),(3,'c')
UPDATE sh_49 SET s = CHAR(ASCII(s) - 1)
SELECT * FROM sh_49 ORDER BY 1, 2
# text: the same value in another case
--CREATE TABLE sh_50 (id INT PRIMARY KEY, s VARCHAR(5) UNIQUE)
--INSERT INTO sh_50 VALUES (1,'a'),(2,'b')
UPDATE sh_50 SET s = UPPER(s)
SELECT * FROM sh_50 ORDER BY 1, 2
# text: a case-insensitive clash with another row
--CREATE TABLE sh_51 (id INT PRIMARY KEY, s VARCHAR(5) UNIQUE)
--INSERT INTO sh_51 VALUES (1,'a'),(2,'A2')
UPDATE sh_51 SET s = 'A' WHERE id = 2
SELECT * FROM sh_51 ORDER BY 1, 2
