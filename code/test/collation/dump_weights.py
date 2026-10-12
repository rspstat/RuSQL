"""dump_weights.py <out.json>: the utf8mb4_0900_ai_ci weights (WEIGHT_STRING) of every code point up to U+10FFFF, from a real MySQL 8.4 on 127.0.0.1:3391.

Feed the result to gen_collation_data.py. Needs pymysql.
"""
import json
import sys

import pymysql

conn = pymysql.connect(host="127.0.0.1", port=3391, user="root", password="", autocommit=True, charset="utf8mb4")
cur = conn.cursor()
cur.execute("DROP DATABASE IF EXISTS cdump")
cur.execute("CREATE DATABASE cdump DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci")
cur.execute("USE cdump")
cur.execute("CREATE TABLE w (cp INT PRIMARY KEY, ch VARCHAR(4) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci)")
rows = [(cp, chr(cp)) for cp in range(1, 0x110000) if not 0xD800 <= cp <= 0xDFFF]
for i in range(0, len(rows), 4000):
    cur.executemany("INSERT INTO w VALUES (%s, %s)", rows[i:i + 4000])
cur.execute("SELECT cp, HEX(WEIGHT_STRING(ch)) FROM w ORDER BY cp")
json.dump({cp: hx for cp, hx in cur.fetchall()}, open(sys.argv[1], "w"))
conn.close()
