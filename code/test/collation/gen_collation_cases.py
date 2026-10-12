"""gen_collation_cases.py <out.inc> [pairs] [seed]

Writes code/backend/tests/collation_cases.inc: pairs of texts and the answer of a real MySQL 8.4 (STRCMP, which follows the default collation utf8mb4_0900_ai_ci).
The texts are made of characters from many scripts, with and without case, accents, spaces and punctuation, of different lengths. Needs pymysql and a MySQL
on 127.0.0.1:3391.
"""
import os
import random
import re
import sys

import pymysql

out_path = sys.argv[1]
pairs = int(sys.argv[2]) if len(sys.argv) > 2 else 6000
seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1

POOL = (
    list("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
    + list(" _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$")
    + list("àáâãäåçèéêëìíîïñòóôõöùúûüýÿÀÉÖÜÑçÇ")
    + ["ß", "æ", "Æ", "œ", "Œ", "ø", "Ø", "đ", "ł", "Ł", "ı", "İ", "þ", "ð"]
    + list("αβγδεζηθικλμνξοπρσςτυφχψωΑΒΓΔΩά")
    + list("абвгдеёжзийклмнопрстуфхцчшщъыьэюяАБВ")
    + list("가각간갈나다라마바사아자차카타파하한글강남")
    + list("漢字日本語中文国")
    + list("ひヒかカあア")
    + list("ａＡｂＢ１２")
    + ["😀", "😁", "©", "€", "™"]
    + ["é", "ä", "́"]
    + ["\x01", "\t", "\n"]
)

rng = random.Random(seed)


def word():
    n = rng.choice([0, 1, 1, 2, 2, 3, 3, 4, 5, 6, 8])
    return "".join(rng.choice(POOL) for _ in range(n))


def variant(s):
    """The same text changed the way a collation ignores (case) or notices (a letter, a space, the end)."""
    kind = rng.randrange(6)
    if kind == 0:
        return s.upper()
    if kind == 1:
        return s.lower()
    if kind == 2:
        return s + rng.choice(POOL)
    if kind == 3:
        return s + " "
    if kind == 4 and s:
        i = rng.randrange(len(s))
        return s[:i] + rng.choice(POOL) + s[i + 1:]
    return s[:-1] if s else s


cases = []
for _ in range(pairs):
    a = word()
    b = variant(a) if rng.random() < 0.5 else word()
    cases.append((a, b))
# every single character of the pool against a few others
for ch in POOL:
    cases.append((ch, rng.choice(POOL)))
    cases.append((ch, ch.upper()))
    cases.append((ch + "x", ch))

# the edges of the runs of the engine's own table (where an off-by-one would hide): the first code point after a run that no run covers (it weighs by the rule for
# unassigned ones) against the last code point of the run before it and against a letter, and the last two code points of some runs that count up
data = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "backend", "core", "src", "collation_data.inc"), encoding="utf-8").read()
runs = [(int(m.group(1), 16), int(m.group(2)), int(m.group(3))) for m in re.finditer(r"\{0x([0-9A-F]+),(\d+),(\d),\d+,\d+\}", data)]
starts = {r[0] for r in runs}


def valid(cp):
    return 1 <= cp <= 0x10FFFF and not 0xD800 <= cp <= 0xDFFF


for start, count, kind in runs:
    gap = start + count
    if gap not in starts and valid(gap):
        cases.append((chr(gap), chr(gap - 1)))
        cases.append((chr(gap), "a"))
        cases.append((chr(gap), "漢"))  # (an unassigned code point weighs more than any ideograph of the first block)
for k, (start, count, kind) in enumerate(runs):
    if count > 1 and kind == 1 and k % 8 == 0 and valid(start + count - 1):
        cases.append((chr(start + count - 2), chr(start + count - 1)))
# Hangul syllables (they weigh what their jamo weigh) next to each other and a whole vowel / final consonant row apart
for s in range(0, 11172, 37):
    cases.append((chr(0xAC00 + s), chr(0xAC00 + s + 1) if s + 1 < 11172 else "a"))
    cases.append((chr(0xAC00 + s), chr(0xAC00 + (s + 588) % 11172)))
    cases.append((chr(0xAC00 + s) + "x", chr(0xAC00 + s)))
    jamo = chr(0x1100 + s // 588) + chr(0x1161 + (s % 588) // 28) + (chr(0x11A7 + s % 28) if s % 28 else "")
    cases.append((chr(0xAC00 + s), jamo))  # (a syllable is its jamo)
    cases.append((chr(0xAC00 + s), jamo[:-1] if len(jamo) == 3 else jamo + "x"))
# ideographs and code points nobody assigned (their weights come from the code point), at the edges of their blocks
cases.append(("漢", chr(0x9FFF)))  # (the weights of an ideograph are made of its block and the low 15 bits: 0x7FFF and 0x8000 are on two sides of a block step)
cases.append((chr(0x7FFF), chr(0x8000)))
cases.append((chr(0x20000), chr(0x2A6DF)))
for cp in [0x377, 0x378, 0x379, 0x5FF, 0x3400, 0x4DBF, 0x4DC0, 0x4E00, 0x7FFF, 0x8000, 0x9FFF, 0xA000, 0xFA0E, 0xFA0F, 0xFA10, 0xFA11, 0xFA12, 0xFA13, 0xFA14, 0xFA1F, 0xFA20, 0xFA21,
           0xFA23, 0xFA24, 0xFA27, 0xFA28, 0xFA29, 0xFA2A, 0xFFFD, 0x1F600, 0x20000, 0x2A6DF, 0x2A6E0, 0x2A700, 0x2EBEF, 0x2EBF0, 0x30000, 0xE0001, 0x10FFFF]:
    cases.append((chr(cp), chr(cp + 1) if cp < 0x10FFFF else "a"))
    cases.append((chr(cp), "a"))
    cases.append((chr(cp) + "a", chr(cp) + "b"))

conn = pymysql.connect(host="127.0.0.1", port=3391, user="root", password="", autocommit=True, charset="utf8mb4")
cur = conn.cursor()
cur.execute("DROP DATABASE IF EXISTS ccases")
cur.execute("CREATE DATABASE ccases DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci")
cur.execute("USE ccases")
cur.execute("CREATE TABLE p (id INT PRIMARY KEY, a VARCHAR(40), b VARCHAR(40))")
for i in range(0, len(cases), 2000):
    cur.executemany("INSERT INTO p VALUES (%s, %s, %s)", [(i + k, a, b) for k, (a, b) in enumerate(cases[i:i + 2000])])
cur.execute("SELECT id, STRCMP(a, b) FROM p ORDER BY id")
answers = dict(cur.fetchall())
conn.close()


def c_string(text):
    out = []
    escaped = False
    for byte in text.encode("utf-8"):
        ch = chr(byte)
        if byte == 0x22:
            out.append('\\"')
            escaped = False
        elif byte == 0x5C:
            out.append("\\\\")
            escaped = False
        elif 0x20 <= byte < 0x7F and not (escaped and ch.isdigit()):
            out.append(ch)
            escaped = False
        else:
            out.append("\\%03o" % byte)  # (a digit that follows an octal escape is written as one too: the compiler warns about it)
            escaped = True
    return '"' + "".join(out) + '"'


with open(out_path, "w", encoding="utf-8", newline="\n") as f:
    f.write("// generated by code/test/collation/gen_collation_cases.py from the STRCMP of a real MySQL 8.4 (utf8mb4_0900_ai_ci): {a, b, STRCMP(a, b)}\n")
    for i, (a, b) in enumerate(cases):
        f.write("    {%s, %s, %d},\n" % (c_string(a), c_string(b), answers[i]))
print("cases:", len(cases), "equal:", sum(1 for v in answers.values() if v == 0))
