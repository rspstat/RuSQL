"""dump_consistency_r84.py <rusql_server.exe> [rounds]

Is `mysqldump --single-transaction` of RuSQL one consistent picture (R84; RECOMMAND "R83 leftovers": the option was only known to run, not to be consistent)?
A writer moves 1 from a_acc to z_acc in a transaction, again and again; the dump reads a_acc, then a big table, then z_acc. A dump that is one snapshot holds a_acc.bal + z_acc.bal = 2000
whenever it is taken. Every dump is loaded into the real MySQL (127.0.0.1:3391) and the sum is read there. The same dump without --single-transaction is run as a control
(it may or may not be torn: it only shows the test can see a torn dump). Prints what went wrong; exit code 1 when a --single-transaction dump was not consistent.
"""
import os, shutil, socket, subprocess, sys, tempfile, threading, time

import pymysql

BIN = r"C:\Program Files\MySQL\MySQL Server 8.4\bin"
MYSQL, MYSQLDUMP = os.path.join(BIN, "mysql.exe"), os.path.join(BIN, "mysqldump.exe")
REAL = dict(host="127.0.0.1", port=3391, user="root", password="")


def wait_port(port, secs=20):
    for _ in range(int(secs / 0.1)):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def main():
    exe = os.path.abspath(sys.argv[1])
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    work = tempfile.mkdtemp(prefix="dump_consistency_")
    port, mport = 17992, 3495
    server = subprocess.Popen([exe, "--port", str(port), "--mysql-port", str(mport), "--data-dir", os.path.join(work, "data")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    bad = 0
    stop = threading.Event()
    done = [0]
    try:
        if not wait_port(mport):
            raise SystemExit("the RuSQL server did not start")
        rus = pymysql.connect(host="127.0.0.1", port=mport, user="root", password="root", autocommit=True, charset="utf8mb4")
        cur = rus.cursor()
        cur.execute("CREATE DATABASE cons")
        cur.execute("USE cons")
        cur.execute("CREATE TABLE a_acc (id INT PRIMARY KEY, bal INT)")
        cur.execute("CREATE TABLE z_acc (id INT PRIMARY KEY, bal INT)")
        cur.execute("CREATE TABLE big (id INT PRIMARY KEY, pad VARCHAR(60))")
        cur.execute("INSERT INTO a_acc VALUES (1, 1000)")
        cur.execute("INSERT INTO z_acc VALUES (1, 1000)")
        for start in range(0, 40000, 2000):
            cur.execute("INSERT INTO big VALUES " + ",".join("(%d,'%s')" % (i, "x" * 40) for i in range(start, start + 2000)))

        def writer():
            conn = pymysql.connect(host="127.0.0.1", port=mport, user="root", password="root", database="cons", autocommit=False, charset="utf8mb4")
            c = conn.cursor()
            while not stop.is_set():
                try:
                    c.execute("UPDATE a_acc SET bal = bal - 1 WHERE id = 1")
                    c.execute("UPDATE z_acc SET bal = bal + 1 WHERE id = 1")
                    conn.commit()
                    done[0] += 1
                except Exception:  # noqa  (a lock wait that gave up: try again)
                    try:
                        conn.rollback()
                    except Exception:  # noqa
                        pass

        threading.Thread(target=writer, daemon=True).start()
        time.sleep(0.5)
        mcur = pymysql.connect(autocommit=True, charset="utf8mb4", **REAL).cursor()
        for label, options in [("--single-transaction", ["--single-transaction"]), ("control: no --single-transaction", ["--skip-lock-tables"])]:
            torn = 0
            for n in range(rounds):
                before = done[0]
                r = subprocess.run([MYSQLDUMP, "-h127.0.0.1", f"-P{mport}", "-uroot", "-proot", "--default-character-set=utf8mb4"] + options + ["cons"], capture_output=True, text=True, encoding="utf-8", errors="replace")
                writes = done[0] - before
                if r.returncode != 0:
                    print(f"{label} #{n}: mysqldump FAILED: {r.stderr.strip()[:300]}")
                    bad += 1
                    continue
                dump = os.path.join(work, "dump.sql")
                open(dump, "w", encoding="utf-8", newline="").write(r.stdout)
                mcur.execute("DROP DATABASE IF EXISTS cons")
                mcur.execute("CREATE DATABASE cons")
                load = subprocess.run([MYSQL, "-h127.0.0.1", "-P3391", "-uroot", "--default-character-set=utf8mb4", "cons"], input=r.stdout, capture_output=True, text=True, encoding="utf-8", errors="replace")
                if load.returncode != 0:
                    print(f"{label} #{n}: the dump did not load: {load.stderr.strip()[:300]}")
                    bad += 1
                    continue
                mcur.execute("SELECT (SELECT bal FROM cons.a_acc WHERE id = 1), (SELECT bal FROM cons.z_acc WHERE id = 1), (SELECT COUNT(*) FROM cons.big)")
                a, z, big = mcur.fetchone()
                if a is None or z is None:
                    print(f"{label} #{n}: a row of an account table is missing in the dump (a_acc={a}, z_acc={z}); the INSERT lines:")
                    for line in r.stdout.splitlines():
                        if line.startswith("INSERT INTO `a_acc`") or line.startswith("INSERT INTO `z_acc`") or line.startswith("LOCK TABLES"):
                            print("    " + line[:200])
                    bad += 1
                    continue
                total = a + z
                verdict = "consistent" if total == 2000 else f"TORN (sum {total})"
                print(f"{label} #{n}: a_acc={a} z_acc={z} big={big} -> {verdict}; the writer made {writes} transfers while the dump ran")
                if total != 2000:
                    torn += 1
                    if label == "--single-transaction":
                        bad += 1
            if label != "--single-transaction":
                print(f"control: {torn} of {rounds} dumps torn (the test " + ("can" if torn else "could not this time") + " see a torn dump)")
        mcur.execute("DROP DATABASE IF EXISTS cons")
    finally:
        stop.set()
        time.sleep(0.3)
        server.kill()
        shutil.rmtree(work, ignore_errors=True)
    print("\nTOTAL PROBLEMS:", bad)
    sys.exit(1 if bad else 0)


main()
