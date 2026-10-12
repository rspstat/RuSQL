"""mysql_probe.py <file.sql>: runs each line on the real MySQL (127.0.0.1:3391, database pr) and prints the outcome."""
import datetime
import sys
import pymysql


def cell(v):
    if v is None:
        return "NULL"
    if isinstance(v, (bytes, bytearray)):
        return v.decode("utf-8", "replace")
    if isinstance(v, datetime.datetime):
        return v.strftime("%Y-%m-%d %H:%M:%S")
    return str(v)


conn = pymysql.connect(host="127.0.0.1", port=3391, user="root", password="", autocommit=True, charset="utf8mb4")
cur = conn.cursor()
cur.execute("DROP DATABASE IF EXISTS pr")
cur.execute("CREATE DATABASE pr")
cur.execute("USE pr")
for line in open(sys.argv[1], encoding="utf-8").read().splitlines():
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    try:
        cur.execute(line)
        if cur.description is None:
            print("OK    ", line)
        else:
            print("ROWS  ", line, "->", [[cell(v) for v in row] for row in cur.fetchall()])
    except Exception as e:  # noqa
        print("ERR   ", line, "->", str(e)[:110])
conn.close()
