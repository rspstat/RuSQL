# JSON values, functions and operators (R77): how a JSON value is written, JSON_VALID / TYPE / LENGTH / KEYS / DEPTH, paths in JSON_EXTRACT / -> / ->> / JSON_VALUE, JSON_SET / INSERT / REPLACE / REMOVE,
# JSON_ARRAY_APPEND / INSERT, JSON_CONTAINS / CONTAINS_PATH / OVERLAPS / SEARCH, MEMBER OF, JSON_MERGE_PATCH / PRESERVE, JSON_QUOTE / UNQUOTE, JSON_ARRAY / OBJECT, JSON_ARRAYAGG / OBJECTAGG,
# CAST AS JSON, comparing and sorting JSON values, JSON columns and the errors. A line is a statement, '--' a setup statement run on both.
--CREATE TABLE j (id INT PRIMARY KEY, doc JSON, note VARCHAR(30))
--INSERT INTO j VALUES (1, '{"a": 1, "b": [10, 20, {"c": "x"}], "d": {"e": null, "f": true}}', 'object'), (2, '[1, 2, 3, [4, 5]]', 'array'), (3, '"text"', 'string'), (4, '42', 'number'), (5, 'null', 'null'), (6, NULL, 'sql null'), (7, '{"k1": "v1", "k2": "v2", "n": {"m": {"o": 1}}}', 'nested'), (8, '3.5', 'double'), (9, 'true', 'boolean')
# how a stored value is written back
SELECT id, doc FROM j ORDER BY id
SELECT doc FROM j WHERE id = 1
SELECT JSON_ARRAY(1, 'a', NULL, TRUE, 2.5)
SELECT JSON_OBJECT('a', 1, 'b', 'x', 'c', NULL, 'd', TRUE)
SELECT JSON_OBJECT('k', JSON_ARRAY(1, 2), 'o', JSON_OBJECT('x', 1))
SELECT JSON_ARRAY()
SELECT JSON_OBJECT()
SELECT JSON_ARRAY('a"b', 'c\\d', 'e/f')
SELECT JSON_OBJECT('b', 1, 'a', 2)
SELECT JSON_OBJECT('a', 1, 'a', 2)
SELECT JSON_ARRAY(JSON_ARRAY(1, 2), JSON_OBJECT('z', 0))
SELECT CAST('{"b":1,   "a":[1,2,   3]}' AS JSON)
SELECT CAST('[1,2]' AS JSON), CAST('"x"' AS JSON), CAST('7' AS JSON), CAST('null' AS JSON)
SELECT CAST(7 AS JSON), CAST(2.5 AS JSON), CAST('abc' AS JSON)
SELECT JSON_ARRAY(1.0, 1.50, 100000000000, -3)
SELECT JSON_ARRAY('日本語', 'ñ', '😀')
SELECT JSON_OBJECT('名', '값')
# JSON_VALID / JSON_TYPE / JSON_LENGTH / JSON_DEPTH / JSON_KEYS
SELECT id, JSON_VALID(doc) FROM j ORDER BY id
SELECT JSON_VALID('{"a":1}'), JSON_VALID('{a:1}'), JSON_VALID('[1,'), JSON_VALID('1'), JSON_VALID('"x"'), JSON_VALID(''), JSON_VALID(NULL), JSON_VALID('null'), JSON_VALID('tru')
SELECT id, JSON_TYPE(doc) FROM j ORDER BY id
SELECT JSON_TYPE('1'), JSON_TYPE('1.5'), JSON_TYPE('"x"'), JSON_TYPE('true'), JSON_TYPE('null'), JSON_TYPE('[]'), JSON_TYPE('{}'), JSON_TYPE(NULL)
SELECT id, JSON_LENGTH(doc) FROM j ORDER BY id
SELECT id, JSON_LENGTH(doc, '$.b'), JSON_LENGTH(doc, '$.d') FROM j WHERE id = 1
SELECT JSON_LENGTH('[1,2,3]', '$[5]'), JSON_LENGTH('{"a":[1,2]}', '$.a'), JSON_LENGTH('{"a":[1,2]}', '$.b')
SELECT id, JSON_DEPTH(doc) FROM j ORDER BY id
SELECT JSON_DEPTH('[]'), JSON_DEPTH('[[]]'), JSON_DEPTH('{"a":{"b":1}}'), JSON_DEPTH('1'), JSON_DEPTH(NULL)
SELECT id, JSON_KEYS(doc) FROM j ORDER BY id
SELECT JSON_KEYS('{"b":1,"a":2}'), JSON_KEYS('{"a":{"x":1,"y":2}}', '$.a'), JSON_KEYS('{"a":1}', '$.a'), JSON_KEYS('{"a":1}', '$.zz')
# paths: JSON_EXTRACT, ->, ->>, JSON_VALUE, JSON_UNQUOTE
SELECT JSON_EXTRACT(doc, '$.a'), JSON_EXTRACT(doc, '$.b'), JSON_EXTRACT(doc, '$.b[1]'), JSON_EXTRACT(doc, '$.b[2].c'), JSON_EXTRACT(doc, '$.d.f'), JSON_EXTRACT(doc, '$.d.e') FROM j WHERE id = 1
SELECT JSON_EXTRACT(doc, '$.nothing'), JSON_EXTRACT(doc, '$.b[9]'), JSON_EXTRACT(doc, '$.a.x') FROM j WHERE id = 1
SELECT JSON_EXTRACT(doc, '$[0]'), JSON_EXTRACT(doc, '$[3][1]'), JSON_EXTRACT(doc, '$[7]') FROM j WHERE id = 2
SELECT JSON_EXTRACT(doc, '$.a', '$.b[0]') FROM j WHERE id = 1
SELECT JSON_EXTRACT(doc, '$.a', '$.nothing') FROM j WHERE id = 1
SELECT JSON_EXTRACT(doc, '$.nothing', '$.nothing2') FROM j WHERE id = 1
SELECT JSON_EXTRACT(doc, '$') FROM j WHERE id IN (3, 4, 5)
SELECT id, doc->'$.a', doc->>'$.a' FROM j WHERE id IN (1, 7)
SELECT id, doc->'$.k1', doc->>'$.k1', doc->'$.n.m.o' FROM j WHERE id = 7
SELECT id, doc->'$.b[2]', doc->>'$.b[2].c' FROM j WHERE id = 1
SELECT id, doc->>'$.d.e', doc->'$.d.e', doc->>'$.d.f' FROM j WHERE id = 1
SELECT id, doc->'$[1]', doc->>'$[1]' FROM j WHERE id = 2
SELECT JSON_UNQUOTE('"abc"'), JSON_UNQUOTE('abc'), JSON_UNQUOTE('"a\\"b"'), JSON_UNQUOTE('"a\\nb"'), JSON_UNQUOTE('"\\u0041"'), JSON_UNQUOTE(NULL), JSON_UNQUOTE('[1]')
SELECT JSON_UNQUOTE(JSON_EXTRACT('{"a":"x y"}', '$.a')), JSON_UNQUOTE(JSON_EXTRACT('{"a":1}', '$.a')), JSON_UNQUOTE(JSON_EXTRACT('{"a":null}', '$.a'))
SELECT JSON_VALUE('{"a":"x","n":5,"f":1.5}', '$.a'), JSON_VALUE('{"a":"x","n":5,"f":1.5}', '$.n'), JSON_VALUE('{"a":"x","n":5,"f":1.5}', '$.f'), JSON_VALUE('{"a":[1]}', '$.a'), JSON_VALUE('{"a":1}', '$.b')
SELECT JSON_VALUE('{"n":"12"}', '$.n' RETURNING SIGNED), JSON_VALUE('{"n":7}', '$.n' RETURNING DECIMAL(5,1)), JSON_VALUE('{"n":"2024-05-06"}', '$.n' RETURNING DATE)
SELECT JSON_EXTRACT('{"a b":1}', '$."a b"'), JSON_EXTRACT('[1,2,3]', '$[last]'), JSON_EXTRACT('[1,2,3]', '$[last-1]'), JSON_EXTRACT('[1,2,3]', '$[1 to 2]')
SELECT JSON_EXTRACT('[1,2,3]', '$[*]'), JSON_EXTRACT('{"a":1,"b":2}', '$.*'), JSON_EXTRACT('{"a":{"x":1},"b":{"x":2}}', '$**.x')
SELECT JSON_EXTRACT('[1,2]', 'a')
SELECT JSON_EXTRACT('{"a":1}', '$.')
SELECT JSON_EXTRACT('{"a":1}', '$[')
SELECT JSON_EXTRACT('{bad', '$.a')
SELECT JSON_EXTRACT(NULL, '$.a'), JSON_EXTRACT('{"a":1}', NULL)
# changing a document
SELECT JSON_SET('{"a":1}', '$.a', 2), JSON_SET('{"a":1}', '$.b', 2), JSON_SET('{"a":1}', '$.a', 5, '$.b', 6)
SELECT JSON_INSERT('{"a":1}', '$.a', 2), JSON_INSERT('{"a":1}', '$.b', 2), JSON_REPLACE('{"a":1}', '$.a', 2), JSON_REPLACE('{"a":1}', '$.b', 2)
SELECT JSON_SET('{"a":1}', '$.c.d', 2), JSON_SET('[1,2]', '$[1]', 9), JSON_SET('[1,2]', '$[5]', 9), JSON_SET('[1,2]', '$[0]', JSON_ARRAY(7, 8))
SELECT JSON_SET('{"a":1}', '$.s', 'text'), JSON_SET('{"a":1}', '$.n', NULL), JSON_SET('{"a":1}', '$.o', JSON_OBJECT('x', 1)), JSON_SET('{"a":1}', '$.t', TRUE)
SELECT JSON_SET(NULL, '$.a', 1), JSON_SET('{"a":1}', NULL, 1), JSON_SET('{"a":1}', '$.b', NULL)
SELECT JSON_REMOVE('{"a":1,"b":2}', '$.a'), JSON_REMOVE('[1,2,3]', '$[1]'), JSON_REMOVE('{"a":1}', '$.zz'), JSON_REMOVE('{"a":1,"b":2}', '$.a', '$.b')
SELECT JSON_REMOVE('{"a":1}', '$')
SELECT JSON_SET('{"a":1}', '$.a')
SELECT JSON_ARRAY_APPEND('[1,2]', '$', 3), JSON_ARRAY_APPEND('{"a":[1]}', '$.a', 2), JSON_ARRAY_APPEND('{"a":1}', '$.a', 2), JSON_ARRAY_APPEND('[1]', '$[0]', 'x')
SELECT JSON_ARRAY_INSERT('[1,3]', '$[1]', 2), JSON_ARRAY_INSERT('[1]', '$[5]', 9), JSON_ARRAY_INSERT('{"a":[1,2]}', '$.a[0]', 0)
SELECT JSON_MERGE_PATCH('{"a":1}', '{"b":2}'), JSON_MERGE_PATCH('{"a":1}', '{"a":2}'), JSON_MERGE_PATCH('{"a":1,"b":2}', '{"a":null}'), JSON_MERGE_PATCH('[1]', '[2]'), JSON_MERGE_PATCH('{"a":{"x":1}}', '{"a":{"y":2}}')
SELECT JSON_MERGE_PRESERVE('{"a":1}', '{"b":2}'), JSON_MERGE_PRESERVE('{"a":1}', '{"a":2}'), JSON_MERGE_PRESERVE('[1]', '[2]'), JSON_MERGE_PRESERVE('1', '2'), JSON_MERGE_PRESERVE('{"a":[1]}', '{"a":[2]}')
SELECT JSON_MERGE('{"a":1}', '{"a":2}')
SELECT JSON_QUOTE('abc'), JSON_QUOTE('a"b'), JSON_QUOTE(NULL), JSON_QUOTE('')
SELECT JSON_PRETTY('{"a":1,"b":[1,2]}')
# searching a document
SELECT JSON_CONTAINS('{"a":1,"b":2}', '1', '$.a'), JSON_CONTAINS('{"a":1,"b":2}', '3', '$.a'), JSON_CONTAINS('[1,2,3]', '2'), JSON_CONTAINS('[1,2,3]', '[1,3]'), JSON_CONTAINS('[1,2,3]', '[1,4]')
SELECT JSON_CONTAINS('{"a":1,"b":2}', '{"a":1}'), JSON_CONTAINS('{"a":1}', '{"a":1,"b":2}'), JSON_CONTAINS('{"a":[1,2]}', '{"a":[1]}'), JSON_CONTAINS('[[1,2],[3]]', '[1,2]'), JSON_CONTAINS('1', '1')
SELECT JSON_CONTAINS('{"a":"x"}', '"x"', '$.a'), JSON_CONTAINS('{"a":"x"}', 'x', '$.a'), JSON_CONTAINS(NULL, '1'), JSON_CONTAINS('[1]', NULL), JSON_CONTAINS('[1]', '1', '$.zz')
SELECT JSON_CONTAINS_PATH('{"a":1,"b":{"c":2}}', 'one', '$.a', '$.zz'), JSON_CONTAINS_PATH('{"a":1,"b":{"c":2}}', 'all', '$.a', '$.zz'), JSON_CONTAINS_PATH('{"a":1,"b":{"c":2}}', 'all', '$.a', '$.b.c')
SELECT JSON_OVERLAPS('[1,2,3]', '[3,4]'), JSON_OVERLAPS('[1,2]', '[3,4]'), JSON_OVERLAPS('{"a":1}', '{"a":1,"b":2}'), JSON_OVERLAPS('1', '[1,2]'), JSON_OVERLAPS('1', '2')
SELECT 3 MEMBER OF('[1,2,3]'), 4 MEMBER OF('[1,2,3]'), 'a' MEMBER OF('["a","b"]'), 1 MEMBER OF('1'), 2 MEMBER OF('[[1,2]]')
SELECT JSON_SEARCH('{"a":"x","b":["y","x"]}', 'one', 'x'), JSON_SEARCH('{"a":"x","b":["y","x"]}', 'all', 'x'), JSON_SEARCH('{"a":"x"}', 'one', 'zz'), JSON_SEARCH('{"a":"abc"}', 'one', 'a%')
# columns of a table
SELECT id FROM j WHERE JSON_EXTRACT(doc, '$.a') = 1
SELECT id FROM j WHERE doc->'$.a' = 1
SELECT id FROM j WHERE doc->>'$.k1' = 'v1'
SELECT id FROM j WHERE JSON_CONTAINS(doc, '1', '$.a')
SELECT id FROM j WHERE doc = CAST('42' AS JSON)
SELECT id FROM j WHERE doc = '42'
SELECT id FROM j WHERE doc IS NULL
SELECT id FROM j WHERE JSON_TYPE(doc) = 'ARRAY'
SELECT id, doc->'$.a' AS a FROM j WHERE id IN (1, 7) ORDER BY a, id
SELECT id, JSON_LENGTH(doc) AS n FROM j ORDER BY n DESC, id
SELECT JSON_TYPE(doc), COUNT(*) FROM j WHERE doc IS NOT NULL AND id <> 5 GROUP BY JSON_TYPE(doc) ORDER BY 1
SELECT id, JSON_EXTRACT(doc, '$[0]') FROM j WHERE JSON_TYPE(doc) = 'ARRAY'
SELECT COUNT(*) FROM j WHERE doc->'$.d.f' = TRUE
SELECT doc->'$.d.f' = TRUE, doc->'$.d.f' = 'true', doc->'$.d.f' = CAST('true' AS JSON) FROM j WHERE id = 1
SELECT JSON_EXTRACT(doc, '$.a') + 1, JSON_EXTRACT(doc, '$.a') * 2, JSON_EXTRACT(doc, '$.b[0]') / 4 FROM j WHERE id = 1
SELECT CONCAT('v=', doc->>'$.k1'), UPPER(doc->>'$.k2'), LENGTH(doc->>'$.k1') FROM j WHERE id = 7
SELECT CONCAT('v=', doc->'$.k1') FROM j WHERE id = 7
# writing JSON columns
--UPDATE j SET doc = JSON_SET(doc, '$.a', 100) WHERE id = 1
SELECT doc FROM j WHERE id = 1
--UPDATE j SET doc = JSON_REMOVE(doc, '$.d') WHERE id = 1
SELECT doc FROM j WHERE id = 1
--UPDATE j SET doc = JSON_INSERT(doc, '$.new', JSON_ARRAY(1, 2)) WHERE id = 7
SELECT doc FROM j WHERE id = 7
--UPDATE j SET doc = JSON_ARRAY_APPEND(doc, '$', 6) WHERE id = 2
SELECT doc FROM j WHERE id = 2
INSERT INTO j VALUES (20, '{bad', 'x')
INSERT INTO j VALUES (21, '', 'x')
INSERT INTO j VALUES (22, '[1, 2', 'x')
INSERT INTO j VALUES (23, 'abc', 'x')
INSERT INTO j VALUES (24, '{"a": 1}', 'ok')
UPDATE j SET doc = 'nope' WHERE id = 24
SELECT doc FROM j WHERE id = 24
INSERT INTO j VALUES (26, JSON_OBJECT('q', 1), 'object into json')
SELECT id, doc FROM j WHERE id = 26
INSERT INTO j VALUES (27, '  {"a":   1}  ', 'spaces')
SELECT doc FROM j WHERE id = 27
INSERT INTO j VALUES (28, '{"b":1,"a":2,"b":3}', 'duplicate and unordered keys')
SELECT doc FROM j WHERE id = 28
INSERT INTO j VALUES (29, '{"big": 12345678901234567890, "neg": -0.5, "exp": 1e3, "u": "\\u00e9"}', 'numbers')
SELECT doc FROM j WHERE id = 29
SELECT doc->'$.big', doc->'$.neg', doc->'$.exp', doc->>'$.u' FROM j WHERE id = 29
# aggregates
SELECT JSON_ARRAYAGG(id) FROM j WHERE id <= 3
SELECT JSON_ARRAYAGG(note) FROM j WHERE id <= 3
SELECT JSON_ARRAYAGG(doc) FROM j WHERE id IN (2, 4)
SELECT JSON_OBJECTAGG(note, id) FROM j WHERE id <= 3
SELECT JSON_OBJECTAGG(id, doc) FROM j WHERE id IN (2, 4)
SELECT JSON_ARRAYAGG(id) FROM j WHERE id > 100
SELECT JSON_OBJECTAGG(id, note) FROM j WHERE id > 100
SELECT JSON_ARRAYAGG(NULL), JSON_ARRAYAGG(id) FROM j WHERE id = 1
SELECT JSON_OBJECTAGG(k, v) FROM (SELECT 'x' k, 1 v UNION ALL SELECT 'y', 2 UNION ALL SELECT 'x', 3) q
SELECT JSON_ARRAYAGG(v), JSON_OBJECTAGG(k, v) FROM (SELECT 'b' k, 1 v UNION ALL SELECT 'a', NULL UNION ALL SELECT 'c', 3) q
SELECT g, JSON_ARRAYAGG(v), JSON_OBJECTAGG(k, v) FROM (SELECT 1 g, 'a' k, 1 v UNION ALL SELECT 1, 'b', NULL UNION ALL SELECT 2, 'c', 3 UNION ALL SELECT 1, 'a', 9) q GROUP BY g ORDER BY g
SELECT JSON_OBJECTAGG(k, v) FROM (SELECT NULL k, 1 v) q
SELECT JSON_ARRAYAGG(CAST(v AS JSON)), JSON_ARRAYAGG(JSON_OBJECT('n', CAST(v AS JSON))) FROM (SELECT '[1]' v UNION ALL SELECT '{"z": 2}') q
# errors
SELECT JSON_OBJECT('a')
SELECT JSON_OBJECT(NULL, 1)
SELECT JSON_ARRAY(1,)
SELECT JSON_TYPE('{bad')
SELECT JSON_LENGTH('{bad')
SELECT JSON_KEYS('{bad')
SELECT JSON_SET('{bad', '$.a', 1)
SELECT JSON_CONTAINS('{bad', '1')
SELECT JSON_EXTRACT('1', '$**.a.b')
SELECT 'abc' MEMBER OF('{bad')
SELECT CAST('{bad' AS JSON)
SELECT JSON_UNQUOTE('"unterminated')
# numbers: how MySQL writes them (an integer as written, a fraction as the shortest DOUBLE, 1e15 and over with an exponent), and what an SQL number becomes in JSON_ARRAY
SELECT CAST('[1e3, 1e15, 1e16, 1e17, 1e20, 1e-5, 1e-7, 1.5e-10, 123456789.123, 0.1, 100.0, -0.0, 0.0, 1E2, 1.0, 1.50, 12.5e1]' AS JSON)
SELECT CAST('[9007199254740993, 18446744073709551615, 18446744073709551616, -9223372036854775808, -9223372036854775809, 0.30000000000000004, 3.14159265358979323846]' AS JSON)
SELECT JSON_ARRAY(CAST(5 AS UNSIGNED), -1, 9223372036854775807, 9223372036854775808, 18446744073709551615, 18446744073709551616)
SELECT JSON_ARRAY(TRUE, FALSE, NULL)
SELECT JSON_ARRAY(1.0, 1.50, 100000000000, -3, 0.1, CAST(1.50 AS DECIMAL(5,2)))
SELECT JSON_ARRAY(CAST(1.50 AS DECIMAL(5,2)), 5 DIV 2, ROUND(2.567, 1))
SELECT JSON_ARRAY(1.5, .5)
SELECT JSON_OBJECT('a', TRUE), JSON_SET('{}', '$.a', TRUE), JSON_ARRAY('true'), JSON_ARRAY('1'), JSON_ARRAY('[1]'), JSON_ARRAY(CAST('[1]' AS JSON)), JSON_ARRAY(JSON_ARRAY(1))
SELECT JSON_OBJECT('b', 1, 'aa', 2, 'a', 3, 'B', 4, 'é', 5, 'z', 6)
SELECT JSON_KEYS('{"b":1,"aa":2,"a":3,"B":4}'), JSON_EXTRACT('{"a":1,"a":2}', '$.a')
SELECT JSON_OBJECT('a', JSON_OBJECT('b', 1), 'c', JSON_ARRAY(1, JSON_OBJECT('d', NULL)))
SELECT JSON_OBJECT('a', '{"b":1}'), JSON_OBJECT('a', CAST('{"b":1}' AS JSON)), JSON_OBJECT(1, 'a'), JSON_OBJECT(1.5, 'a'), JSON_OBJECT('a', 'é', 'b', '"')
SELECT CAST('"\\u00e9\\ud83d\\ude00 \\/ \\b \\u0001 é"' AS JSON)
# strings: JSON_QUOTE, JSON_UNQUOTE
SELECT JSON_QUOTE('é'), JSON_QUOTE('a"b\\c'), JSON_QUOTE('[1]')
SELECT JSON_QUOTE(1)
SELECT JSON_UNQUOTE('"\\u00e9\\ud83d\\ude00"'), JSON_UNQUOTE('"a\\/b"'), JSON_UNQUOTE('"x\\ty"'), JSON_UNQUOTE('"abc'), JSON_UNQUOTE('abc"'), JSON_UNQUOTE(''), JSON_UNQUOTE('"'), JSON_UNQUOTE('""'), JSON_UNQUOTE('{"a":1}')
SELECT JSON_UNQUOTE('"\\q"')
SELECT JSON_UNQUOTE('"a"b"')
# types
SELECT JSON_TYPE(JSON_EXTRACT('[1.5]', '$[0]')), JSON_TYPE(JSON_EXTRACT('[1]', '$[0]')), JSON_TYPE(CAST('18446744073709551615' AS JSON)), JSON_TYPE(JSON_EXTRACT('[true]', '$[0]')), JSON_TYPE('1'), JSON_TYPE('[1]')
# paths: last, ranges, wildcards, quoted keys, several paths, the errors
SELECT JSON_EXTRACT('[1,2,3]', '$[last]'), JSON_EXTRACT('[1,2,3]', '$[last-1]'), JSON_EXTRACT('[1,2,3]', '$[last-5]'), JSON_EXTRACT('[1,2,3]', '$[0 to 1]'), JSON_EXTRACT('[1,2,3]', '$[1 to last]'), JSON_EXTRACT('[1,2,3]', '$[ 1 ]'), JSON_EXTRACT('[1,2,3]', '$[last - 1]'), JSON_EXTRACT('[1,2,3]', '$[0 to 9]')
SELECT JSON_EXTRACT('5', '$[*]'), JSON_EXTRACT('5', '$[last]'), JSON_EXTRACT('{"a b":1}', '$."a b"'), JSON_EXTRACT('{"c-d":2}', '$."c-d"'), JSON_EXTRACT('{"é":4}', '$.é')
SELECT JSON_EXTRACT('{"a":{"b":1}}', '$.a.b', '$.a'), JSON_EXTRACT('{"a":1}', '$.b', '$.c'), JSON_EXTRACT('{"a":[]}', '$.a[*]')
SELECT JSON_EXTRACT('[[1,2],[3]]', '$[*][0]'), JSON_EXTRACT('[[1,2],[3]]', '$[*][*]'), JSON_EXTRACT('{"a":{"a":{"a":1}}}', '$**.a'), JSON_EXTRACT('{"a":1}', '$**.a'), JSON_EXTRACT('[{"a":1},{"a":2}]', '$[*].a'), JSON_EXTRACT('[{"a":1},{"a":2}]', '$[*].a', '$[0]')
SELECT JSON_EXTRACT('{"a":[1,2],"b":{"a":3}}', '$**.a'), JSON_EXTRACT('{"a":[1,2],"b":{"a":3}}', '$.a[*]'), JSON_EXTRACT('{"a":[1,2],"b":{"a":3}}', '$.*'), JSON_EXTRACT('{"a":[1,2]}', '$.*'), JSON_EXTRACT('[1,2]', '$**[0]')
SELECT JSON_EXTRACT('{"a":1}', '  $.a'), JSON_EXTRACT('{"a":1}', '$ .a'), JSON_EXTRACT('{"a":1}', '$.a ')
SELECT JSON_EXTRACT('{"a b":1}', '$.a b')
SELECT JSON_EXTRACT('{"1x":3}', '$.1x')
SELECT JSON_EXTRACT('{"a":1}', '$.a.')
SELECT JSON_EXTRACT('{"a":1}', '$..a')
SELECT JSON_EXTRACT('[1,2]', '$[-1]')
SELECT JSON_EXTRACT('[1,2]', '$[a]')
SELECT JSON_EXTRACT('[1,2]', '$[]')
SELECT JSON_EXTRACT('[1,2]', '$[0')
SELECT JSON_EXTRACT('[1,2]', '$**')
SELECT JSON_EXTRACT('[1,2]', '$[0]x')
SELECT JSON_EXTRACT('[1,2,3]', '$[2 to 0]')
SELECT JSON_SET('[1,2]', '$[*]', 1)
SELECT JSON_SET('{"a":1}', '$.*', 1)
SELECT JSON_SET('{"a":1}', '$**.a', 1)
SELECT JSON_SET('[1,2]', '$[0 to 1]', 1)
SELECT JSON_SET('[1,2,3]', '$[last+1]', 9)
SELECT JSON_REMOVE('{"a":1}', '$')
SELECT JSON_REMOVE('[1,2,3]', '$[*]')
SELECT JSON_KEYS('{"a":{"b":1}}', '$.*')
SELECT JSON_CONTAINS('{"a":1}', '1', '$.*')
SELECT JSON_ARRAY_APPEND('{"a":[1]}', '$.*', 2)
# changing a document: positions, missing paths, a value that is no array, the root
SELECT JSON_REMOVE('[1,2,3]', '$[last]'), JSON_SET('[1,2,3]', '$[last]', 9), JSON_REMOVE('[1,2,3]', '$[last-1]'), JSON_REMOVE('{"a":1}', '$.a.b')
SELECT JSON_REMOVE('{"a":1}', '$[0]'), JSON_REMOVE('[1]', '$.a'), JSON_REMOVE('[1,2]', '$[5]')
SELECT JSON_SET('{"a":1}', '$[0]', 9), JSON_SET('{"a":1}', '$[1]', 9), JSON_SET('5', '$[1]', 9), JSON_SET('5', '$.a', 9), JSON_SET('[1]', '$.a', 9)
SELECT JSON_INSERT('[1]', '$[3]', 9), JSON_INSERT('[1,2]', '$[0]', 9), JSON_REPLACE('[1,2]', '$[3]', 9), JSON_SET('{"a":{"b":1}}', '$.a.c.d', 1), JSON_SET('{"a":[1]}', '$.a[1]', 2)
SELECT JSON_SET('{"a":1}', '$', 5), JSON_INSERT('{"a":1}', '$', 5), JSON_REPLACE('{"a":1}', '$', 5), JSON_SET('{"a":1}', '$', JSON_ARRAY(1))
SELECT JSON_SET('{"a":1}', '$.b', 'x', '$.b', 'y', '$.c', 1)
SELECT JSON_REMOVE(NULL, '$.a'), JSON_REMOVE('{"a":1}', NULL), JSON_INSERT(NULL, '$.a', 1), JSON_REPLACE('{"a":1}', NULL, 1)
SELECT JSON_SET('{"a":1}', '$.b', 1, NULL, 2)
SELECT JSON_INSERT('{"a":1}', '$.b', 1, '$.c')
SELECT JSON_ARRAY_APPEND('[1]', '$', JSON_ARRAY(2)), JSON_ARRAY_APPEND('[1]', '$', '[2]'), JSON_ARRAY_APPEND('{"a":1}', '$', 2), JSON_ARRAY_APPEND('1', '$', 2), JSON_ARRAY_APPEND('[1]', '$[5]', 2), JSON_ARRAY_APPEND('[1]', '$', NULL), JSON_ARRAY_APPEND('[1]', '$', 2, '$', 3)
SELECT JSON_ARRAY_APPEND('[1]', '$[0]', 2), JSON_ARRAY_APPEND('[[1]]', '$[0]', 2), JSON_ARRAY_APPEND(NULL, '$', 1), JSON_ARRAY_APPEND('[1]', NULL, 1), JSON_ARRAY_APPEND('{"a":[1],"b":[2]}', '$.a', 9, '$.b', 8), JSON_ARRAY_APPEND('[1,2]', '$[last]', 9)
SELECT JSON_ARRAY_APPEND('[1]', '$')
SELECT JSON_ARRAY_INSERT('[1,2]', '$[1]', 'a'), JSON_ARRAY_INSERT('[1,2]', '$[2]', 'a'), JSON_ARRAY_INSERT('[1,2]', '$[9]', 'a'), JSON_ARRAY_INSERT('{"a":[1]}', '$.a[0]', 0), JSON_ARRAY_INSERT('1', '$[0]', 'a'), JSON_ARRAY_INSERT('{"a":1}', '$[0]', 'a'), JSON_ARRAY_INSERT('[1]', '$.a[0]', 'a'), JSON_ARRAY_INSERT(NULL, '$[0]', 'a'), JSON_ARRAY_INSERT('[1]', '$[0]', 2, '$[0]', 3), JSON_ARRAY_INSERT('[1,2]', '$[last]', 'a')
SELECT JSON_ARRAY_INSERT('{"a":1}', '$.a', 2)
SELECT JSON_ARRAY_INSERT('[1]', '$', 2)
# merging
SELECT JSON_MERGE_PATCH('{"a":1}', 'null'), JSON_MERGE_PATCH('1', '{"a":1}'), JSON_MERGE_PATCH('{"a":1}', '[1]'), JSON_MERGE_PATCH('{"a":{"b":1}}', '{"a":null}'), JSON_MERGE_PATCH('{"a":1}', '{"b":null}'), JSON_MERGE_PATCH('{"a":1}', '{"a":1}', '{"c":3}'), JSON_MERGE_PATCH('{"a":1,"b":2}', '{"a":null}', '{"b":null}'), JSON_MERGE_PATCH('{"a":[1]}', '{"a":[2]}'), JSON_MERGE_PATCH('[1]', '{"a":1}'), JSON_MERGE_PATCH('"x"', '{"a":null}'), JSON_MERGE_PATCH('{"a":1}', '{"a":{"b":null}}')
SELECT JSON_MERGE_PATCH('{"a":1}', NULL), JSON_MERGE_PATCH(NULL, '{"a":1}'), JSON_MERGE_PATCH(NULL, '[1]')
SELECT JSON_MERGE_PATCH('{"a":1}')
SELECT JSON_MERGE_PRESERVE('{"a":1}', '[1]'), JSON_MERGE_PRESERVE('"a"', '"b"'), JSON_MERGE_PRESERVE('[1]', '{"a":1}'), JSON_MERGE_PRESERVE('{"a":{"b":1}}', '{"a":{"c":2}}'), JSON_MERGE_PRESERVE('[1,2]', '[3]', '[4]'), JSON_MERGE_PRESERVE('{"a":[1]}', '{"a":2}'), JSON_MERGE_PRESERVE('{"a":1}', '{"a":2}', '{"a":3}'), JSON_MERGE_PRESERVE('1', '[2]'), JSON_MERGE_PRESERVE('null', '1')
SELECT JSON_MERGE_PRESERVE('{"a":1}', NULL), JSON_MERGE_PRESERVE(NULL, '[1]')
SELECT JSON_MERGE_PRESERVE('{"a":1}')
# searching
SELECT JSON_CONTAINS_PATH('{"a":1}', 'ONE', '$.a'), JSON_CONTAINS_PATH('{"a":1}', 'All', '$.a'), JSON_CONTAINS_PATH('{"a":1}', 'all', '$.a', NULL), JSON_CONTAINS_PATH('{"a":1}', NULL, '$.a'), JSON_CONTAINS_PATH('{"a":1}', 'one', '$.zz', '$.a'), JSON_CONTAINS_PATH('{"a":1}', 'one', '$.a', NULL), JSON_CONTAINS_PATH(NULL, 'one', '$.a'), JSON_CONTAINS_PATH('{"a":[1,2]}', 'one', '$.a[*]'), JSON_CONTAINS_PATH('{"a":[1,2]}', 'one', '$.a[5]')
SELECT JSON_CONTAINS_PATH('{"a":1}', 'x', '$.a')
SELECT JSON_SEARCH('{"a":"abc","b":["abd","abc"]}', 'one', 'ab_'), JSON_SEARCH('{"a":"abc","b":["abd","abc"]}', 'all', 'ab%'), JSON_SEARCH('{"a":"A"}', 'one', 'a'), JSON_SEARCH('{"a":"a%"}', 'one', 'a|%', '|'), JSON_SEARCH('{"a":"x","b":{"c":"x"}}', 'all', 'x', NULL, '$.b')
SELECT JSON_SEARCH('{"a":1}', 'one', '1'), JSON_SEARCH('["x"]', 'one', 'x'), JSON_SEARCH('{"a":"x"}', 'ONE', 'x'), JSON_SEARCH('{"a":"xyz"}', 'one', 'x%z'), JSON_SEARCH('{"a":"x"}', 'one', NULL), JSON_SEARCH('{"a":["x","y"],"b":"x"}', 'all', 'x', NULL, '$.a'), JSON_SEARCH('{"a b":"x"}', 'one', 'x'), JSON_SEARCH('{"a":{"b":["x"]}}', 'one', 'x'), JSON_SEARCH('[["x"]]', 'one', 'x')
SELECT JSON_SEARCH('{"a":"x"}', 'x', 'x')
SELECT JSON_DEPTH('{}'), JSON_DEPTH('[[1]]'), JSON_DEPTH('[1,[2,[3]]]'), JSON_DEPTH('{"a":[]}'), JSON_DEPTH('"x"'), JSON_DEPTH('null'), JSON_DEPTH(CAST(NULL AS JSON))
SELECT JSON_OVERLAPS('{"a":1,"b":2}', '{"a":1}'), JSON_OVERLAPS('[1,2]', '[2,3]'), JSON_OVERLAPS('1', '1'), JSON_OVERLAPS('[1]', '1'), JSON_OVERLAPS('{"a":1}', '[1]'), JSON_OVERLAPS('{"a":[1]}', '{"a":[1]}'), JSON_OVERLAPS('[1,[2]]', '[2]'), JSON_OVERLAPS('{"a":1}', '{"a":2}'), JSON_OVERLAPS('[]', '[]'), JSON_OVERLAPS('"a"', '["a"]'), JSON_OVERLAPS(NULL, '1'), JSON_OVERLAPS('[1,{"a":1}]', '{"a":1}')
SELECT 1 MEMBER OF('[1,2]'), '1' MEMBER OF('[1,2]'), 1.0 MEMBER OF('[1,2]'), NULL MEMBER OF('[1]'), 1 MEMBER OF(NULL), 'a' MEMBER OF('"a"'), JSON_ARRAY(1) MEMBER OF('[[1]]'), CAST('[1]' AS JSON) MEMBER OF('[[1]]'), 1 MEMBER OF ('[1]')
SELECT JSON_CONTAINS('{"a":[1,2,3]}', '[1,3]', '$.a'), JSON_CONTAINS('[1,2]', '[]'), JSON_CONTAINS('{"a":1}', '{}'), JSON_CONTAINS('[[1,2]]', '[1,2]'), JSON_CONTAINS('[1,[2,3]]', '2'), JSON_CONTAINS('1', '1.0'), JSON_CONTAINS('[1.0]', '1'), JSON_CONTAINS('"a"', '"a"'), JSON_CONTAINS('[true]', 'true'), JSON_CONTAINS('[true]', '1'), JSON_CONTAINS('{"a":{"b":1,"c":2}}', '{"a":{"b":1}}')
SELECT JSON_LENGTH('[1,[2,3]]'), JSON_LENGTH('{"a":1}', '$.*'), JSON_LENGTH(NULL), JSON_LENGTH('[1,2]', NULL), JSON_KEYS('[1]'), JSON_KEYS('{}'), JSON_KEYS('"a"')
SELECT JSON_PRETTY('[1,{"a":[1,2],"b":{}}, []]'), JSON_PRETTY('1'), JSON_PRETTY('{"a":"x"}'), JSON_PRETTY(NULL), JSON_PRETTY('{"b":[],"a":{},"c":[{"x":1}]}'), JSON_PRETTY('"x"'), JSON_PRETTY('[]')
# JSON_VALUE: the value as SQL sees it
SELECT JSON_VALUE('{"a":null}', '$.a'), JSON_VALUE('{"a":null}', '$.a') IS NULL, JSON_VALUE('{"a":"x"}', '$.a'), JSON_VALUE('{"a":{"b":1}}', '$.a'), JSON_VALUE('{"a":[1]}', '$.a'), JSON_VALUE('{"a":true}', '$.a'), JSON_VALUE('{"a":1.50}', '$.a'), JSON_VALUE('{"a":1}', '$.*')
SELECT JSON_VALUE('{"a":"12"}', '$.a' RETURNING SIGNED), JSON_VALUE('{"a":"x"}', '$.a' RETURNING SIGNED), JSON_VALUE('{"a":7}', '$.a' RETURNING DECIMAL(5,1)), JSON_VALUE('{"a":"2024-05-06"}', '$.a' RETURNING DATE), JSON_VALUE('{"a":"hello"}', '$.a' RETURNING CHAR(3)), JSON_VALUE('{"a":1}', '$.b' RETURNING SIGNED)
SELECT JSON_EXTRACT('{"a":null}', '$.a'), JSON_EXTRACT('{"a":null}', '$.a') IS NULL, JSON_EXTRACT('{"a":null}', '$.b') IS NULL
# comparing JSON values: by type (null < numbers < strings < objects < arrays < booleans), a string against a JSON string, a number against a JSON number
SELECT JSON_ARRAY('a', 1) = CAST('["a", 1]' AS JSON), CAST('{"a":1}' AS JSON) = CAST('{"a":1.0}' AS JSON), CAST('[1,2]' AS JSON) = CAST('[1,2]' AS JSON), CAST('[1,2]' AS JSON) <> CAST('[2,1]' AS JSON)
SELECT CAST('"a"' AS JSON) < CAST('"b"' AS JSON), CAST('1' AS JSON) < CAST('"a"' AS JSON), CAST('[1]' AS JSON) < CAST('{"a":1}' AS JSON), CAST('true' AS JSON) > CAST('5' AS JSON), CAST('null' AS JSON) < CAST('1' AS JSON)
SELECT CAST('1' AS JSON) = 1.0, CAST('1.5' AS JSON) = 1.5, CAST('1.5' AS JSON) = '1.5', CAST('"a"' AS JSON) = 'a', CAST('"10"' AS JSON) > 9, CAST('[1]' AS JSON) = 1, CAST('null' AS JSON) = NULL, CAST('null' AS JSON) <=> CAST('null' AS JSON)
SELECT CAST('"x"' AS JSON) = 'x', CAST('"x"' AS JSON) = '"x"', CAST('1' AS JSON) = 1, CAST('1' AS JSON) = '1', CAST('1.0' AS JSON) = 1, CAST('true' AS JSON) = TRUE, CAST('true' AS JSON) = 'true', CAST('[1]' AS JSON) = '[1]', CAST('[1]' AS JSON) = CAST('[1]' AS JSON)
SELECT JSON_EXTRACT('{"a":"x"}', '$.a') = 'x', JSON_EXTRACT('{"a":1}', '$.a') = 1, JSON_EXTRACT('{"a":true}', '$.a') = TRUE, JSON_EXTRACT('{"a":1}', '$.a') > 0, JSON_EXTRACT('{"a":"b"}', '$.a') > 'a', JSON_EXTRACT('{"a":10}', '$.a') > 9, JSON_EXTRACT('{"a":"10"}', '$.a') > 9
SELECT id FROM j WHERE doc->'$.k1' = 'v1'
SELECT id FROM j WHERE doc->'$.a' = 1
SELECT id FROM j WHERE doc->'$.d.f' = FALSE
SELECT id FROM j WHERE doc = CAST('"text"' AS JSON)
SELECT id FROM j WHERE doc = 'text'
SELECT id FROM j WHERE doc > 40 ORDER BY id
SELECT id FROM j WHERE doc IN (CAST('42' AS JSON), CAST('"text"' AS JSON)) ORDER BY id
SELECT id FROM j WHERE id IN (2, 3, 4, 5, 6, 8, 9) ORDER BY doc, id
SELECT id FROM j WHERE id IN (2, 3, 4, 5, 6, 8, 9) ORDER BY doc DESC, id
SELECT id, JSON_TYPE(doc) FROM j WHERE id IN (2, 3, 4, 8, 9) ORDER BY doc, id
SELECT MAX(doc), MIN(doc) FROM j WHERE id IN (3, 4, 8)
# the errors of a document that is no JSON
SELECT CAST('[1,2' AS JSON)
SELECT CAST('' AS JSON)
SELECT CAST(' 5 ' AS JSON)
SELECT CAST('01' AS JSON)
SELECT CAST('1.' AS JSON)
SELECT CAST('.5' AS JSON)
SELECT CAST('+1' AS JSON)
SELECT CAST('-' AS JSON)
SELECT CAST('NaN' AS JSON)
SELECT CAST('[1,]' AS JSON)
SELECT CAST('{"a":1,}' AS JSON)
SELECT CAST('TRUE' AS JSON)
SELECT JSON_VALID('{"a":1,}'), JSON_VALID('01'), JSON_VALID(' 5 '), JSON_VALID('1.'), JSON_VALID('TRUE'), JSON_VALID('1e5'), JSON_VALID('-0'), JSON_VALID(CAST('[1]' AS JSON)), JSON_VALID(JSON_ARRAY(1))
# a comma before the closing parenthesis is a syntax error, in every function
SELECT CONCAT('a',)
SELECT COALESCE(1,)
SELECT UPPER('a',)
SELECT JSON_ARRAY(,1)
SELECT JSON_OBJECT('a',1,)
