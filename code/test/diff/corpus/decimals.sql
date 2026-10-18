# how DECIMAL and integer results are written: the places a column has, the places an operation gives (a sum 2, a product the sum of the two, a quotient 4 more),
# the places SUM / AVG / ROUND / TRUNCATE / MOD keep (run with --exact: the texts must be the same, not only the numbers; no DOUBLE here, a client sees those as floats)
--CREATE TABLE dc (id INT PRIMARY KEY, price DECIMAL(10,2), qty INT, rate DECIMAL(5,3), amt DECIMAL(12,4))
--INSERT INTO dc VALUES (1, 10.50, 3, 1.250, 100.0000), (2, 20.25, 2, 0.500, 0.5000), (3, 5.00, 4, 2.000, 7.1234), (4, NULL, NULL, NULL, NULL), (5, 0.10, 1, 0.001, 0.0001)
SELECT id, price, qty, rate, amt FROM dc ORDER BY id
SELECT SUM(price), AVG(price), MIN(price), MAX(price), COUNT(price) FROM dc
SELECT SUM(qty), AVG(qty), SUM(rate), AVG(rate), SUM(amt), AVG(amt) FROM dc
SELECT SUM(price * qty), AVG(price * qty), SUM(price * rate), MAX(price * rate), MIN(amt - price) FROM dc
SELECT SUM(price) / SUM(qty), SUM(price) / COUNT(*), SUM(price) * 2, SUM(price) + 1, SUM(price) - 0.5, MAX(price) - MIN(price) FROM dc
SELECT SUM(DISTINCT price), AVG(DISTINCT price), COUNT(DISTINCT price), SUM(DISTINCT qty), AVG(DISTINCT qty) FROM dc
SELECT id, price * 2, price + 1, price - 0.5, price / 4, price * qty, price * rate, price + qty, -price FROM dc ORDER BY id
SELECT id, qty * 2, qty / 2, qty + 0.5, qty * qty, rate * 2, rate + rate, rate / 3, amt / 3, amt * qty FROM dc ORDER BY id
SELECT id, ROUND(price), ROUND(price, 1), ROUND(price / 3, 2), ROUND(rate, 1), ROUND(amt, 2), ROUND(amt, 6), ROUND(price, -1), ROUND(qty, 2) FROM dc ORDER BY id
SELECT id, TRUNCATE(price, 0), TRUNCATE(price, 1), TRUNCATE(amt, 2), TRUNCATE(rate, 5), TRUNCATE(qty, 1), TRUNCATE(price / 3, 3) FROM dc ORDER BY id
SELECT id, ABS(-price), ABS(price - 20), ABS(-qty), CEIL(price), FLOOR(price), SIGN(price), CEIL(price / 3), FLOOR(-price) FROM dc ORDER BY id
SELECT id, MOD(price, 3), price % 3, qty % 3, MOD(amt, 0.3), MOD(-price, 3), MOD(price, 0.5) FROM dc ORDER BY id
SELECT 10 / 4, 10 / 5, 7 / 2, 1.5 * 2, 2 * 3.5, 0.1 + 0.2, 1 / 3, 100 * 1.10, 1.50 + 1.5, 3 - 0.50, 10 / 3 * 3, 2 / 3
SELECT 12 / (2 * 3), 5 DIV 2, 5 / 2, -5 / 2, 0 / 3, 0.0 * 5, 1.00 * 1.00, 1.0 / 3.00
SELECT qty, SUM(price), AVG(price), SUM(price * rate), MAX(amt) FROM dc GROUP BY qty ORDER BY qty
SELECT qty, SUM(price) FROM dc GROUP BY qty HAVING SUM(price) > 5 ORDER BY qty
SELECT SUM(price) OVER (ORDER BY id), AVG(price) OVER (ORDER BY id), SUM(qty) OVER (ORDER BY id), AVG(qty) OVER (ORDER BY id) FROM dc ORDER BY id
SELECT id, SUM(price) OVER (), AVG(price) OVER (), MAX(price) OVER (), MIN(amt) OVER () FROM dc ORDER BY id
SELECT CAST(price AS DECIMAL(10,4)), CAST(qty AS DECIMAL(10,2)), CAST(rate AS DECIMAL(10,1)), CAST(10 AS DECIMAL(5,3)), CAST(price AS SIGNED) FROM dc ORDER BY id
SELECT SUM(price), AVG(price), MAX(price), COUNT(*), SUM(qty) FROM dc WHERE id > 100
SELECT GROUP_CONCAT(price ORDER BY id), GROUP_CONCAT(rate ORDER BY id), GROUP_CONCAT(amt ORDER BY id) FROM dc
SELECT price, price * 1, price + 0, price - 0, price * 1.0, price + 0.000 FROM dc ORDER BY id
SELECT SUM(price) + SUM(rate), SUM(price) * SUM(rate), SUM(amt) / 3, SUM(price) / 3 FROM dc
SELECT qty, price, price * qty AS line FROM dc WHERE price IS NOT NULL ORDER BY line DESC
SELECT id FROM dc WHERE price * qty > 30 ORDER BY id
SELECT id, price FROM dc WHERE price / qty = 3.5 ORDER BY id
UPDATE dc SET price = price * 1.1 WHERE id = 5
SELECT id, price FROM dc WHERE id = 5
UPDATE dc SET rate = rate * 2 + 0.0005, amt = amt / 7 WHERE id = 1
SELECT id, rate, amt FROM dc WHERE id = 1
INSERT INTO dc (id, price, qty, rate, amt) VALUES (6, 3 / 2, 5, 1 / 3, 10 / 3)
SELECT id, price, rate, amt FROM dc WHERE id = 6
