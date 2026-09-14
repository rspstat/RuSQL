"""Generates the NL->SQL fine-tuning dataset.

Usage: python generate.py [--out-dir OUT_DIR] [--seed N]

For every (schema, table, intent) combination, instantiates a concrete SQL query and
expands it into one training row per NL phrasing (Korean + English). Training/
validation rows come only from schemas.TRAIN_DOMAINS; the test split is built from
schemas.HELD_OUT_DOMAINS (schemas never seen in training), to measure generalization
to unseen schemas rather than memorization.

Output: train.jsonl / val.jsonl / test.jsonl, each line
    {"domain": str, "lang": "ko"|"en", "schema": <CREATE TABLE DDL text>,
     "question": str, "sql": str}
"""

import argparse
import json
import random
from pathlib import Path

from schemas import SCHEMAS, TRAIN_DOMAINS, HELD_OUT_DOMAINS
from templates import INTENTS


def build_rows(domains: list[str], rng: random.Random) -> list[dict]:
    rows = []
    for domain in domains:
        schema = SCHEMAS[domain]
        ddl = schema.create_table_sql()
        for table in schema.tables:
            for intent in INTENTS:
                inst = intent(schema, table, rng)
                if inst is None:
                    continue
                for q in inst.ko:
                    rows.append({"domain": domain, "lang": "ko", "schema": ddl, "question": q, "sql": inst.sql})
                for q in inst.en:
                    rows.append({"domain": domain, "lang": "en", "schema": ddl, "question": q, "sql": inst.sql})
    return rows


def write_jsonl(path: Path, rows: list[dict]) -> None:
    with path.open("w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default="dataset")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--val-fraction", type=float, default=0.05)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    train_domains = list(TRAIN_DOMAINS)
    all_rows = build_rows(train_domains, rng)
    rng.shuffle(all_rows)

    n_val = int(len(all_rows) * args.val_fraction)
    val_rows = all_rows[:n_val]
    train_rows = all_rows[n_val:]
    test_rows = build_rows(list(HELD_OUT_DOMAINS), rng)

    write_jsonl(out_dir / "train.jsonl", train_rows)
    write_jsonl(out_dir / "val.jsonl", val_rows)
    write_jsonl(out_dir / "test.jsonl", test_rows)

    print(f"train: {len(train_rows)} rows ({len(train_domains)} domains)")
    print(f"val:   {len(val_rows)} rows")
    print(f"test:  {len(test_rows)} rows ({len(HELD_OUT_DOMAINS)} held-out domains: {sorted(HELD_OUT_DOMAINS)})")


if __name__ == "__main__":
    main()
