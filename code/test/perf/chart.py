"""
bench.py 실행 후 생성된 result.json을 읽어 차트를 PNG로 출력합니다.
사용법: python chart.py
"""

import json
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import os

RESULT_FILE = "result.json"
OUT_DIR = "charts"
COLORS = {"rusql": "#4ec9b0", "mysql": "#e06c75"}

os.makedirs(OUT_DIR, exist_ok=True)

def save(fig, name):
    path = os.path.join(OUT_DIR, name)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved: {path}")

def bar2(ax, labels, rusql_vals, mysql_vals, ylabel, title):
    x = range(len(labels))
    w = 0.35
    ax.bar([i - w/2 for i in x], rusql_vals, w, label="RuSQL v2.2.0", color=COLORS["rusql"])
    ax.bar([i + w/2 for i in x], mysql_vals,  w, label="MySQL 8.0",     color=COLORS["mysql"])
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.legend()
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))

def main():
    with open(RESULT_FILE, encoding="utf-8") as f:
        d = json.load(f)

    print("차트 생성 중...")

    # ── 1. INSERT TPS ────────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(5, 4))
    bar2(ax,
         ["INSERT 10k rows"],
         [d["insert_tps"]["rusql"]],
         [d["insert_tps"]["mysql"]],
         "Transactions / sec",
         "INSERT TPS (auto-commit, 10,000 rows)")
    save(fig, "01_insert_tps.png")

    # ── 2. SELECT 등호 latency ───────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(6, 4))
    labels = ["SeqScan", "B-tree Index", "Hash Index"]
    r = d["select_eq"]["rusql"]
    m = d["select_eq"]["mysql"]
    bar2(ax,
         labels,
         [r["seq"], r["btree"], r["hash"]],
         [m["seq"], m["btree"], m["hash"]],
         "ms / query",
         "SELECT 등호 latency (5,000 rows)")
    save(fig, "02_select_eq.png")

    # ── 3. SELECT 범위 latency ───────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(5, 4))
    r = d["select_range"]["rusql"]
    m = d["select_range"]["mysql"]
    bar2(ax,
         ["No Index", "B-tree Index"],
         [r["no_index"], r["index"]],
         [m["no_index"], m["index"]],
         "ms / query",
         "SELECT 범위 latency — BETWEEN (5,000 rows)")
    save(fig, "03_select_range.png")

    # ── 4. 병렬 스케일링 ─────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(5, 4))
    p = d["parallel"]
    labels = ["Parallel OFF", "Parallel ON"]
    vals = [p["off"], p["on"]]
    bars = ax.bar(labels, vals, color=[COLORS["mysql"], COLORS["rusql"]])
    for bar, val in zip(bars, vals):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() * 1.01,
                f"{val:.1f} ms", ha="center", va="bottom", fontsize=9)
    speedup = p["off"] / p["on"] if p["on"] > 0 else 0
    ax.set_ylabel("ms / query")
    ax.set_title(f"RuSQL 병렬 스케일링 — GROUP BY 집계\n(speedup: {speedup:.2f}x)")
    save(fig, "04_parallel.png")

    # ── 5. 동시 접속 TPS ─────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(6, 4))
    threads = sorted(d["concurrent"].keys(), key=int)
    r_vals = [d["concurrent"][t]["rusql"] for t in threads]
    m_vals = [d["concurrent"][t]["mysql"]  for t in threads]
    ax.plot(threads, r_vals, marker="o", color=COLORS["rusql"], label="RuSQL v2.2.0", linewidth=2)
    ax.plot(threads, m_vals, marker="s", color=COLORS["mysql"],  label="MySQL 8.0",     linewidth=2)
    ax.set_xlabel("동시 접속 수 (threads)")
    ax.set_ylabel("Total TPS")
    ax.set_title("동시 접속 SELECT TPS 스케일링")
    ax.legend()
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    save(fig, "05_concurrent.png")

    print(f"\n완료: {OUT_DIR}/ 폴더에 차트 5개 저장됨.")

if __name__ == "__main__":
    main()
